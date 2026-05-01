#ifndef MINIM_GC_H
#define MINIM_GC_H

#include <stddef.h>
#include "minim.h"

void gc_init(size_t initial_bytes);
void gc_shutdown(void);
char *gc_alloc(size_t n);
void gc_collect(size_t need);

#endif /* MINIM_GC_H */
