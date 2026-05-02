#ifndef _INTERNAL_H_
#define _INTERNAL_H_

/* Defined in symbol.c. Initializes the intern table.
 */
void symbol_init(void);

/* Defined in symbol.c. Tears down the intern table; must run before
 * gc_shutdown so we can still read symbol name pointers. */
void symbol_shutdown(void);

#endif /* _INTERNAL_H_ */
