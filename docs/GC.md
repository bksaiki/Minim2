# Minim | Garbage Collector Design

This document captures the design of Minim's allocator, and garbage collector.

The design is a deliberate slim-down of Chez Scheme's runtime (referenced as `c/alloc.c`, `c/gc.c`, `c/segment.c`, and `boot/ta6le/scheme.h` from the user's local Chez checkout under `~/reference/ChezScheme/`). What we keep: 3-bit pointer tagging, bump-pointer allocation, forwarding-pointer copying GC. What we drop: generational machinery, parallel sweepers, mark-in-place hybrid, write barriers, card tables, dirty-segment lists, chunk pools, locked/immobile objects, stack maps, weak/ephemeron pairs.

## Scope

**Types supported in v1**: fixnum, pair, flonum, symbol, vector, plus three immediate constants (`#t`, `#f`, `'()`). Anything else — closures, strings, bignums, characters, records, ports — is out of scope for now and reserved in the tag space.

**Concurrency**: single-threaded. No locks, no atomics.

**Platform**: 64-bit POSIX (Linux, macOS). `mmap`-backed heap. Big-endian and 32-bit hosts are out of scope.

## Pointer tagging

The runtime's value type is `mobj`, a `uintptr_t`-sized opaque word. Every value is exactly one word. The bottom 3 bits of an `mobj` are the **primary type tag** (`primary_type_bits = 3` in Chez parlance). Heap objects are 16-byte aligned, leaving 4 low bits unused on heap pointers and 3 of them used as tag.

| bits  | hex  | name              | encoding                                              |
|------:|-----:|-------------------|-------------------------------------------------------|
| `000` | 0x0  | `MTAG_FIXNUM`     | value `<< 3`; tag-zero ⇒ free `+`/`-` arithmetic       |
| `001` | 0x1  | `MTAG_PAIR`       | heap base \| 1; 16-byte object                         |
| `010` | 0x2  | `MTAG_FLONUM`     | heap base \| 2; 16-byte object (header + double)       |
| `011` | 0x3  | `MTAG_SYMBOL`     | heap base \| 3; 16-byte object (header + name ptr)     |
| `100` | 0x4  | (reserved)        | unused; future closure or string                       |
| `101` | 0x5  | (reserved)        | unused; future closure or string                       |
| `110` | 0x6  | `MTAG_IMMEDIATE`  | constants: `#f=0x06`, `#t=0x0E`, `()=0x26`, `eof=0x36` |
| `111` | 0x7  | `MTAG_TYPED_OBJ`  | heap base \| 7; first heap word is a secondary tag     |

### Why this layout

- **Fixnum tag = 0** so untagged adds and subtracts of two fixnums produce a fixnum directly (`(a*8) + (b*8) = (a+b)*8`). No tag manipulation, no untag-then-retag. Range is `[-2^60, 2^60)` (61-bit signed integers) on 64-bit hosts.
- **Pair tag = 1** because pairs are by far the most common heap object in Scheme; a 1-bit tag is the smallest cost per `cons`.
- **Flonum, symbol given dedicated tags** so common predicates (`flonum?`, `symbol?`) are a one-instruction tag check, not a two-step "tag 7 then secondary".
- **Tag 7 (typed object) is the catch-all** — anything that doesn't justify a primary tag goes here, and the secondary type lives in the first word of the heap object. Vectors are the only inhabitant in v1.
- **Tag 6 (immediate) collects all the heap-less constants** — `#t`, `#f`, `'()`, `eof`, and (later) characters, void. They are full-word constants whose bits never collide with a heap pointer.
- **16-byte alignment** gives us a fourth low bit that we deliberately don't use for tagging — it stays zero on every heap pointer, which lets us spot-check pointer validity in debug builds.

### Immediate values

```c
#define Mfalse  ((mobj)0x06)   // 0000 0110
#define Mtrue   ((mobj)0x0E)   // 0000 1110
#define Mnull   ((mobj)0x26)   // 0010 0110
#define Meof    ((mobj)0x36)   // 0011 0110
```

Predicate tricks worth keeping:
- `(x & 0xF7) == 0x06` matches both `#t` and `#f` ⇒ one-instruction `boolean?` (the mask clears bit 3, the only bit that distinguishes them; `()` and `eof` set higher bits and so don't collide).
- `x == Mnull`, `x == Meof` are exact equality — each has a unique bit pattern.

### Forward marker

`MFORWARD_MARKER = 0x3E` — written into the first word of a heap object during GC to signal "I've already been moved." The second word then holds the new typed pointer. Same value as Chez's, chosen so that `(MFORWARD_MARKER & 0x7) = 0x6 = MTAG_IMMEDIATE` — it can't be confused with a valid heap pointer's tag, and its full 8-byte value (`0x3E` in low byte, zero elsewhere) doesn't collide with any valid object header.

## Object layouts

All heap allocations are 16-byte aligned. All sizes are multiples of 16.

### Pair (16 bytes)

```
offset 0..7 : car
offset 8..15: cdr
typed pointer = base | 1
```

No header word. During GC, `forward()` clobbers `car` with `MFORWARD_MARKER` and `cdr` with the new typed pointer. Both fields are pointer-sized and traced.

Accessors are written in C as:
```c
#define MINIM_CAR(p) (((mobj *)((uintptr_t)(p) - 1))[0])
#define MINIM_CDR(p) (((mobj *)((uintptr_t)(p) - 1))[1])
```

We **do not** copy Chez's "displacement absorbs the tag" trick (where `pair_car_disp = 7` and `Scar(p) = *(p + 7)` to skip an explicit untag). In hand-written assembly that saves an instruction; in C, the compiler folds either form to one `mov reg, [base+disp]`. Untagging explicitly reads better.

### Flonum (16 bytes)

```
offset 0..7 : header (type tag in normal use; MFORWARD_MARKER during copy)
offset 8..15: IEEE 754 double
typed pointer = base | 2
```

The header word costs 8 bytes per flonum, but it lets us use the same forwarding scheme as everything else. Chez avoids this overhead with a per-segment `forwarded_flonums` bitmap (`gc.c:533`); we don't, because the simplification savings dwarf the per-flonum 8 bytes for our purposes.

### Symbol (16 bytes)

```
offset 0..7 : header (type tag word; MFORWARD_MARKER during copy)
offset 8..15: char *name (pointer to malloc'd C string)
typed pointer = base | 3
```

Symbol names are stored as `malloc`'d C strings *outside* the GC heap. Lifetime is tied to the symbol's residency in the intern table, which is "forever" in v1 (we never delete interned symbols). When a symbol is forwarded, the name pointer is preserved verbatim — `malloc`'d memory is not GC-managed.

The intern table itself lives outside the GC heap (`malloc`'d hash table). Each table slot that holds an `mobj` symbol is a global root.

### Vector (8 + 8·n bytes, padded up to 16)

```
offset 0..7        : type word — (length << 4) | MSEC_VECTOR
offset 8..(8+8n-1) : payload slots
typed pointer = base | 7
```

`MSEC_VECTOR = 0x0` is the secondary tag in the type word's low 4 bits. The length lives in the upper 60 bits. Maximum vector length is `2^60` (well past anything practical).

The type word is the secondary header, but it doubles as the forwarding-marker slot during GC: a forwarded vector has `MFORWARD_MARKER` in offset 0..7, and the new typed pointer in offset 8..15. Note that the *original* slot 0 of the vector payload (which would normally hold the first element) gets clobbered as part of forwarding — but that's fine because once a vector is forwarded, no one will read it again from the from-space.

Empty vectors (`length == 0`) still get a 16-byte allocation: 8 bytes of type word + 8 bytes of padding. Future optimization: a global `empty_vector` constant like Chez's `S_G.null_vector`.

## Heap layout

Two equal-sized semispaces, each a single contiguous `mmap`'d block. State (in `src/gc.c`):

```c
struct minim_heap {
    char *from_base, *from_end;   // current alloc region
    char *to_base,   *to_end;     // reserve region
    char *ap;                     // bump pointer (alloc next)
    char *scan;                   // Cheney scan pointer (during GC only)
    size_t space_bytes;           // size of each semispace
};
```

### Allocation fast path

```c
char *gc_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;     // round up to 16
    if (heap.ap + n > heap.from_end) gc_collect(n);
    char *p = heap.ap;
    heap.ap += n;
    return p;
}
```

Constructors apply the appropriate tag and write the object header:

```c
mobj minim_cons(mobj car, mobj cdr) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT2(car, cdr);
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = car;
    ((mobj *)p)[1] = cdr;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_PAIR);
}
```

Note that `car` and `cdr` are protected *before* `gc_alloc` because the alloc may trigger a GC, and the C-side argument values would otherwise be stale. The address-of-local form (`&car`, `&cdr`) is essential — the GC rewrites those slots in place.

## Cheney semispace GC

Standard Cheney's algorithm. The scan pointer chases the alloc pointer through to-space; when they meet, the collection is done.

### `gc_collect(size_t need)`

```
1. Swap from-space and to-space (rename).
2. Reset ap = to_base; scan = to_base.
3. For each root in {shadow stack, global roots, intern table slots}:
       forward(root_addr);
4. While scan < ap:
       size = object_size_at(scan);
       for each pointer field f in object at scan:
           forward(f);
       scan += size;
5. If ap + need > to_end:
       grow heap (double space_bytes), allocate new pair of spaces,
       copy live data forward again from the just-finished to-space.
6. Done. From-space is now garbage; ap is the new high-water mark.
```

### `forward(mobj *root)`

```c
void forward(mobj *root) {
    mobj v = *root;
    if (is_immediate_or_fixnum(v)) return;

    char *base = (char *)((uintptr_t)v & ~(uintptr_t)0x7);   // untag
    mobj first = ((mobj *)base)[0];

    if (first == MFORWARD_MARKER) {
        *root = ((mobj *)base)[1];   // already moved; install the new ptr
        return;
    }

    size_t sz = object_size(v);
    char *new_base = heap.ap;
    memcpy(new_base, base, sz);
    heap.ap += sz;

    mobj new_typed = (mobj)((uintptr_t)new_base | (v & 0x7));
    ((mobj *)base)[0] = MFORWARD_MARKER;
    ((mobj *)base)[1] = new_typed;
    *root = new_typed;
}
```

### `object_size(mobj v)` dispatch

```
tag == MTAG_PAIR    → 16
tag == MTAG_FLONUM  → 16
tag == MTAG_SYMBOL  → 16
tag == MTAG_TYPED_OBJ:
    header = ((mobj *)untag(v))[0];
    secondary = header & 0xF;
    if secondary == MSEC_VECTOR:
        length = header >> 4;
        return align16(8 + 8 * length);
```

### Per-object pointer-field traversal in the scan loop

The scan loop reads the *new* (to-space) copy of each object and forwards each pointer field:

```
PAIR:
    forward(&car); forward(&cdr);
FLONUM:
    no pointer fields (header + double).
SYMBOL:
    no pointer fields (header + char* outside the GC heap).
TYPED_OBJ + MSEC_VECTOR:
    for i in 0..length-1:
        forward(&slots[i]);
```

Symbols hold a `char *` to a `malloc`'d string; that pointer is *not* forwarded — it points outside the GC heap and is preserved verbatim.

### Heap growth

When `gc_collect` finishes and `ap + need > to_end`, double `space_bytes`, `mmap` a new pair of semispaces, copy the live data from the current to-space into the new from-space (a second Cheney pass over the just-collected data, with the old to-space as from-space), `munmap` the old pair, and return.

We always grow proactively rather than ever fail an allocation. There is no max heap size in v1; OOM behavior is whatever `mmap` returns.

## Roots: shadow stack + globals

### Shadow stack

```c
extern mobj **minim_shadow_stack;     // array of mobj*
extern size_t minim_ssp;              // current top
extern size_t minim_ssp_capacity;
```

C runtime code that holds a Scheme value across a possible GC point (i.e., across a `gc_alloc` or anything that transitively allocates) must push the variable's address onto the shadow stack. The macros, in `include/minim.h`:

```c
#define MINIM_GC_FRAME_BEGIN   size_t __minim_save_ssp = minim_ssp
#define MINIM_GC_PROTECT(v)    (minim_shadow_stack[minim_ssp++] = (mobj *)&(v))
#define MINIM_GC_PROTECT2(a,b) do { MINIM_GC_PROTECT(a); MINIM_GC_PROTECT(b); } while (0)
#define MINIM_GC_PROTECT3(a,b,c) do { MINIM_GC_PROTECT(a); MINIM_GC_PROTECT(b); MINIM_GC_PROTECT(c); } while (0)
#define MINIM_GC_FRAME_END     (minim_ssp = __minim_save_ssp)
#define MINIM_GC_RETURN(x)     do { mobj __r = (x); MINIM_GC_FRAME_END; return __r; } while (0)
```

The save/restore via `__minim_save_ssp` is robust to early returns and error paths: every exit path either falls through `MINIM_GC_FRAME_END` or uses `MINIM_GC_RETURN`. The shadow stack reallocates on overflow.

### Global roots

A small array of `mobj **` registered at runtime startup with `minim_protect(&x)`. Used for:
- The intern table's bucket array (each slot holds a possibly-forwarded symbol pointer).
- Any built-in or user-registered global Scheme constants.

Treated identically to shadow-stack entries during GC — `gc_collect` walks both arrays and forwards each one.

### Stress mode

`#ifdef MINIM_GC_STRESS` — every `gc_alloc` triggers a full `gc_collect` *before* bumping. Latent missing-protect bugs become deterministic crashes on the line that's wrong. Enabled via the CMake option `MINIM_GC_STRESS=ON`. The cost is enormous (every cons triggers full heap traversal), so it's a debug-only mode.

## Symbol intern table

Lives outside the GC heap. Hash table keyed by `(name, length)`, mapping to `mobj` symbol pointer.

- **Backing storage**: `malloc`'d. Open-addressing or chained — implementer's choice; we'll use chaining for simplicity.
- **Symbol name strings**: also `malloc`'d, owned by the symbol object via its `char *name` field. Lifetime = forever in v1.
- **GC interaction**: every bucket pointer (the `mobj` symbol) is a global root. On collection, `forward(&bucket[i])` updates each bucket to point at the symbol's new heap address.
- **`minim_intern(const char *s)`**: hash, search bucket for an existing match, return it; otherwise allocate a new symbol heap object with the name copied via `strdup`, install it in the bucket, return it.

## File structure

```
include/
  minim.h              # public: types, predicates, accessors,
                       # constructors, GC protect macros

src/
  types.h              # internal: layout offsets, header packing, MFORWARD_MARKER
  gc.h                 # internal: gc_init, gc_alloc, gc_collect, minim_protect
  alloc.c              # bump alloc + constructors:
                       # minim_cons, minim_make_vector, minim_make_flonum
  gc.c                 # Cheney: forward, scan, gc_collect, heap-grow
  symbol.c             # intern table + minim_make_symbol + minim_intern
  main.c               # smoke-test driver

tests/
  test_gc.c            # standalone executable: cons/vector/circular/intern survival

docs/agents/
  design.md            # this document
  TODO.md              # work-breakdown checklist
```

`CMakeLists.txt` updates:
- Add `src/gc.c` and `src/symbol.c` to the `minim` target.
- Add `option(MINIM_GC_STRESS "Trigger GC on every allocation" OFF)` and propagate via `target_compile_definitions`.
- Add `enable_testing()` and a `minim_test_gc` executable target linking the same sources plus `tests/test_gc.c`.
- Add `add_test(NAME gc COMMAND minim_test_gc)`.

## Verification

End-to-end:

1. **Normal build** — `cmake -B build && cmake --build build && ./build/minim` prints the banner; allocator smoke test runs without crashing.
2. **Stress build** — `cmake -B build-stress -DMINIM_GC_STRESS=ON && cmake --build build-stress && ctest --test-dir build-stress` forces a GC on every allocation; the test suite still passes. This is the strongest guarantee that protect annotations are correct.
3. **GDB sanity check** — break in `gc_collect`, run a test, watch a pair's address change post-collection while `MINIM_CAR` still returns the right value.

Test cases in `tests/test_gc.c`:
- Cons survives: build a long list, force `gc_collect`, walk it again, verify contents.
- Vector survives: same pattern with element pointers (mix of fixnums and pairs).
- Circular list: `set-car!` a pair to itself; `gc_collect` must terminate (forwarding marker prevents infinite recursion).
- Intern identity: `minim_intern("foo") == minim_intern("foo")` before and after `gc_collect`.

## Out of scope for v1

These are deliberately deferred and noted here so future work doesn't redesign the same surface:

- **Closures, strings, bignums, ratnums, chars, ports, records.** Tag space (4 and 5) and secondary types under `MTAG_TYPED_OBJ` are reserved for them.
- **Generational collection.** Single-generation Cheney is enough until allocation rate or heap size makes minor GCs worth it.
- **Write barriers, card tables, dirty lists, remembered sets.** Not needed without generations.
- **Mark-in-place / locked / immobile objects.** Not needed without conservative roots.
- **Conservative C-stack scan.** We chose explicit shadow stack precisely to avoid this.
- **Multithreading.** No locks; data structures are single-threaded.
- **JIT / W^X / code segments.** No compiled code path in v1.
- **Tail calls / first-class continuations.** Runtime support deferred until eval/apply lands.

## References

Quoting the user's local Chez checkout at `~/reference/ChezScheme/`:

- `c/alloc.c:609` — `Scons` (the model for `minim_cons`).
- `c/alloc.c:704` — `S_vector` (the model for `minim_make_vector`).
- `c/alloc.c:330` — `S_reset_allocation_pointer` (segment alloc; we degenerate this to a single semispace).
- `c/gc.c:566–679` — `relocate_pure` / `relocate_impure` macros (the model for `forward`).
- `c/gc.c:34–95` — header comment on copy vs mark; we drop the mark path.
- `c/gc.c:803–853` — `copy_stack` (we have no stacks to copy in v1).
- `c/segment.c` — chunk-and-segment allocator we replace with a single mmap pair.
- `boot/ta6le/scheme.h:75–116` — Chez's predicates; ours mirror them.
- `boot/ta6le/equates.h` — Chez's tag and offset constants; ours are renamed but structurally identical.
