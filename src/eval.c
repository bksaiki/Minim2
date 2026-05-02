#include "minim.h"
#include "internal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ======================================================================
 * Primitive function table
 *
 * Bare C function pointers cannot live in a GC-traced slot — they have
 * no ABI-guaranteed alignment, so their low 3 bits could collide with
 * a non-leaf primary tag and the GC would dereference them as heap
 * pointers. Mprim instead stores a fixnum index into this table; the
 * table lives outside the GC heap (malloc'd) and is reset on shutdown
 * so the next Minit cycle starts at index 0.
 * ====================================================================== */

static Mprim_fn *prim_fn_table = NULL;
static size_t prim_fn_n = 0;
static size_t prim_fn_cap = 0;

size_t prim_fn_register(Mprim_fn fn) {
    if (prim_fn_n == prim_fn_cap) {
        size_t nc = prim_fn_cap == 0 ? 32 : prim_fn_cap * 2;
        Mprim_fn *nt = realloc(prim_fn_table, nc * sizeof(Mprim_fn));
        if (!nt) {
            fprintf(stderr, "minim: prim_fn_register: OOM\n");
            abort();
        }
        prim_fn_table = nt;
        prim_fn_cap = nc;
    }
    prim_fn_table[prim_fn_n] = fn;
    return prim_fn_n++;
}

Mprim_fn Mprim_fn_of(mobj v) {
    size_t idx = (size_t)Mfixnum_val(Mtyped_obj_ref(v, 3));
    return prim_fn_table[idx];
}

/* ======================================================================
 * Recoverable errors — see Merror in include/minim.h.
 * ====================================================================== */

jmp_buf *minim_error_jmp = NULL;
size_t   minim_error_jmp_ssp = 0;

void Merror(const char *fmt, ...) {
    va_list ap;
    fputs("minim: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    if (minim_error_jmp != NULL) {
        /* Unwind any partial MINIM_GC_PROTECT frames left behind by
         * the longjmped-over code so the next operation sees a clean
         * shadow stack. */
        minim_ssp = minim_error_jmp_ssp;
        longjmp(*minim_error_jmp, 1);
    }
    abort();
}

/* ======================================================================
 * Eval loop state
 *
 * Four globals carry the entire state of the abstract machine. Three
 * of them (expr, env, kont) are mobjs registered with minim_protect at
 * eval_init, so every transition inside step_eval / step_apply is
 * automatically GC-safe with respect to the state itself — only
 * ephemeral C locals need MINIM_GC_PROTECT.
 *
 * `mode` is a small enum, not an mobj, so it does not need to be
 * registered as a root.
 * ====================================================================== */

typedef enum {
    EVAL_MODE,
    APPLY_MODE,
    HALT_MODE,
} eval_mode_t;

static eval_mode_t eval_mode;
mobj eval_expr;
mobj eval_env;
mobj eval_kont;

void eval_init(void) {
    eval_expr = Mnull;
    eval_env = Mnull;
    eval_kont = Mnull;
    minim_protect(&eval_expr);
    minim_protect(&eval_env);
    minim_protect(&eval_kont);
}

void eval_shutdown(void) {
    /* Keep the malloc'd prim table buffer; just reset the count so
     * the next Minit cycle starts from a clean index space. */
    prim_fn_n = 0;

    /* Reset the eval state slots and special-form symbol caches.
     * gc_shutdown drops the global_roots array, so re-registration
     * on the next Minit is correct. */
    eval_expr = 0;
    eval_env = 0;
    eval_kont = 0;
    if_sym = 0;
    begin_sym = 0;
}

/* ======================================================================
 * Helpers
 * ====================================================================== */

static bool is_self_evaluating(mobj v) {
    return Mfixnump(v) || Mflonump(v) || Mimmediatep(v) || Mvectorp(v);
}

static mobj env_lookup(mobj env, mobj sym) {
    /* Phase 2 has no bindings yet — every lookup is unbound. Lambda
     * and define land in Phases 3-4 and will fill in this routine. */
    (void)env;
    Merror("unbound variable: %s", Msymbol_name(sym));
    return Mnull; /* unreachable */
}

/* Length of a proper list, for arity checks on special forms. Errors
 * if the list is improper. */
static size_t list_length(mobj lst) {
    size_t n = 0;
    while (Mpairp(lst)) { n++; lst = Mcdr(lst); }
    if (!Mnullp(lst)) Merror("improper list where proper list expected");
    return n;
}

/* ======================================================================
 * step_eval — examine eval_expr and either reduce to a value (and
 * switch to APPLY) or push a continuation frame and recurse on a
 * sub-expression.
 * ====================================================================== */

static void step_eval(void) {
    mobj expr = eval_expr;

    /* Self-evaluating literals: numbers, vectors, immediates. */
    if (is_self_evaluating(expr)) {
        eval_mode = APPLY_MODE;
        return;
    }

    /* Symbol: variable reference. */
    if (Msymbolp(expr)) {
        eval_expr = env_lookup(eval_env, expr);
        eval_mode = APPLY_MODE;
        return;
    }

    /* Compound: special form or application. */
    if (Mpairp(expr)) {
        mobj head = Mcar(expr);
        mobj rest = Mcdr(expr);

        if (head == quote_sym) {
            /* (quote datum) → datum, with no recursion. */
            if (list_length(rest) != 1) Merror("malformed quote");
            eval_expr = Mcar(rest);
            eval_mode = APPLY_MODE;
            return;
        }

        if (head == if_sym) {
            /* (if test then else): push KONT_IF, eval test. The IF
             * frame's parent is the current kont, so when the chosen
             * branch eventually returns, control flows past the IF
             * — automatic tail position for both branches.
             *
             * Discipline: stash the test expression in eval_expr
             * (registered root) BEFORE calling Mkont_if, since
             * Mkont_if allocates and the bare C local would go stale
             * across the collection. then_e and else_e are protected
             * inside Mkont_if itself. */
            if (list_length(rest) != 3) Merror("malformed if (expected 3 args)");
            mobj then_e = Mcar(Mcdr(rest));
            mobj else_e = Mcar(Mcdr(Mcdr(rest)));
            eval_expr = Mcar(rest);
            eval_kont = Mkont_if(eval_kont, eval_env, then_e, else_e);
            return; /* stay in EVAL_MODE */
        }

        if (head == begin_sym) {
            /* (begin e1 ... en). With one expression, evaluate it
             * directly with no frame (it's already in tail position).
             * With more, push a SEQ frame holding the rest, evaluate
             * e1; SEQ APPLY pops on the last expression so it runs
             * tail.
             *
             * Same discipline as `if`: write the next expression to
             * eval_expr before calling Mkont_seq. */
            if (Mnullp(rest)) Merror("empty begin");
            mobj more = Mcdr(rest);
            eval_expr = Mcar(rest);
            if (Mnullp(more)) {
                return; /* tail — no SEQ frame needed */
            }
            eval_kont = Mkont_seq(eval_kont, eval_env, more);
            return;
        }

        /* Unrecognized compound form. Procedure application lands in
         * Phase 3; for now this is a recoverable user error. */
        Merror("procedure application not supported in v0");
        return; /* unreachable */
    }

    /* Mnull, or some other non-evaluable form. */
    Merror("cannot evaluate this form");
}

/* ======================================================================
 * step_apply — examine eval_kont's top frame and either pop into
 * HALT_MODE (whole evaluation done) or schedule the next sub-expression
 * by switching back to EVAL_MODE.
 *
 * Every pop restores eval_env to the env captured at the popped
 * frame's creation. This keeps env consistent even after sub-frames
 * have changed it (Phase 3+ closure entry).
 * ====================================================================== */

static void step_apply(void) {
    mobj k = eval_kont;
    mobj kind = Mkont_kind(k);

    if (kind == KONT_HALT) {
        eval_mode = HALT_MODE;
        return;
    }

    if (kind == KONT_IF) {
        mobj then_e = Mtyped_obj_ref(k, 3);
        mobj else_e = Mtyped_obj_ref(k, 4);
        mobj test_val = eval_expr;
        eval_env = Mkont_env(k);
        eval_kont = Mkont_parent(k);
        /* Only #f is false; everything else (including 0, '(), nil)
         * is truthy in Scheme. */
        eval_expr = (test_val == Mfalse) ? else_e : then_e;
        eval_mode = EVAL_MODE;
        return;
    }

    if (kind == KONT_SEQ) {
        /* The just-arrived value is discarded; we proceed with the
         * next expression in the begin body. */
        mobj rest = Mtyped_obj_ref(k, 3);
        if (Mnullp(rest)) {
            /* Should never happen — a one-expression begin doesn't
             * push a SEQ, and longer begins always have at least
             * one element when SEQ is consulted. */
            Merror("internal: SEQ frame with empty rest");
        }
        mobj first = Mcar(rest);
        mobj more = Mcdr(rest);
        if (Mnullp(more)) {
            /* Last expression — pop SEQ before evaluating so the
             * expression sits in the original tail position. */
            eval_env = Mkont_env(k);
            eval_kont = Mkont_parent(k);
        } else {
            /* More to come — keep the frame, advance its rest, and
             * EVAL the next expression. The frame's env was captured
             * at SEQ-push time; restore it as the env for `first`. */
            Mtyped_obj_set(k, 3, more);
            eval_env = Mkont_env(k);
        }
        eval_expr = first;
        eval_mode = EVAL_MODE;
        return;
    }

    Merror("internal: APPLY: unhandled kont kind %ld",
           (long)Mfixnum_val(kind));
}

/* ======================================================================
 * Public entry point
 * ====================================================================== */

mobj Meval(mobj expr) {
    /* Seed the state from the argument. eval_expr / env / kont are
     * registered roots, so once we copy `expr` into eval_expr and
     * allocate the bottom KONT_HALT frame, nothing further needs
     * shadow-stack discipline at this level. */
    eval_expr = expr;
    eval_env = Mnull;
    eval_kont = Mkont(KONT_HALT, Mnull, Mnull, 0);
    eval_mode = EVAL_MODE;

    while (eval_mode != HALT_MODE) {
        if (eval_mode == EVAL_MODE) step_eval();
        else                        step_apply();
    }

    return eval_expr;
}
