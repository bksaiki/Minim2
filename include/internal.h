#ifndef _INTERNAL_H_
#define _INTERNAL_H_

/* Defined in symbol.c. Tears down the intern table; must run before
 * gc_shutdown so we can still read symbol name pointers. */
void symbol_shutdown(void);

/* Defined in parser.c. Clears reader state cached across runtime
 * lifetimes (e.g. the cached `quote` symbol). */
void parser_shutdown(void);


#endif /* _INTERNAL_H_ */
