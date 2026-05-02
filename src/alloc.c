#include "minim.h"
#include "gc.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*-----------------------------------------------------------------------
 * Pair: tag=0x1, 2 slots [car, cdr]
 * -------------------------------------------------------------------- */

mobj Mcons(mobj car, mobj cdr) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT2(car, cdr);
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = car;
    ((mobj *)p)[1] = cdr;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_PAIR);
}

/*-----------------------------------------------------------------------
 * Flonum: tag=0x2, 2 slots [header, value]
 * -------------------------------------------------------------------- */

mobj Mflonum(double d) {
    MINIM_GC_FRAME_BEGIN;
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = MTAG_FLONUM; /* header */
    memcpy(p + 8, &d, sizeof(double));
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_FLONUM);
}

/* ----------------------------------------------------------------------
 * Generic typed-object allocation: tag=0x7
 *
 * Every typed-object kind below shares the same on-heap shape:
 * header word `(slot_count << 4) | sec` followed by `slot_count`
 * word-sized payload slots. The GC trace every slot uniformly;
 * per-kind interpretation is handled by the accessors in include/minim.h.
 * -------------------------------------------------------------------- */

static mobj typed_alloc(size_t slot_count, mobj sec) {
    char *p = gc_alloc(minim_vector_size(slot_count));
    mobj header = ((mobj)slot_count << 4) | sec;
    ((mobj *)p)[0] = header;
    return (mobj)((uintptr_t)p | MTAG_TYPED_OBJ);
}

static mobj typed_alloc_fill(size_t slot_count, mobj sec, mobj fill) {
    char *p = gc_alloc(minim_vector_size(slot_count));
    mobj header = ((mobj)slot_count << 4) | sec;
    ((mobj *)p)[0] = header;
    mobj *slots = (mobj *)p + 1;
    for (size_t i = 0; i < slot_count; i++) slots[i] = fill;
    return (mobj)((uintptr_t)p | MTAG_TYPED_OBJ);
}

/*-----------------------------------------------------------------------
 * Vector: tag=0x7, 1+ slots [header, slot0, slot1, ...]
 * -------------------------------------------------------------------- */

mobj Mvector(size_t length, mobj fill) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(fill);
    char *p = gc_alloc(minim_vector_size(length));
    mobj header = ((mobj)length << 4) | MSEC_VECTOR;
    ((mobj *)p)[0] = header;
    mobj *slots = (mobj *)p + 1;
    for (size_t i = 0; i < length; i++)
        slots[i] = fill;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_TYPED_OBJ);
}

/* ----------------------------------------------------------------------
 * Closure: tag=0x5, 4 fixed slots [params, body, env, name].
 *
 * Modeled on Chez's `closure_disp_*` layout — closures get their own
 * primary tag because procedure application is the hottest path in
 * the runtime. No header word: the size is implied by the tag and is
 * fixed at MINIM_CLOSURE_SIZE bytes. The GC's forward-marker scheme
 * uses the first two slots (params/body) of the old location during
 * a copy, just like pairs.
 * -------------------------------------------------------------------- */

mobj Mclosure(mobj params, mobj body, mobj env, mobj name) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(params);
    MINIM_GC_PROTECT(body);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(name);
    char *p = gc_alloc(MINIM_CLOSURE_SIZE);
    ((mobj *)p)[0] = params;
    ((mobj *)p)[1] = body;
    ((mobj *)p)[2] = env;
    ((mobj *)p)[3] = name;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_CLOSURE);
}

/* ----------------------------------------------------------------------
 * Environment frame: tag=0x7, 2 slots [rib, parent]
 * -------------------------------------------------------------------- */

mobj Menv_extend(mobj rib, mobj parent) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT2(rib, parent);
    mobj v = typed_alloc(2, MSEC_ENV);
    Mtyped_obj_set(v, 0, rib);
    Mtyped_obj_set(v, 1, parent);
    MINIM_GC_FRAME_END;
    return v;
}

/* ----------------------------------------------------------------------
 * Continuation frame: tag=0x7, 3 standard slots [kind, parent, env] plus
 * `extra_slots` kind-specific slots, zero-initialized. Per-kind
 * constructors below wrap this with semantic argument names.
 * -------------------------------------------------------------------- */

mobj Mkont(mobj kind, mobj parent, mobj env, size_t extra_slots) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT3(kind, parent, env);
    mobj v = typed_alloc(3 + extra_slots, MSEC_KONT);
    Mtyped_obj_set(v, 0, kind);
    Mtyped_obj_set(v, 1, parent);
    Mtyped_obj_set(v, 2, env);
    MINIM_GC_FRAME_END;
    return v;
}

mobj Mkont_if(mobj parent, mobj env, mobj then_expr, mobj else_expr) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(parent);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(then_expr);
    MINIM_GC_PROTECT(else_expr);
    mobj v = Mkont(KONT_IF, parent, env, 2);
    Mtyped_obj_set(v, 3, then_expr);
    Mtyped_obj_set(v, 4, else_expr);
    MINIM_GC_FRAME_END;
    return v;
}

mobj Mkont_seq(mobj parent, mobj env, mobj rest) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT3(parent, env, rest);
    mobj v = Mkont(KONT_SEQ, parent, env, 1);
    Mtyped_obj_set(v, 3, rest);
    MINIM_GC_FRAME_END;
    return v;
}

mobj Mkont_app(mobj parent, mobj env, mobj unev, mobj evald) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(parent);
    MINIM_GC_PROTECT(env);
    MINIM_GC_PROTECT(unev);
    MINIM_GC_PROTECT(evald);
    mobj v = Mkont(KONT_APP, parent, env, 2);
    Mtyped_obj_set(v, 3, unev);
    Mtyped_obj_set(v, 4, evald);
    MINIM_GC_FRAME_END;
    return v;
}

mobj Mkont_set(mobj parent, mobj env, mobj name) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT3(parent, env, name);
    mobj v = Mkont(KONT_SET, parent, env, 1);
    Mtyped_obj_set(v, 3, name);
    MINIM_GC_FRAME_END;
    return v;
}

mobj Mkont_define(mobj parent, mobj env, mobj name) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT3(parent, env, name);
    mobj v = Mkont(KONT_DEFINE, parent, env, 1);
    Mtyped_obj_set(v, 3, name);
    MINIM_GC_FRAME_END;
    return v;
}

/* ----------------------------------------------------------------------
 * Primitive procedure: tag=0x7, 4 slots [name-symbol, arity-min, arity-max,
 * fn-table-index].
 *
 * The function pointer cannot live directly in a slot — function
 * pointers are not ABI-guaranteed to be 8-byte aligned, and an
 * unaligned pointer's low 3 bits could collide with a non-leaf primary
 * tag (MTAG_PAIR / MTAG_FLONUM / MTAG_SYMBOL / MTAG_TYPED_OBJ) that
 * the GC would then dereference as a heap pointer. The fnptr table
 * itself lives in src/eval.c (outside the GC heap); we just store its
 * index here as a fixnum.
 * -------------------------------------------------------------------- */

mobj Mprim(const char *name, Mprim_fn fn, intptr_t arity_min, intptr_t arity_max) {
    MINIM_GC_FRAME_BEGIN;
    mobj name_sym = Mintern(name);
    MINIM_GC_PROTECT(name_sym);
    size_t idx = prim_fn_register(fn);
    mobj v = typed_alloc(4, MSEC_PRIM);
    Mtyped_obj_set(v, 0, name_sym);
    Mtyped_obj_set(v, 1, Mfixnum(arity_min));
    Mtyped_obj_set(v, 2, Mfixnum(arity_max));
    Mtyped_obj_set(v, 3, Mfixnum((intptr_t)idx));
    MINIM_GC_FRAME_END;
    return v;
}

/* ----------------------------------------------------------------------
 * Captured continuation: tag=0x7, 1 slot [kont]
 * -------------------------------------------------------------------- */

mobj Mcont(mobj kont) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(kont);
    mobj v = typed_alloc(1, MSEC_CONT);
    Mtyped_obj_set(v, 0, kont);
    MINIM_GC_FRAME_END;
    return v;
}
