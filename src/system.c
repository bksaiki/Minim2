#include "gc.h"
#include "internal.h"
#include "minim.h"

void Minit(void) {
    gc_init(HEAP_INITIAL_BYTES);
    symbol_init();

    quote_sym = Mintern("quote");
    minim_protect(&quote_sym);

    eval_init();
}

void Mshutdown(void) {
    eval_shutdown();
    symbol_shutdown();
    gc_shutdown();

    quote_sym = 0;
}
