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

/* Top-level environment. Implemented as an alist of (sym . val)
 * pairs, prepended to on each new top-level `define`. Linear lookup
 * is fine for v1; a hash representation can drop in once it's worth
 * the complexity. Lives on the GC heap and is registered as a single
 * global root, so its entries' values are properly traced. */
static mobj top_level_env;

void eval_init(void) {
    eval_expr = Mnull;
    eval_env = Mnull;
    eval_kont = Mnull;
    top_level_env = Mnull;
    minim_protect(&eval_expr);
    minim_protect(&eval_env);
    minim_protect(&eval_kont);
    minim_protect(&top_level_env);
    prims_register_all();
}

void eval_shutdown(void) {
    /* Keep the malloc'd prim table buffer; just reset the count so
     * the next Minit cycle starts from a clean index space. */
    prim_fn_n = 0;

    /* Reset the eval state slots. gc_shutdown drops the global_roots
     * array, so re-registration on the next Minit is correct. The
     * special-form symbols are reset by symbol_shutdown. */
    eval_expr = 0;
    eval_env = 0;
    eval_kont = 0;
    top_level_env = 0;
}

/* ======================================================================
 * Helpers
 * ====================================================================== */

static bool is_self_evaluating(mobj v) {
    return Mfixnump(v) || Mflonump(v) || Mimmediatep(v) || Mvectorp(v);
}

/* Top-level env helpers. The environment is an alist of (sym . val)
 * pairs prepended to on each new define; lookup is O(n) and that's
 * fine until performance matters. None of these helpers allocate
 * unless `top_env_define` needs to prepend a new entry. */

static bool top_env_lookup(mobj sym, mobj *out) {
    for (mobj p = top_level_env; Mpairp(p); p = Mcdr(p)) {
        mobj entry = Mcar(p);
        if (Mcar(entry) == sym) {
            *out = Mcdr(entry);
            return true;
        }
    }
    return false;
}

static bool top_env_set(mobj sym, mobj val) {
    for (mobj p = top_level_env; Mpairp(p); p = Mcdr(p)) {
        mobj entry = Mcar(p);
        if (Mcar(entry) == sym) {
            Mset_cdr(entry, val);
            return true;
        }
    }
    return false;
}

static void top_env_define(mobj sym, mobj val) {
    /* Replace existing or prepend a fresh entry. Mcons can run a GC
     * so sym/val must be protected. */
    if (top_env_set(sym, val)) return;
    MINIM_GC_FRAME_BEGIN;
    MINIM_GC_PROTECT(sym);
    MINIM_GC_PROTECT(val);
    mobj entry = Mcons(sym, val);
    MINIM_GC_PROTECT(entry);
    top_level_env = Mcons(entry, top_level_env);
    MINIM_GC_FRAME_END;
}

/* Bind `name` to a freshly-allocated primitive procedure in the
 * top-level env. Used by prims_register_all during eval_init. */
void prim_register(const char *name, Mprim_fn fn,
                   intptr_t arity_min, intptr_t arity_max) {
    MINIM_GC_FRAME_BEGIN;
    mobj sym = Mintern(name);
    MINIM_GC_PROTECT(sym);
    mobj p = Mprim(name, fn, arity_min, arity_max);
    MINIM_GC_PROTECT(p);
    top_env_define(sym, p);
    MINIM_GC_FRAME_END;
}

/* Walk the lexical-env chain looking for `sym` in each rib. Each rib
 * is a vector laid out as [name0 val0 name1 val1 ...]; we scan
 * linearly. Top-level fallthrough handles globals defined via
 * (define ...). */
static mobj env_lookup(mobj env, mobj sym) {
    while (Menvp(env)) {
        mobj rib = Menv_rib(env);
        size_t len = Mvector_length(rib);
        for (size_t i = 0; i < len; i += 2) {
            if (Mvector_ref(rib, i) == sym) return Mvector_ref(rib, i + 1);
        }
        env = Menv_parent(env);
    }
    mobj val;
    if (top_env_lookup(sym, &val)) return val;
    Merror("unbound variable: %s", Msymbol_name(sym));
    return Mnull; /* unreachable */
}

/* Walk the chain looking for `sym` and mutate its slot. Returns true
 * if found, false otherwise. Used by `set!` to update the innermost
 * lexical binding; on failure, the caller falls through to the
 * top-level env. */
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
        /* Bare `()` is a syntax error per R7RS — the empty-list datum
         * must be quoted to flow as a value. Check this before the
         * self-evaluating arm, since Mnull's primary tag is
         * MTAG_IMMEDIATE and would otherwise pass that test. */
        if (Mnullp(expr)) {
            Merror("cannot evaluate empty list (use '() for the literal)");
        }

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

        if (head == lambda_sym) {
            /* (lambda (params...) body...). Construct a closure that
             * captures the current env. The body is a list of forms
             * run as implicit `begin` once the closure is invoked.
             * Closure name is `#f` for now; Phase 5's `define` will
             * fill it in for top-level closures. */
            if (list_length(rest) < 2)
                Merror("malformed lambda (need params and body)");
            mobj params = Mcar(rest);
            mobj body = Mcdr(rest);
            if (!Mnullp(params) && !Mpairp(params))
                Merror("lambda: params must be a list");
            for (mobj p = params; Mpairp(p); p = Mcdr(p)) {
                if (!Msymbolp(Mcar(p)))
                    Merror("lambda: param name must be a symbol");
            }
            eval_expr = Mclosure(params, body, eval_env, Mfalse);
            eval_mode = APPLY_MODE;
            return;
        }

        if (head == define_sym) {
            /* (define name value-expr). Phase 5 supports top-level
             * defines only — internal defines (R7RS body-position)
             * arrive in a later pass that desugars them to letrec*.
             *
             * Reject internal defines explicitly: silently mutating
             * the top-level env when the user clearly meant a body-
             * local binding is worse than an error. We detect "we're
             * at top level" by `eval_env` being Mnull — every let /
             * lambda / let-style form extends the env to an Menvp
             * frame, so anything that has been entered shows up as
             * non-null here. */
            if (Menvp(eval_env)) {
                Merror("internal define not supported yet "
                       "(only top-level define is implemented)");
            }
            if (list_length(rest) != 2) Merror("malformed define");
            mobj name = Mcar(rest);
            if (!Msymbolp(name)) Merror("define: name must be a symbol");
            eval_expr = Mcar(Mcdr(rest));
            eval_kont = Mkont_define(eval_kont, eval_env, name);
            return;
        }

        if (head == set_sym) {
            /* (set! name value-expr). Push KONT_SET carrying the
             * name; EVAL the value; APPLY mutates the innermost
             * lexical binding, falling through to the top-level
             * env on miss. */
            if (list_length(rest) != 2) Merror("malformed set!");
            mobj name = Mcar(rest);
            if (!Msymbolp(name)) Merror("set!: name must be a symbol");
            eval_expr = Mcar(Mcdr(rest));
            eval_kont = Mkont_set(eval_kont, eval_env, name);
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

        /* Procedure application. Push KONT_APP and evaluate the
         * operator first; KONT_APP's APPLY path then walks through
         * the arguments left-to-right, finally invoking the operator
         * on the accumulated values.
         *
         * Discipline: stash the operator in eval_expr (registered)
         * BEFORE Mkont_app allocates, so the kont push can't strand
         * the operator expression in a stale C local. `args` is a
         * read of expr's cdr; Mkont_app protects it as `unev` once
         * inside the call. */
        mobj args = Mcdr(expr);
        eval_expr = Mcar(expr);
        eval_kont = Mkont_app(eval_kont, eval_env, args, Mnull);
        return;
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

    if (kind == KONT_APP) {
        /* The just-arrived value is the next position in the
         * operator-and-args sequence. Prepend it to `evald` (which
         * is in reverse order). If unevaluated args remain, advance
         * to the next; otherwise it's time to invoke.
         *
         * Convention: `evald` ends up holding [aN, ..., a0, op] in
         * reverse-of-evaluation order. Reversing once at invocation
         * time gives the natural [op, a0, ..., aN] form.
         *
         * Discipline mirrors KONT_LET: snapshot every kont field
         * into protected locals before the first allocation, then
         * use eval_kont (the registered global) to write back into
         * the frame. The C local `k` goes stale across each Mcons /
         * Mvector / Menv_extend in this branch. */
        MINIM_GC_FRAME_BEGIN;
        mobj unev = Mnull, evald = Mnull;
        mobj parent = Mnull, saved_env = Mnull;
        mobj value = Mnull;
        MINIM_GC_PROTECT(unev);
        MINIM_GC_PROTECT(evald);
        MINIM_GC_PROTECT(parent);
        MINIM_GC_PROTECT(saved_env);
        MINIM_GC_PROTECT(value);

        unev = Mtyped_obj_ref(k, 3);
        evald = Mtyped_obj_ref(k, 4);
        parent = Mkont_parent(k);
        saved_env = Mkont_env(k);
        value = eval_expr;

        /* Snapshot the new value onto evald. */
        evald = Mcons(value, evald);

        if (Mpairp(unev)) {
            /* More to evaluate. Update the frame, EVAL the next. */
            mobj next_expr = Mcar(unev);
            mobj rest_unev = Mcdr(unev);
            Mtyped_obj_set(eval_kont, 3, rest_unev);
            Mtyped_obj_set(eval_kont, 4, evald);
            eval_env = saved_env;
            eval_expr = next_expr;
            MINIM_GC_FRAME_END;
            eval_mode = EVAL_MODE;
            return;
        }

        /* unev is empty — we have a complete (op + args) sequence.
         * Reverse evald to get [op, a0, ..., aN]. */
        mobj reversed = Mnull, walk = Mnull;
        MINIM_GC_PROTECT(reversed);
        MINIM_GC_PROTECT(walk);
        walk = evald;
        while (Mpairp(walk)) {
            reversed = Mcons(Mcar(walk), reversed);
            walk = Mcdr(walk);
        }
        mobj op = Mnull, args = Mnull;
        MINIM_GC_PROTECT(op);
        MINIM_GC_PROTECT(args);
        op = Mcar(reversed);
        args = Mcdr(reversed);

        /* Pop the KONT_APP frame. The procedure's body will run with
         * `parent` as the current kont — automatic tail-call
         * placement. */
        eval_kont = parent;

        if (Mclosurep(op)) {
            mobj params = Mnull, body = Mnull, captured_env = Mnull;
            mobj rib = Mnull;
            MINIM_GC_PROTECT(params);
            MINIM_GC_PROTECT(body);
            MINIM_GC_PROTECT(captured_env);
            MINIM_GC_PROTECT(rib);

            params = Mclosure_params(op);
            body = Mclosure_body(op);
            captured_env = Mclosure_env(op);

            size_t n_params = list_length(params);
            size_t n_args = list_length(args);
            if (n_params != n_args) {
                Merror("arity mismatch: closure expected %zu arg%s, got %zu",
                       n_params, n_params == 1 ? "" : "s", n_args);
            }
            if (Mnullp(body)) Merror("closure has empty body");

            /* Build rib [p0 a0 p1 a1 ...]. No allocation inside the
             * loop, so plain C locals for `p` and `a` are safe. */
            rib = Mvector(2 * n_params, Mfalse);
            mobj p = params, a = args;
            for (size_t i = 0; i < n_params; i++) {
                Mvector_set(rib, 2 * i,     Mcar(p));
                Mvector_set(rib, 2 * i + 1, Mcar(a));
                p = Mcdr(p);
                a = Mcdr(a);
            }

            eval_env = Menv_extend(rib, captured_env);
            eval_expr = Mcar(body);
            mobj more_body = Mcdr(body);
            if (Mpairp(more_body)) {
                eval_kont = Mkont_seq(eval_kont, eval_env, more_body);
            }
            MINIM_GC_FRAME_END;
            eval_mode = EVAL_MODE;
            return;
        }

        if (Mprimp(op)) {
            /* Arity check using the prim's stored min/max, then call
             * the C function with the already-evaluated args list.
             * `args` is on the protect stack from earlier in this
             * branch, so the prim is free to allocate. The result is
             * a bare local but we use it immediately to write a
             * registered global. */
            intptr_t arity_min = Mfixnum_val(Mprim_arity_min(op));
            intptr_t arity_max = Mfixnum_val(Mprim_arity_max(op));
            size_t n_args = list_length(args);
            if (n_args < (size_t)arity_min) {
                Merror("arity mismatch: %s expected at least %ld arg%s, got %zu",
                       Msymbol_name(Mprim_name(op)), (long)arity_min,
                       arity_min == 1 ? "" : "s", n_args);
            }
            if (arity_max >= 0 && n_args > (size_t)arity_max) {
                Merror("arity mismatch: %s expected at most %ld arg%s, got %zu",
                       Msymbol_name(Mprim_name(op)), (long)arity_max,
                       arity_max == 1 ? "" : "s", n_args);
            }
            Mprim_fn fn = Mprim_fn_of(op);
            eval_expr = fn(args);
            MINIM_GC_FRAME_END;
            eval_mode = APPLY_MODE;
            return;
        }

        if (Mkontp(op)) {
            Merror("continuation invocation not yet supported (Phase 8)");
        }

        Merror("attempt to apply a non-procedure value");
        /* Merror longjmps; not reached. */
        MINIM_GC_FRAME_END;
        return;
    }

    if (kind == KONT_DEFINE) {
        /* The just-arrived value is what `name` should be bound to
         * at top level. If it's an anonymous closure, fill in its
         * name slot for nicer printing. */
        MINIM_GC_FRAME_BEGIN;
        mobj parent = Mnull, saved_env = Mnull, name = Mnull, value = Mnull;
        MINIM_GC_PROTECT(parent);
        MINIM_GC_PROTECT(saved_env);
        MINIM_GC_PROTECT(name);
        MINIM_GC_PROTECT(value);

        parent = Mkont_parent(k);
        saved_env = Mkont_env(k);
        name = Mtyped_obj_ref(k, 3);
        value = eval_expr;

        if (Mclosurep(value) && Mfalsep(Mclosure_name(value))) {
            Mclosure_set_name(value, name);
        }
        top_env_define(name, value);

        eval_env = saved_env;
        eval_kont = parent;
        eval_expr = Mvoid;
        MINIM_GC_FRAME_END;
        eval_mode = APPLY_MODE;
        return;
    }

    if (kind == KONT_SET) {
        /* The just-arrived value is the new value for the binding
         * named in slot 3. Try lexical first; on miss, fall through
         * to the top-level env. Truly unbound is an error. */
        MINIM_GC_FRAME_BEGIN;
        mobj parent = Mnull, saved_env = Mnull, name = Mnull, value = Mnull;
        MINIM_GC_PROTECT(parent);
        MINIM_GC_PROTECT(saved_env);
        MINIM_GC_PROTECT(name);
        MINIM_GC_PROTECT(value);

        parent = Mkont_parent(k);
        saved_env = Mkont_env(k);
        name = Mtyped_obj_ref(k, 3);
        value = eval_expr;

        if (!env_set(saved_env, name, value)) {
            if (!top_env_set(name, value)) {
                Merror("set!: unbound variable: %s", Msymbol_name(name));
            }
        }

        eval_env = saved_env;
        eval_kont = parent;
        eval_expr = Mvoid;
        MINIM_GC_FRAME_END;
        eval_mode = APPLY_MODE;
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
