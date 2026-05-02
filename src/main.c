#include "minim.h"

#include <setjmp.h>
#include <stdio.h>

/* ----------------------------------------------------------------------
 * minim REPL — a thin read / eval / write loop for demo purposes.
 *
 * Reads one s-expression from stdin per turn, evaluates it, and prints
 * the result to stdout. The banner and prompt go to stderr, which
 * keeps piped output clean:
 *
 *   echo "(if #t 'yes 'no)" | ./minim       # → just "yes\n" on stdout
 *   ./minim                                  # → prompts visible
 *
 * Parse and evaluation errors are recoverable: an installed setjmp
 * handler catches them via Merror, prints the message, and the loop
 * continues with the next prompt. EOF (Ctrl-D on a tty, or end-of-pipe)
 * exits cleanly.
 * -------------------------------------------------------------------- */

int main(void) {
    Minit();

    fprintf(stderr, "Minim %s\n", MINIM_VERSION_STRING);

    mreader r;
    mreader_init_file(&r, stdin);

    /* Install an error handler so that parse and eval errors print a
     * message and drop us back at the prompt instead of aborting. The
     * handler captures the shadow-stack depth at the top of each
     * iteration so any in-flight MINIM_GC_PROTECT frames inside Mread
     * or Meval are unwound on error. */
    jmp_buf jmp;
    minim_error_jmp = &jmp;

    for (;;) {
        minim_error_jmp_ssp = minim_ssp;
        if (setjmp(jmp) != 0) {
            /* Returned here from Merror's longjmp. The error message
             * has already been printed; just keep looping. */
        }

        fputs("> ", stderr);
        fflush(stderr);

        mobj expr = Mread(&r);
        if (Meofp(expr)) break;

        mobj value = Meval(expr);
        Mwrite(value, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }

    minim_error_jmp = NULL;
    fputc('\n', stderr);
    Mshutdown();
    return 0;
}
