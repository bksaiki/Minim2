#include "minim.h"
#include "gc.h"

#include <string.h>

#define HEAP_INITIAL_BYTES (1024 * 1024)  /* 1 MiB per semispace */

void minim_init(void) {
    gc_init(HEAP_INITIAL_BYTES);
}

void minim_shutdown(void) {
    gc_shutdown();
}

mobj minim_cons(mobj car, mobj cdr) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT2(car, cdr);
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = car;
    ((mobj *)p)[1] = cdr;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_PAIR);
}

mobj minim_make_flonum(double d) {
    MINIM_GC_FRAME_BEGIN;
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = MTAG_FLONUM;  /* header */
    memcpy(p + 8, &d, sizeof(double));
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_FLONUM);
}

mobj minim_make_vector(size_t length, mobj fill) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(fill);
    size_t sz = (size_t)(8 + 8 * length);
    /* MINIM_ALIGN rounds up to 16 */
    sz = (sz + 15) & ~(size_t)15;
    char *p = gc_alloc(sz);
    mobj header = ((mobj)length << 4) | MSEC_VECTOR;
    ((mobj *)p)[0] = header;
    mobj *slots = (mobj *)p + 1;
    for (size_t i = 0; i < length; i++)
        slots[i] = fill;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_TYPED_OBJ);
}

