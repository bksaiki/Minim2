#ifndef MINIM_INTERNAL_H
#define MINIM_INTERNAL_H

/* Defined in symbol.c. Initializes the intern table.
 */
void symbol_init(void);

/* Defined in symbol.c. Tears down the intern table; must run before
 * gc_shutdown so we can still read symbol name pointers. */
void symbol_shutdown(void);

/* Defined in eval.c. Registers a C primitive function in the
 * out-of-GC-heap function table and returns its index. The index is
 * stored as a fixnum in the prim object's slot 3. */
size_t prim_fn_register(Mprim_fn fn);

/* Defined in eval.c. Registers the eval-loop state slots as global GC roots,
 * and seeds them. */
void eval_init(void);

/* Defined in eval.c. Resets the primitive-function table index, the
 * eval-loop state slots, and the special-form symbol caches so a new
 * Minit/Mshutdown cycle starts cleanly. The malloc'd prim table
 * buffer is reused across cycles. */
void eval_shutdown(void);

/* Defined in eval.c. Interns `name`, allocates an MSEC_PRIM object,
 * and binds it into the top-level env. Called from prims_register_all
 * during eval_init, after the env globals are seeded. */
void prim_register(const char *name, Mprim_fn fn,
                   intptr_t arity_min, intptr_t arity_max);

/* Defined in prims.c. Registers every built-in procedure into the
 * top-level env. */
void prims_register_all(void);

#endif /* MINIM_INTERNAL_H_ */
