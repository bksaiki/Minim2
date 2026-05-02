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

/* Defined in eval.c. Resets the primitive-function table index so a
 * new Minit/Mshutdown cycle starts from index 0. The malloc'd table
 * itself is reused across cycles. */
void eval_shutdown(void);

#endif /* MINIM_INTERNAL_H_ */
