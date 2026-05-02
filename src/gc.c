#include "minim.h"
#include "gc.h"

#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Shadow stack */
mobj **minim_shadow_stack = NULL;
size_t minim_ssp = 0;
size_t minim_ssp_capacity = 0;

#define SHADOW_STACK_INIT_CAP 256

void minim_shadow_stack_grow(void) {
    size_t new_cap = minim_ssp_capacity == 0
        ? SHADOW_STACK_INIT_CAP
        : minim_ssp_capacity * 2;
    mobj **ns = realloc(minim_shadow_stack, new_cap * sizeof(mobj *));
    if (!ns) { fprintf(stderr, "minim: shadow stack OOM\n"); abort(); }
    minim_shadow_stack = ns;
    minim_ssp_capacity = new_cap;
}

/* Global roots */
static mobj **global_roots = NULL;
static size_t global_roots_n = 0;
static size_t global_roots_cap = 0;

void minim_protect(mobj *slot) {
    if (global_roots_n == global_roots_cap) {
        size_t nc = global_roots_cap == 0 ? 16 : global_roots_cap * 2;
        mobj **ng = realloc(global_roots, nc * sizeof(mobj *));
        if (!ng) { fprintf(stderr, "minim: global roots OOM\n"); abort(); }
        global_roots = ng;
        global_roots_cap = nc;
    }
    global_roots[global_roots_n++] = slot;
}

/* Heap */
static struct minim_heap {
    char *from_base, *from_end;
    char *to_base, *to_end;
    char *ap;
    char *scan;
    size_t space_bytes;
} heap;

static char *heap_mmap(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "minim: mmap failed\n"); abort(); }
    return (char *)p;
}

void gc_init(size_t initial_bytes) {
    /* Round up to page boundary (4 KiB) */
    size_t sz = (initial_bytes + 4095) & ~(size_t)4095;
    heap.space_bytes = sz;
    heap.from_base = heap_mmap(sz);
    heap.from_end = heap.from_base + sz;
    heap.to_base = heap_mmap(sz);
    heap.to_end = heap.to_base + sz;
    heap.ap = heap.from_base;
    heap.scan = heap.from_base;

    /* Shadow stack */
    minim_shadow_stack_grow();
}

void gc_shutdown(void) {
    munmap(heap.from_base, heap.space_bytes);
    munmap(heap.to_base, heap.space_bytes);
    free(minim_shadow_stack);
    free(global_roots);
    minim_shadow_stack = NULL;
    minim_ssp = 0;
    minim_ssp_capacity = 0;
    global_roots = NULL;
    global_roots_n = 0;
    global_roots_cap = 0;
}

/* -----------------------------------------------------------------------
 * Forward / object_size  (used by gc_collect)
 * --------------------------------------------------------------------- */

/* Raw base pointer of a tagged value (untagged), valid only for heap tags */
static inline char *minim_untag(mobj v) {
    return (char *)((uintptr_t)v & ~(uintptr_t)MTAG_MASK);
}

static size_t object_size(mobj v) {
    mobj tag = v & MTAG_MASK;
    switch (tag) {
    case MTAG_PAIR: return MINIM_PAIR_SIZE;
    case MTAG_FLONUM: return MINIM_FLONUM_SIZE;
    case MTAG_SYMBOL: return MINIM_SYMBOL_SIZE;
    case MTAG_CLOSURE: return MINIM_CLOSURE_SIZE;
    case MTAG_TYPED_OBJ: {
        /* Every typed-object kind shares the same shape:
         *   header = (slot_count << 4) | secondary_tag
         * followed by `slot_count` machine-word slots. The byte size
         * is the same formula regardless of secondary tag, so we just
         * read the slot count and reuse the vector helper. */
        mobj header = *(mobj *)minim_untag(v);
        size_t slots = (size_t)(header >> 4);
        return minim_vector_size(slots);
    }
    default:
        fprintf(stderr, "minim: gc: object_size on non-heap tag %lx\n",
                (unsigned long)tag);
        abort();
    }
}

/* -----------------------------------------------------------------------
 * gc_collect
 * --------------------------------------------------------------------- */

/* Forward declaration — scan_fields and forward_tagged are mutually
 * referenced (scan walks each object's slots via forward_tagged). */
static void forward_tagged(mobj *root, char *to_base);

/* Typed scan: given a to-space pointer (base address + original tag),
 * forward all pointer fields inside it. New objects copied as a result
 * land in to-space and must have their tags recorded for the scan loop,
 * so we route through forward_tagged rather than the bare forward(). */
static void scan_fields(char *base, mobj tag, char *to_base) {
    switch (tag) {
    case MTAG_PAIR: {
        mobj *slots = (mobj *)base;
        forward_tagged(&slots[0], to_base);
        forward_tagged(&slots[1], to_base);
        break;
    }
    case MTAG_FLONUM:
        /* header + double — no pointer fields */
        break;
    case MTAG_SYMBOL:
        /* header + char* (outside GC heap) — not forwarded */
        break;
    case MTAG_CLOSURE: {
        /* Four fixed mobj slots: params, body, env, name. */
        mobj *slots = (mobj *)base;
        forward_tagged(&slots[0], to_base);
        forward_tagged(&slots[1], to_base);
        forward_tagged(&slots[2], to_base);
        forward_tagged(&slots[3], to_base);
        break;
    }
    case MTAG_TYPED_OBJ: {
        /* Uniform trace: forward every payload slot. The secondary tag
         * dictates *interpretation* (which slot means what), but the
         * GC only needs to know that every slot is either an mobj
         * (forwarded) or a leaf-tagged raw word (no-op via
         * minim_is_leaf inside forward_tagged). MSEC_PRIM's fnptr slot
         * is a function pointer cast to mobj; on every supported
         * platform it is at least 8-byte aligned, so its low 3 bits
         * read as MTAG_FIXNUM and forward_tagged short-circuits as a
         * leaf — the bytes are preserved verbatim across collections. */
        mobj header = ((mobj *)base)[0];
        size_t slots = (size_t)(header >> 4);
        mobj *payload = (mobj *)base + 1;
        for (size_t i = 0; i < slots; i++)
            forward_tagged(&payload[i], to_base);
        break;
    }
    default:
        fprintf(stderr, "minim: scan_fields: unknown tag %lx\n",
                (unsigned long)tag);
        abort();
    }
}

/* Per-to-space-object tag storage. One entry per 16-byte slot. */
static mobj *scan_tags = NULL;
static size_t scan_tags_cap = 0; /* in units of MINIM_ALIGN_BYTES slots */

static void scan_tags_ensure(size_t space_bytes) {
    size_t slots = space_bytes / MINIM_ALIGN_BYTES;
    if (slots > scan_tags_cap) {
        free(scan_tags);
        scan_tags = malloc(slots * sizeof(mobj));
        if (!scan_tags) { fprintf(stderr, "minim: scan tags OOM\n"); abort(); }
        scan_tags_cap = slots;
    }
}

/* Record the tag for a newly copied object at obj_base. */
static void scan_tag_set(char *base_start, char *obj_base, mobj tag) {
    size_t idx = (size_t)(obj_base - base_start) / MINIM_ALIGN_BYTES;
    scan_tags[idx] = tag;
}

static mobj scan_tag_get(char *base_start, char *obj_base) {
    size_t idx = (size_t)(obj_base - base_start) / MINIM_ALIGN_BYTES;
    return scan_tags[idx];
}

/* True for "leaf" values that the GC never traces: fixnums and immediates.
 * A fixnum has tag 0; an immediate has tag 6 (MTAG_IMMEDIATE). */
static inline int minim_is_leaf(mobj v) {
    mobj tag = v & MTAG_MASK;
    return tag == MTAG_FIXNUM || tag == MTAG_IMMEDIATE;
}

/* forward_tagged: copy + record tag */
static void forward_tagged(mobj *root, char *to_base) {
    mobj v = *root;
    if (minim_is_leaf(v)) return;

    char *base = minim_untag(v);
    mobj first = ((mobj *)base)[0];

    if (first == MFORWARD_MARKER) {
        *root = ((mobj *)base)[1];
        return;
    }

    mobj tag = v & MTAG_MASK;
    size_t sz = object_size(v);
    char *new_base = heap.ap;
    memcpy(new_base, base, sz);
    heap.ap += sz;

    mobj new_typed = (mobj)((uintptr_t)new_base | tag);
    ((mobj *)base)[0] = MFORWARD_MARKER;
    ((mobj *)base)[1] = new_typed;
    *root = new_typed;

    scan_tag_set(to_base, new_base, tag);
}

/* One Cheney collection pass: swap spaces, copy live roots into the new
 * from-space (current to-space at entry), scan, done. Does not grow.
 * Caller must ensure scan_tags is sized for the larger of the two
 * semispaces before calling. */
static void do_collect(void) {
    /* 1. Swap spaces */
    char *tmp_base = heap.from_base;
    char *tmp_end = heap.from_end;
    heap.from_base = heap.to_base;
    heap.from_end = heap.to_end;
    heap.to_base = tmp_base;
    heap.to_end = tmp_end;

    /* 2. Reset alloc / scan to start of new from-space */
    heap.ap = heap.from_base;
    heap.scan = heap.from_base;

    /* 3. Forward all roots */
    for (size_t i = 0; i < minim_ssp; i++)
        forward_tagged(minim_shadow_stack[i], heap.from_base);
    for (size_t i = 0; i < global_roots_n; i++)
        forward_tagged(global_roots[i], heap.from_base);

    /* 4. Scan loop */
    while (heap.scan < heap.ap) {
        mobj tag = scan_tag_get(heap.from_base, heap.scan);
        size_t sz = 0;
        switch (tag) {
        case MTAG_PAIR:    sz = MINIM_PAIR_SIZE; break;
        case MTAG_FLONUM:  sz = MINIM_FLONUM_SIZE; break;
        case MTAG_SYMBOL:  sz = MINIM_SYMBOL_SIZE; break;
        case MTAG_CLOSURE: sz = MINIM_CLOSURE_SIZE; break;
        case MTAG_TYPED_OBJ: {
            mobj header = ((mobj *)heap.scan)[0];
            size_t length = (size_t)(header >> 4);
            sz = minim_vector_size(length);
            break;
        }
        default:
            fprintf(stderr, "minim: gc scan: bad tag %lx\n", (unsigned long)tag);
            abort();
        }
        scan_fields(heap.scan, tag, heap.from_base);
        heap.scan += sz;
    }
}

void gc_collect(size_t need) {
    scan_tags_ensure(heap.space_bytes);
    do_collect();

    /* Grow if the post-collect heap still can't fit `need`. We allocate
     * a bigger to-space, then run a second collection that compacts live
     * data out of the just-collected from-space into the new bigger
     * from-space. Repeat until `need` fits. */
    while (heap.ap + need > heap.from_end) {
        size_t old_sz = heap.space_bytes;
        size_t new_sz = old_sz * 2;
        size_t live = (size_t)(heap.ap - heap.from_base);
        while (new_sz < live + need) new_sz *= 2;

        /* Replace the (garbage) to-space with a bigger fresh one. */
        munmap(heap.to_base, old_sz);
        heap.to_base = heap_mmap(new_sz);
        heap.to_end = heap.to_base + new_sz;
        scan_tags_ensure(new_sz);

        /* Run another collection: live data flows from the smaller
         * from-space into the bigger to-space. After do_collect the
         * smaller space has become to-space again. */
        do_collect();

        /* Replace the now-undersized to-space with one matching new_sz. */
        munmap(heap.to_base, old_sz);
        heap.to_base = heap_mmap(new_sz);
        heap.to_end = heap.to_base + new_sz;
        heap.space_bytes = new_sz;
    }
}

char *gc_alloc(size_t n) {
    n = MINIM_ALIGN(n);
#ifdef MINIM_GC_STRESS
    gc_collect(n);
#endif
    if (heap.ap + n > heap.from_end)
        gc_collect(n);
    if (heap.ap + n > heap.from_end) {
        fprintf(stderr, "minim: gc_alloc: out of memory after GC\n");
        abort();
    }
    char *p = heap.ap;
    heap.ap += n;
    return p;
}
