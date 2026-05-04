#include "gc.h"
#include "internal.h"
#include "minim.h"

#include "core_lib_data.h"

/* Read every form from the bundled core library and Meval each.
 * No error handler is installed: if the bootstrap fails the
 * runtime is broken, so let Merror's default abort path fire. */
static void core_lib_load(void) {
    mreader r;
    mreader_init_string(&r, minim_core_lib);
    for (;;) {
        mobj expr = Mread(&r);
        if (Meofp(expr)) break;
        Meval(expr);
        /* expr is a bare local but we don't use it across the loop
         * boundary — it's freshly produced by Mread each iteration. */
    }
}

void Minit(void) {
    gc_init(HEAP_INITIAL_BYTES);
    symbol_init();
    eval_init();
    core_lib_load();
}

void Mshutdown(void) {
    eval_shutdown();
    symbol_shutdown();
    gc_shutdown();
}
