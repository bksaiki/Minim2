#ifndef MINIM_INTERNAL_H
#define MINIM_INTERNAL_H

/* Defined in symbol.c. Initializes the intern table.
 */
void symbol_init(void);

/* Defined in symbol.c. Tears down the intern table; must run before
 * gc_shutdown so we can still read symbol name pointers. */
void symbol_shutdown(void);

#endif /* MINIM_INTERNAL_H_ */
