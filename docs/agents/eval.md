# Evaluator TODO

Tracker for the tree-walking Scheme interpreter. Source of truth for
design decisions: [`docs/EVAL.md`](../EVAL.md). The interpreter lives
in `src/eval.c` (and split-out helpers as needed) with its public API
in `include/minim.h`.

The runtime currently has fixnum, pair, flonum, symbol, vector, plus
four immediates (`#t`, `#f`, `'()`, `eof`). The evaluator adds five
new heap-object kinds — closure (primary tag `MTAG_CLOSURE`), env,
kont, prim — with kont mobjs doubling as first-class continuations.

## Phase 1 — heap shapes
- [x] Add `MTAG_CLOSURE` (primary) plus secondary tags `MSEC_KONT`,
      `MSEC_ENV`, `MSEC_PRIM` in `include/minim.h`.
- [x] GC: closure has its own no-header arm in `object_size` /
      `scan_fields` / scan loop; the typed-object kinds share one
      "walk all slots" branch.
- [x] Constructors in `src/alloc.c`: `Mclosure`, `Mkont`,
      `Menv_extend`, `Mprim`. Each follows the existing `Mvector`
      shadow-stack discipline.
- [x] Predicates and accessors in `include/minim.h`: `Mclosurep`,
      `Mkontp`, `Menvp`, `Mprimp`, plus slot accessors. `Mprocedurep`
      covers closure / prim / kont.
- [x] Round-trip tests in `tests/test_gc.c` and `tests/test_alloc.c`:
      construct each kind, force `gc_collect`, walk slots, verify
      survival.
- [x] Same suite under `MINIM_GC_STRESS=ON`.

## Phase 2 — minimal evaluator (literals, `quote`, `if`, `begin`)
- [x] State globals registered with `minim_protect`: `eval_expr`,
      `eval_env`, `eval_kont`. (`eval_mode` is a small enum, not an
      mobj — no root needed.)
- [x] `eval_init` / `eval_shutdown`, hooked from `Minit`/`Mshutdown`
      in `src/system.c` via declarations in `include/internal.h`.
- [x] Cache special-form symbols (`if_sym`, `begin_sym`) — `quote_sym`
      already lives in `src/symbol.c` for the parser. The eval-loop
      caches its own in `src/eval.c`.
- [x] State-machine loop with `EVAL`/`APPLY`/`HALT` modes; `Meval`
      seeds the state and runs the loop.
- [x] Self-evaluating: fixnum, flonum, immediates, vectors.
- [x] `quote`.
- [x] `if` with `KONT_IF` push/pop.
- [x] `begin` with `KONT_SEQ` push/pop; `KONT_SEQ` pops itself before
      the last expression so tail position falls out.
- [x] Variable lookup against a (still-empty) top-level env aborts
      with an `unbound variable` message.
- [x] Tests in `tests/test_eval.c`: self-eval, `quote`, `if`,
      `begin`, nested forms, deep nesting (200-deep `begin` and
      `if`). Both default and `MINIM_GC_STRESS=ON` configs pass.

### Discipline lesson banked from this phase

In `step_eval`, when a special form needs to push a kont *and* set
`eval_expr` to a sub-expression, do the assignment **before** the
allocation:

    eval_expr = Mcar(rest);                  // registered slot
    eval_kont = Mkont_if(eval_kont, eval_env, then_e, else_e);

A bare C local (`mobj test_e = Mcar(rest); ...; eval_expr = test_e;`)
goes stale across the kont allocation under stress mode — the heap
object moves, the registered globals get forwarded, but the C local
still holds the pre-collection address. Rule: registered globals
absorb GCs; C locals don't.

## Phase 3 — environments and `let`
- [x] Env frame layout (rib vector + parent pointer); helpers
      `env_lookup(env, sym)` and `env_set(env, sym, val)` that walk
      the parent chain. `Menv_extend` already exists from Phase 1.
- [x] Cache `let_sym` alongside the other special-form symbols.
- [x] New continuation frame kind `KONT_LET` (kind value `7 << 3`)
      for driving multi-binding evaluation. Slot layout: pending =
      list of (var init) pairs head-first, evald = list of
      (var . val) pairs in reverse order, body = body forms.
- [x] `(let ((var init) ...) body ...)`: evaluate inits in the
      *current* env left-to-right; on the last init, build a fresh
      rib `[var0 v0 var1 v1 ...]`, extend env, pop the LET frame
      (so the body runs in tail position), and EVAL the body as an
      implicit `begin`. Empty bindings degenerate to running the
      body in the current env.
- [x] Variable lookup walks the lexical env chain; unbound falls
      through to `Merror("unbound variable: …")` until top-level
      lookup lands in Phase 5. `env_set` mutates a chain slot in
      place; not yet wired up (used by `set!` in Phase 5).
- [x] Tests in `tests/test_eval.c`: basic single/multi/empty
      bindings, init using `if`/`begin`/quoted lists, parent-env
      walk, shadowing (3 levels deep), let inside `if`/begin, an
      unbound-error path that exercises `Merror`'s longjmp
      handler, and a 50-binding stress case. Default and
      `MINIM_GC_STRESS=ON` configs both pass.

## Phase 4 — `lambda` and procedure application
- [x] Cache `lambda_sym` alongside the other special-form symbols.
- [x] `(lambda (params...) body...)` constructs a closure capturing
      the current env. Body is a list, evaluated as implicit
      `begin`. Name slot is `#f` for now; Phase 5's `define` will
      fill it for top-level closures.
- [x] Procedure application path: `KONT_APP` push, evaluate
      operator first then arguments left-to-right, prepend each
      value to `evald` (reverse-order accumulator), invoke when
      `unev` is empty. Frame is popped before the closure's body
      runs — automatic tail-call placement.
- [x] Closure invocation: `Mvector` rib `[p0 a0 ...]`, `Menv_extend`
      onto the captured env, EVAL body as implicit `begin`. The
      `Mprimp` and `Mkontp` dispatch arms are stubs that route to
      `Merror`; full implementations land in Phases 6 and 8.
- [x] Arity check: `Merror` if `list_length(params) != list_length
      (args)`.
- [x] Tests in `tests/test_eval.c`: identity, multi-arg, zero-arg,
      multi-expression body, closure capture (incl. independent
      captures from a constructor), shadowing across lambda
      boundaries, lambda passed as arg, `let`-inside-`lambda` and
      `lambda`-inside-`let`, recoverable arity / non-procedure
      errors via the `Merror` longjmp handler, and a 50-arg stress
      case. Default and `MINIM_GC_STRESS=ON` configs both pass.

### GC discipline carried over

`KONT_APP` allocates several times in its APPLY branch (Mcons for
the evald snapshot, Mvector for the rib, Menv_extend for the new
env, possibly Mkont_seq for the body). Same pattern as KONT_LET —
snapshot every kont field into protected locals before the first
allocation, then write back to the frame through `eval_kont` (the
registered global) rather than the C local `k`, which goes stale
across each allocation.

## Phase 5 — top-level env, `set!`, `define`
- [ ] Top-level env structure (hash-table-on-vector keyed by
      symbol-name hash).
- [ ] `define` at top level: mutate top-level env. `define` inside a
      body: extend the current env (R7RS internal-definitions).
- [ ] `set!` with `KONT_SET` push/pop; mutates either a lexical rib
      slot or a top-level binding.
- [ ] Tests: `(define x 1) x`, `(set! x 2) x`, mutating a captured
      variable from inside a closure (counter pattern).

## Phase 6 — primitives
- [ ] `MSEC_PRIM` constructor + `prim_register(name, fn, min, max)`
      helper that interns the name, allocates the prim, and installs
      it into the top-level env.
- [ ] APPLY dispatches on closure-vs-primitive-vs-cont; primitive
      path calls the C function with the `evald` arg list.
- [ ] Implement (split into groups; one PR per group is fine):
      - Arithmetic: `+`, `-`, `*`, `=`, `<`, `>`, `<=`, `>=`,
        `quotient`, `remainder`, `zero?`, `positive?`, `negative?`,
        `abs`. Mixed fixnum/flonum follows R7RS contagion (any
        flonum arg ⇒ flonum result).
      - Pair / list: `cons`, `car`, `cdr`, `pair?`, `null?`, `list`,
        `list?`.
      - Equality: `eq?`, `eqv?`. `equal?` deferred until we have
        cycle-aware structural equality.
      - I/O: `display`, `write`, `newline`, `read`. `display` is
        currently identical to `write` (until strings/chars exist —
        see `docs/agents/writer.md` Phase 3).
      - Type predicates: `symbol?`, `number?`, `boolean?`,
        `procedure?`, `vector?`, `eof-object?`.
      - Vectors: `make-vector`, `vector-ref`, `vector-set!`,
        `vector-length`, `vector?`, `vector`.
- [ ] Tests per group.

## Phase 7 — tail-call optimization
- [ ] By construction: `KONT_APP` pops *before* invoking the closure
      body; `KONT_IF` pops before EVAL of the chosen branch;
      `KONT_SEQ` pops before the last sub-expression.
- [ ] Test: 1M-iteration tail-recursive loop runs without growing
      the live heap past one or two semispaces. Run under stress to
      double-check.
- [ ] Test: mutually-recursive `even?`/`odd?` for 1M iterations.

## Phase 8 — `call/cc`
- [ ] `call/cc` primitive: capture global `kont`, pass it directly
      to the user procedure as a kont mobj (no wrapper object).
- [ ] Continuation invocation in APPLY: when the procedure being
      applied is `Mkontp`, replace global `kont` with the captured
      one, set `expr` to the supplied value, switch to APPLY.
- [ ] Tests: simple escape (`(call/cc (lambda (k) (+ 1 (k 42))))`),
      generator-style coroutine, exception-style early exit, multiple
      re-invocations of a saved continuation, capture-and-invoke
      across textually distant contexts.

## Phase 9 — REPL
- [ ] Promote `src/main.c` to a read-eval-write loop on `stdin`.
- [ ] Print prompt; on `Meof`, exit cleanly.
- [ ] Smoke-test via stdin scripts in CI.

## Phase 10 — robustness and ergonomics
- [ ] Error handling on malformed input (unbound variable, arity
      mismatch, applying non-procedure, set! of unbound, etc.) —
      abort with a clear message; revisit once exceptions land.
- [ ] `KONT_EXC` reserved by Phase 1; no implementation in v1.

## Out of scope for v1
- `dynamic-wind` (interacts subtly with `call/cc`).
- Macros: `define-syntax`, `syntax-rules`, `let-syntax`,
  `letrec-syntax`. Sugar that desugars to v1 forms (`let`, `let*`,
  `letrec`, `cond`, `and`, `or`, `when`, `unless`) is deferred to
  the same layer.
- Multiple values: `values`, `call-with-values`.
- Exception handling: `with-exception-handler`, `raise`,
  `error-object?`. `KONT_EXC` is reserved as a frame kind so the
  future implementation lives in the same chain.
- Tail-call analysis at parse time. Kont-placement rules give us
  TCO automatically; no analysis needed.
- Bignums, rationals, complex numbers.
- Characters and strings (blocked on the runtime types not existing
  yet).
- `delay`/`force`/promises.
- Continuation marks.
- `eval` inside the language.
