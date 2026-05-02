#include "minim.h"
#include "gc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Special symbols
 * --------------------------------------------------------------------- */

mobj begin_sym;
mobj define_sym;
mobj if_sym;
mobj lambda_sym;
mobj let_sym;
mobj quote_sym;
mobj set_sym;

#define INTERN_AND_PROTECT(sym, name) \
    do { \
        sym = Mintern(name); \
        minim_protect(&sym); \
    } while (0)

/* -----------------------------------------------------------------------
 * Intern table — chained hash map keyed by name string.
 * Lives entirely outside the GC heap (malloc'd).
 * --------------------------------------------------------------------- */

typedef struct intern_bucket {
    mobj symbol; /* tagged mobj; also a global root */
    struct intern_bucket *next;
} intern_bucket;

#define INTERN_TABLE_INIT_SIZE 64

static intern_bucket **intern_table = NULL;
static size_t intern_table_sz = 0;
static size_t intern_table_n = 0; /* total interned symbols */

static unsigned long hash_name(const char *s, size_t len) {
    /* FNV-1a 64-bit */
    unsigned long h = 14695981039346656037UL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211UL;
    }
    return h;
}

void symbol_init(void) {
    intern_table_sz = INTERN_TABLE_INIT_SIZE;
    intern_table = calloc(intern_table_sz, sizeof(intern_bucket *));
    if (!intern_table) { fprintf(stderr, "minim: intern table OOM\n"); abort(); }

    INTERN_AND_PROTECT(begin_sym, "begin");
    INTERN_AND_PROTECT(define_sym, "define");
    INTERN_AND_PROTECT(if_sym, "if");
    INTERN_AND_PROTECT(lambda_sym, "lambda");
    INTERN_AND_PROTECT(let_sym, "let");
    INTERN_AND_PROTECT(quote_sym, "quote");
    INTERN_AND_PROTECT(set_sym, "set!");
}

mobj Mintern(const char *name) {
    size_t len = strlen(name);
    size_t idx = (size_t)(hash_name(name, len) % intern_table_sz);

    /* Search existing bucket */
    for (intern_bucket *b = intern_table[idx]; b; b = b->next) {
        const char *bname = Msymbol_name(b->symbol);
        if (strcmp(bname, name) == 0)
            return b->symbol;
    }

    /* Allocate new symbol on the GC heap */
    char *p = gc_alloc(MINIM_SYMBOL_SIZE);
    /* header word — we use MSEC_MASK+1 area; for symbols the header
     * just needs to be non-MFORWARD_MARKER.  We use MTAG_SYMBOL as
     * a recognizable sentinel. */
    ((mobj *)p)[0] = MTAG_SYMBOL;
    /* name pointer */
    char *name_copy = malloc(len + 1);
    if (!name_copy) { fprintf(stderr, "minim: symbol name OOM\n"); abort(); }
    memcpy(name_copy, name, len + 1);
    *(char **)(p + 8) = name_copy;

    mobj sym = (mobj)((uintptr_t)p | MTAG_SYMBOL);

    /* Insert into bucket */
    intern_bucket *b = malloc(sizeof(intern_bucket));
    if (!b) { fprintf(stderr, "minim: intern bucket OOM\n"); abort(); }
    b->symbol = sym;
    b->next = intern_table[idx];
    intern_table[idx] = b;
    intern_table_n++;

    /* Register symbol slot as a global GC root so GC can update it */
    minim_protect(&b->symbol);

    return sym;
}

void symbol_shutdown(void) {
    if (!intern_table) return;

    for (size_t i = 0; i < intern_table_sz; i++) {
        intern_bucket *b = intern_table[i];
        while (b) {
            intern_bucket *next = b->next;
            /* The symbol's heap object goes away with gc_shutdown; only the
             * malloc'd name string is ours to free. */
            free((void *)Msymbol_name(b->symbol));
            free(b);
            b = next;
        }
    }

    free(intern_table);
    intern_table = NULL;
    intern_table_sz = 0;
    intern_table_n = 0;

    begin_sym = 0;
    define_sym = 0;
    if_sym = 0;
    lambda_sym = 0;
    let_sym = 0;
    quote_sym = 0;
    set_sym = 0;
}
