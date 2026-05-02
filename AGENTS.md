# AGENTS.md

Guidance for AI agents working in this repository. Read this before
making changes.

## Orient yourself first

1. Skim [`README.md`](README.md) for build/test commands.
2. Read [`docs/GC.md`](docs/GC.md) and [`docs/EVAL.md`](docs/EVAL.md)
   — the runtime's tag layout, allocation discipline, and evaluator
   shape are non-obvious and easy to break silently.
3. If the task touches a tracked feature (parser, writer, chars,
   eval), read its tracker under [`docs/todos/`](docs/todos/) and
   keep it up to date as you land work.

## Build and test workflow

Always build and run tests in **both** configurations before declaring
a task done:

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-stress -DMINIM_GC_STRESS=ON && cmake --build build-stress && ctest --test-dir build-stress --output-on-failure
```

`MINIM_GC_STRESS=ON` collects on every allocation. It is the primary
way unprotected `mobj` C locals get caught — a default-config pass is
not sufficient evidence that GC discipline is correct.

## GC discipline (read this twice)

The runtime is a precise copying collector with shadow-stack roots.
Any `mobj` held in a C local across an allocation must be protected,
or it will silently move out from under you.

- Use `MINIM_GC_FRAME_BEGIN(n)` / `MINIM_GC_PROTECT(slot, var)` /
  `MINIM_GC_FRAME_END()` for ephemeral locals.
- The four evaluator state words (`eval_expr`, `eval_env`, `eval_kont`,
  `top_level_env`) are registered globals — write to them **before**
  the next allocation, and bare locals copied off them go stale at
  the next `Mcons`/`Mvector`/`Mkont_*` etc.
- New heap kinds need entries in `object_size` and `scan_fields` in
  `src/gc.c`. Forward through `forward_tagged`, not bare `forward`.
- New value tags need predicates that don't accidentally match
  existing ones. See `docs/todos/chars.md` for how the char tag
  (`0x16`) was placed without colliding with booleans.

When in doubt: add the test, run it under stress, and only then trust
the change.

## Style and scope

- Match the surrounding code. C11, no GNU extensions
  (`CMAKE_C_EXTENSIONS=OFF`).
- Don't add features, helpers, or backwards-compat shims beyond what
  the task requires. The runtime is intentionally small.
- Don't write comments that restate the code. Reserve comments for
  non-obvious *why* (a tag-collision invariant, a stress-mode
  hazard, a deviation from R7RS).
- Don't create new docs unless the task warrants one (a new
  multi-phase feature gets a tracker; a bug fix does not).
- Single-letter naming conventions: `M`-prefixed for runtime API
  (`Mcons`, `Mread`); `m`-prefixed for value-type names (`mobj`,
  `mchar`); `MTAG_`/`MSEC_` for primary/secondary tags.

## Working with trackers

When implementing a phase from `docs/todos/<feature>.md`:

1. Land one phase at a time.
2. Tick the phase's items only after both build configs pass.
3. Update the tracker's intro/Scope sections if the feature changes
   what the runtime supports — stale "deferred" language is a common
   review finding.
4. Cross-reference: chars-aware primitives belong to
   `eval.md` Phase 6, not the chars tracker. Keep cross-doc claims
   consistent.

## Things to avoid

- **Don't bypass hooks or skip stress runs to make a task "done".**
  A stress failure is a real bug, not noise.
- **Don't introduce silent fallbacks.** If `()` evaluates to `()`,
  if internal `define` mutates the top-level, if a malformed
  character literal returns `#f` — these are bugs, not features.
  Surface as `Merror`.
- **Don't store function pointers in fixnum-shifted words.**
  Function-pointer alignment is not ABI-guaranteed; use an index
  into a malloc'd table.
- **Don't add Chez or implementation-specific aliases** to the
  reader/writer. R7RS canonical forms only unless the user asks
  otherwise.

## Where to look when stuck

- The git log: `git log --oneline -- <path>` is usually the fastest
  way to recover the *why* behind a non-obvious choice.
