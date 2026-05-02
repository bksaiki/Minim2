#include "minim.h"
#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mobj Mcons(mobj car, mobj cdr) {
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT2(car, cdr);
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = car;
    ((mobj *)p)[1] = cdr;
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_PAIR);
}

mobj Mflonum(double d) {
    MINIM_GC_FRAME_BEGIN;
    char *p = gc_alloc(16);
    ((mobj *)p)[0] = MTAG_FLONUM; /* header */
    memcpy(p + 8, &d, sizeof(double));
    MINIM_GC_FRAME_END;
    return (mobj)((uintptr_t)p | MTAG_FLONUM);
}

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

