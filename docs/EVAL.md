# Minim | Interpreter Design

This document captures the design of Minim's tree-walking Scheme
interpreter. The TODO checklist lives at
[`docs/agents/eval.md`](./agents/eval.md). The pointer tagging,
allocator, and GC are described in [`docs/GC.md`](./GC.md); this doc
extends those layouts with the heap shapes the evaluator needs.

The design is organized around two non-negotiable goals:

1. **First-class continuations** (`call/cc`) without stack copying or
   conservative C-stack scanning.
2. **No C-stack overflow on deep recursion** in the interpreted program
   — the C call depth stays O(1) regardless of Scheme call depth.

Both goals are achieved by representing the call stack as a
heap-allocated chain — a **spaghetti stack** — and driving evaluation
through a state-machine loop instead of recursive C functions. The
runtime's existing precise GC and shadow-stack roots make this
representation cheap and trace-friendly.

## Scope

**Special forms supported in v1**: `quote`, `if`, `let`, `lambda`, `set!`,
`define`, `begin`, `call/cc`. Sugar (`let`, `let*`, `letrec`, `cond`,
`and`, `or`, `when`, `unless`) is deferred to a later macro/desugar
layer.

**Heap object kinds added by the interpreter**: closure, environment,
continuation frame, primitive procedure, captured continuation. Each
gets a new `MSEC_*` secondary tag under the existing `MTAG_TYPED_OBJ`
primary tag.

**Out of scope for v1**, called out so future work doesn't redesign
the same surface:

- `dynamic-wind` — interacts subtly with `call/cc`. Punt.
- Macros (`define-syntax`, `syntax-rules`) and any non-trivial
  desugaring. Sugar listed above is deferred to that layer.
- Multiple values (`values`/`call-with-values`).
- Exception handling (`with-exception-handler`, `raise`,
  `error-object?`). A frame kind is *reserved* for it (see below) but
  not implemented.
- Tail-call detection at parse time. We don't need it — the kont
  placement rules give us TCO automatically.
- Bignums, rationals, complex numbers; characters; strings.

## Pointer tagging additions

The interpreter adds one new **primary tag** for closures and four new
**secondary tags** under `MTAG_TYPED_OBJ` for the rest:

| primary | name           | layout                                        | what it represents     |
|--------:|----------------|-----------------------------------------------|------------------------|
| 0x5     | `MTAG_CLOSURE` | 4 fixed mobj slots, no header                 | user-defined procedure |

| sec  | name           | first slot is...                | what it represents       |
|-----:|----------------|----------------------------------|--------------------------|
| 0x0  | `MSEC_VECTOR`  | (length implicit in header)      | vectors (already exists) |
| 0x1  | `MSEC_KONT`    | kind discriminator (small fixnum)| spaghetti-stack frame; also the value `call/cc` hands back |
| 0x2  | `MSEC_ENV`     | rib vector                       | lexical-env frame        |
| 0x3  | `MSEC_PRIM`    | name symbol                      | built-in procedure       |

Closures get their own primary tag — modeled on Chez's
`closure_disp_*` layout — because procedure application is the
hottest path in a Scheme runtime, and a primary-tag check is one
machine instruction. Secondary-tag predicates need an extra header
load and a mask, which is acceptable for everything else.

The remaining typed-object kinds share the same header shape as
vectors: `(slot_count << 4) | secondary_tag` followed by `slot_count`
mobj slots. They are **traced uniformly**: the GC's `scan_fields`
case for `MTAG_TYPED_OBJ` doesn't branch on the secondary tag — it
reads the slot count from the header and forwards every payload slot.
`object_size` computes the byte size from the slot count (identical
formula for every kind).

`MTAG_CLOSURE` has its own dedicated `object_size` and `scan_fields`
arms, since it has no header word: the size is a fixed
`MINIM_CLOSURE_SIZE = 32` bytes (4 slots × 8) and the trace forwards
exactly four mobj fields.

## Heap object layouts

All sizes are 16-byte aligned, like every other heap object.

### Continuation frame (`MSEC_KONT`)

```
slot 0 : kind     (small fixnum: KONT_HALT, KONT_IF, ...)
slot 1 : parent   (mobj — next frame down, or KONT_HALT sentinel)
slot 2 : env      (mobj — environment captured at frame creation)
slot 3+: kind-specific saved data
```

A single secondary tag covers every frame kind. The kind discriminator
in slot 0 selects the APPLY behavior; the GC doesn't care.

Frame kinds for v1:

| kind          | extra slots                                  | pushed by                  |
|---------------|----------------------------------------------|----------------------------|
| `KONT_HALT`   | (none)                                        | initial state              |
| `KONT_IF`     | then-expr, else-expr                          | `if` test eval             |
| `KONT_SEQ`    | rest-of-exprs (Scheme list)                   | `begin` body eval          |
| `KONT_APP`    | unevaluated-args (list), evaluated-args (list)| application eval           |
| `KONT_SET`    | symbol                                        | `set!` rhs eval            |
| `KONT_DEFINE` | symbol                                        | `define` rhs eval          |
| `KONT_EXC`    | (reserved for future exception handler)       | (not pushed in v1)         |

### Closure (`MTAG_CLOSURE`)

```
slot 0 : params    (Scheme list of symbols, or symbol for varargs)
slot 1 : body      (Scheme list of expressions; evaluated as begin)
slot 2 : env       (captured lexical environment)
slot 3 : name      (symbol, or #f if anonymous — for write only)
```

No header word — size is fixed at `MINIM_CLOSURE_SIZE = 32` bytes.
The GC's forward-marker scheme uses the first two slots (params, body)
of the OLD location during a copy, just like pairs. Both fields can
hold any mobj; none of the values they ever take match
`MFORWARD_MARKER = 0x3E` (which carries `MTAG_IMMEDIATE`).

### Environment frame (`MSEC_ENV`)

```
slot 0 : rib       (a vector of [name0, val0, name1, val1, ...])
slot 1 : parent    (mobj — next env frame down, or top-level env, or ())
```

Lookup walks the parent chain, scanning each rib linearly. `set!`
mutates a rib slot in place. The rib being a vector makes lexical-
address resolution a drop-in optimization later (rib index + slot
index) without changing the runtime representation; only the lookup
function changes.

### Primitive procedure (`MSEC_PRIM`)

```
slot 0 : name      (symbol, for write)
slot 1 : arity-min (fixnum)
slot 2 : arity-max (fixnum, or -1 for varargs)
slot 3 : idx       (index into the primitives table, for dispatch)
```

Primitives are first-class — `(map + xs)` works because `+` is a
`mobj`. The eval loop has one APPLY path that dispatches once on
closure-vs-primitive-vs-kont (a kont *is* the value `call/cc` hands
back; see below).

### Captured continuation = a kont mobj

There is no separate "wrapped continuation" heap object. The value
that `call/cc` passes to the user procedure is the current kont
itself — a heap pointer with secondary tag `MSEC_KONT`. Because the
chain is immutable (each frame's `kind`/`parent`/`env`/extras are
fixed at construction), the same kont can be invoked any number of
times safely. Each invocation:

1. Replaces the global current-kont with the saved chain.
2. Sets `expr` to the supplied value.
3. Switches to APPLY.

Capture is O(1); invocation is O(1); no stack copy. The eval loop's
internal frames and the user-visible `(call/cc (lambda (k) ...)) k`
are the same kind of object — `Mkontp` is true for both, and the
APPLY path treats any kont as callable.

### Top-level environment

The top-level env is a *distinct* heap object from `MSEC_ENV` lexical
frames. It wraps a hash-table-on-vector keyed by symbol; lookup falls
through from the lexical chain. Keeping it distinct now avoids future
pain when REPL redefinition meets module loading. Concrete shape:
either a separate `MSEC_TOPENV` or a `MSEC_ENV` whose parent slot is
the sentinel `Mnull` and whose rib is a hash-table vector. Decision
deferred to implementation; the public `env_lookup`/`env_set` API
hides it.

## State-machine evaluator

Four state words are held in static globals registered **once** with
`minim_protect()` at init:

```
mode     : EVAL | APPLY | HALT
expr     : (in EVAL mode) expression being evaluated;
           (in APPLY mode) value being returned to the kont
env      : current lexical environment
kont     : current continuation frame (top of the spaghetti stack)
```

Because these are registered globals, every transition inside the
eval loop is automatically GC-safe with respect to the four state
slots. There is no need for `MINIM_GC_FRAME_BEGIN` discipline inside
eval/apply themselves; only ephemeral C locals (for example, an `mobj`
held while building a new kont frame) need `MINIM_GC_PROTECT`.

The loop:

```c
void Meval_loop(void) {
    while (mode != HALT) {
        if (mode == EVAL)  step_eval();
        else               step_apply();
    }
}
```

`step_eval()` examines `expr` and either reduces it directly (literal,
variable lookup) and switches to APPLY, or pushes a new kont frame
and replaces `expr`/`env` with a sub-expression to evaluate next.

`step_apply()` examines `kont`'s kind and either pops the frame and
continues with the parent, or schedules the next sub-expression by
switching back to EVAL.

`HALT` is set when APPLY pops a `KONT_HALT` frame; the resulting
`expr` is the final value, returned to the caller of `Meval`.

## Tail-call discipline (no analysis required)

A kont frame represents "what to do **after** this sub-expression". A
sub-expression is in tail position iff entering its evaluation does
not require pushing a new frame.

Walking the cases:

- **`(if t a b)`**: push `KONT_IF(then=a, else=b, parent=current)`,
  EVAL `t`. When `t`'s value comes back, APPLY of `KONT_IF` pops the
  frame and EVAL of `a` or `b` runs *with the parent restored as
  current*. `a`/`b` inherit `if`'s tail-ness automatically. `t` is
  non-tail; `a`/`b` are tail.
- **`(begin e1 e2 ... en)`**: push `KONT_SEQ(rest=[e2..en],
  parent=current)`, EVAL `e1`. When the rest list shrinks to one
  expression, pop the frame *before* evaluating it. `en` is tail;
  earlier exprs are not.
- **`(f x y)` in tail position**: when EVAL begins, push
  `KONT_APP(unev=[x,y], evald=[], parent=current)` — note `parent` is
  the kont active *before* the application started. As each argument
  evaluates, `evald` grows. After the last argument's value arrives,
  pop the `KONT_APP` *before* invoking the procedure body. The
  procedure runs with the parent kont as current — automatic TCO.

The invariant: every special form's eval rule is written so that the
sub-expression in tail position requires no new frame.

## Special forms

Special forms are recognized by symbol identity, not by string
comparison. Each form's symbol is interned at `Minit` time and cached
as a global root, mirroring the existing `quote_sym` pattern in
[src/symbol.c](../src/symbol.c). Cached symbols: `quote_sym`,
`if_sym`, `lambda_sym`, `set_bang_sym`, `define_sym`, `begin_sym`,
`callcc_sym`.

A pair whose car is one of these symbols is dispatched as the
corresponding special form. Anything else is treated as a procedure
application.

## Primitives and `call/cc`

### Primitive integration

Primitives are wrapped (`MSEC_PRIM`) for three reasons:

- First-classness — `(map + xs)` requires `+` to be a `mobj`.
- One APPLY path — closure, primitive, and continuation invocations
  share the path that pops `KONT_APP` and uses its `evald` list.
- Self-describing — the writer can print `#<procedure:name>` once a
  procedure-printing branch lands.

Primitive registration helper:
`prim_register("name", fn, min_arity, max_arity)` — interns the name,
allocates the prim object, installs it in the top-level env. Called
many times during `eval_init`.

Primitives execute synchronously (no allocation suspension): the C
function takes the evaluated argument list and returns a single
`mobj`. The eval loop transitions to APPLY with that value.

### `call/cc`

`call/cc` is itself a primitive. When invoked with a procedure `f`:

1. Take the current global `kont` (the parent of the just-popped
   `KONT_APP` for `call/cc` itself).
2. Hand it directly to `f` — no wrapper object, the kont mobj is the
   first-class value the user sees.

Invoking a captured continuation:
- Recognized by the APPLY path's procedure dispatch via `Mkontp`.
- Replaces the global `kont` with the captured one, replaces `expr`
  with the supplied value, switches to APPLY mode.
- The C-stack depth never changes; the spaghetti chain just gets
  rerooted.

This is the entire reason for the spaghetti-stack design: capture is
O(1), invocation is O(1), and the GC keeps captured continuations
alive without any special bookkeeping.

## Verification strategy

Standard tests (`tests/test_eval.c`, registered in
`tests/CMakeLists.txt`) follow the existing harness style: `Minit`,
run a sequence of `Meval` cases — many built by reading source via
`Mread` — compare results, `Mshutdown`.

Two correctness invariants get their own dedicated tests:

1. **Tail-call sanity**: a deep tail-recursive loop (1M iterations)
   must run with bounded heap. With TCO, the kont chain stays one
   frame deep at the recursive call site; without it, the chain grows
   per iteration. Run under stress mode for double the assurance —
   every allocation triggers a full collection.
2. **`call/cc` sanity**: capture a continuation in one expression,
   return from it, then re-invoke from a textually-distant context.
   The spaghetti-stack design either makes this trivial or breaks
   visibly — there is no middle ground.

The CI matrix already builds with `gc_stress: [OFF, ON]`. The new
test target is picked up automatically.

## References

The design draws on several well-known runtimes and papers:

- Felleisen et al., *A Programmer's Reduction Semantics* — the
  CESK abstract machine, of which this evaluator is a concrete
  variant. Our `(mode, expr_or_value, env, kont)` is exactly CESK
  with the store implicit in the heap.
- Clinger, Hartheimer, Ost, *Implementation Strategies for First-Class
  Continuations* — surveys spaghetti stacks vs. stack-copying vs.
  segmented stacks. The spaghetti choice is explicitly endorsed for
  precise-GC runtimes like ours.
- Chez Scheme's `eval-stack-link` and `c/syscalls.c:S_call_help` —
  Chez does **not** use a spaghetti stack (it copies stack segments
  on capture); we deliberately diverge here because the GC story is
  much simpler when frames are heap objects from the start.
- Racket's `chez-style` continuation implementation, which uses
  mark-stack-and-copy and is significantly more complex than what we
  need for v1.
