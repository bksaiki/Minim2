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

/* Walk the lexical-env chain looking for `sym` in each rib. Each rib
 * is a vector laid out as [name0 val0 name1 val1 ...]; we scan
 * linearly. Top-level lookup falls through to Merror until Phase 5
 * lands the global env. */
static mobj env_lookup(mobj env, mobj sym) {
    while (Menvp(env)) {
        mobj rib = Menv_rib(env);
        size_t len = Mvector_length(rib);
        for (size_t i = 0; i < len; i += 2) {
            if (Mvector_ref(rib, i) == sym) return Mvector_ref(rib, i + 1);
        }
        env = Menv_parent(env);
    }
    Merror("unbound variable: %s", Msymbol_name(sym));
    return Mnull; /* unreachable */
}

/* Walk the chain looking for `sym` and mutate its slot. Returns true
 * if found, false otherwise. (Used by `set!` once Phase 5 lands; kept
 * here so the lookup/mutate pair lives together.) */
static bool env_set(mobj env, mobj sym, mobj val) {
    while (Menvp(env)) {
        mobj rib = Menv_rib(env);
        size_t len = Mvector_length(rib);
        for (size_t i = 0; i < len; i += 2) {
            if (Mvector_ref(rib, i) == sym) {
                Mvector_set(rib, i + 1, val);
                return true;
            }
        }
        env = Menv_parent(env);
    }
    return false;
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

        if (head == let_sym) {
            /* (let ((var0 init0) ...) body ...). Evaluate inits
             * left-to-right in the *current* env. Once all are
             * collected, build a rib `[var0 v0 ...]`, extend env,
             * and run the body as implicit `begin` in tail position.
             *
             * The KONT_LET frame keeps the still-pending bindings at
             * its head so APPLY can pair the just-arrived value with
             * the current var. The body lives in the frame too. */
            if (list_length(rest) < 2) Merror("malformed let");
            mobj bindings = Mcar(rest);
            mobj body = Mcdr(rest);
            if (!Mnullp(bindings) && !Mpairp(bindings))
                Merror("let: bindings must be a list");
            for (mobj b = bindings; Mpairp(b); b = Mcdr(b)) {
                mobj binding = Mcar(b);
                if (list_length(binding) != 2)
                    Merror("let: each binding must be (var init)");
                if (!Msymbolp(Mcar(binding)))
                    Merror("let: binding name must be a symbol");
            }

            if (Mnullp(bindings)) {
                /* (let () body ...) — degenerate: run body in current env. */
                eval_expr = Mcar(body);
                if (Mpairp(Mcdr(body))) {
                    eval_kont = Mkont_seq(eval_kont, eval_env, Mcdr(body));
                }
                return;
            }

            /* Push KONT_LET, then eval the first binding's init.
             * Discipline: assign eval_expr before Mkont_let allocates. */
            eval_expr = Mcar(Mcdr(Mcar(bindings)));
            eval_kont = Mkont_let(eval_kont, eval_env, bindings, Mnull, body);
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

    if (kind == KONT_LET) {
        /* The just-arrived value is val of the binding at head of the
         * frame's `pending` list. Pair it with that var, prepend to
         * `evald`, and either advance to the next init or build the
         * rib + extend env + run body.
         *
         * Discipline: this branch allocates several times (Mcons for
         * the new evald entry, Mvector for the rib, Menv_extend, and
         * possibly Mkont_seq). Snapshot every kont field into
         * protected locals up front; after that the bare `k` C local
         * becomes irrelevant since we use `eval_kont` (the registered
         * global) for any further frame access. */
        MINIM_GC_FRAME_BEGIN;
        mobj pending = Mnull, evald = Mnull, body = Mnull;
        mobj parent = Mnull, saved_env = Mnull;
        mobj value = Mnull, var = Mnull, rest_pending = Mnull;
        mobj rib = Mnull, new_pair = Mnull;
        MINIM_GC_PROTECT(pending);
        MINIM_GC_PROTECT(evald);
        MINIM_GC_PROTECT(body);
        MINIM_GC_PROTECT(parent);
        MINIM_GC_PROTECT(saved_env);
        MINIM_GC_PROTECT(value);
        MINIM_GC_PROTECT(var);
        MINIM_GC_PROTECT(rest_pending);
        MINIM_GC_PROTECT(rib);
        MINIM_GC_PROTECT(new_pair);

        pending = Mtyped_obj_ref(k, 3);
        evald   = Mtyped_obj_ref(k, 4);
        body    = Mtyped_obj_ref(k, 5);
        parent  = Mkont_parent(k);
        saved_env = Mkont_env(k);
        value   = eval_expr;

        var = Mcar(Mcar(pending));
        rest_pending = Mcdr(pending);

        if (Mnullp(rest_pending)) {
            /* Final binding — build rib including (var, value) and
             * every (var . val) pair already in `evald`. Then extend
             * the let-time env, pop the frame, and run the body. */
            size_t count = 1;
            for (mobj e = evald; Mpairp(e); e = Mcdr(e)) count++;
            rib = Mvector(2 * count, Mfalse);
            Mvector_set(rib, 0, var);
            Mvector_set(rib, 1, value);
            size_t i = 2;
            for (mobj e = evald; Mpairp(e); e = Mcdr(e), i += 2) {
                mobj p = Mcar(e);
                Mvector_set(rib, i,     Mcar(p));
                Mvector_set(rib, i + 1, Mcdr(p));
            }
            eval_env = Menv_extend(rib, saved_env);
            eval_kont = parent; /* pop */
            /* Run body as implicit begin (in tail position). */
            eval_expr = Mcar(body);
            mobj more = Mcdr(body);
            if (Mpairp(more)) {
                eval_kont = Mkont_seq(eval_kont, eval_env, more);
            }
        } else {
            /* More bindings to evaluate. Save (var . value) on
             * `evald`, advance `pending`, and EVAL the next init in
             * the let-time env. eval_kont still points at the same
             * frame (we haven't popped), so we update its slots
             * through eval_kont rather than the stale C local k. */
            new_pair = Mcons(var, value);
            evald = Mcons(new_pair, evald);
            Mtyped_obj_set(eval_kont, 3, rest_pending);
            Mtyped_obj_set(eval_kont, 4, evald);
            eval_env = saved_env;
            eval_expr = Mcar(Mcdr(Mcar(rest_pending)));
        }

        MINIM_GC_FRAME_END;
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
