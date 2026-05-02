#include "gc.h"
#include "internal.h"
#include "minim.h"

void Minit(void) {
    gc_init(HEAP_INITIAL_BYTES);
}

void Mshutdown(void) {
    parser_shutdown();
    symbol_shutdown();
    gc_shutdown();
}
