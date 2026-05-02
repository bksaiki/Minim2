#include "minim.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

/* ----------------------------------------------------------------------
 * Primitive function table
 *
 * Bare C function pointers cannot live in a GC-traced slot — they have
 * no ABI-guaranteed alignment, so their low 3 bits could collide with
 * a non-leaf primary tag and the GC would dereference them as heap
 * pointers. Mprim instead stores a fixnum index into this table; the
 * table lives outside the GC heap (malloc'd) and is reset on shutdown
 * so the next Minit cycle starts at index 0.
 * -------------------------------------------------------------------- */

static Mprim_fn *prim_fn_table = NULL;
static size_t prim_fn_n = 0;
static size_t prim_fn_cap = 0;

size_t prim_fn_register(Mprim_fn fn) {
    if (prim_fn_n == prim_fn_cap) {
        size_t nc = prim_fn_cap == 0 ? 32 : prim_fn_cap * 2;
        Mprim_fn *nt = realloc(prim_fn_table, nc * sizeof(Mprim_fn));
        if (!nt) {
            fprintf(stderr, "minim: prim_fn_register: OOM\n");
            abort();
        }
        prim_fn_table = nt;
        prim_fn_cap = nc;
    }
    prim_fn_table[prim_fn_n] = fn;
    return prim_fn_n++;
}

Mprim_fn Mprim_fn_of(mobj v) {
    size_t idx = (size_t)Mfixnum_val(Mtyped_obj_ref(v, 3));
    return prim_fn_table[idx];
}

void eval_shutdown(void) {
    /* Keep the malloc'd buffer; just reset the count so the next
     * Minit cycle starts from a clean index space. */
    prim_fn_n = 0;
}
