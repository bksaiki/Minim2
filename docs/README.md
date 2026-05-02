# Minim2 Documentation

Design docs describe what we built and why. Per-feature trackers
under [`todos/`](todos/) carry the phased TODO checklists used to
land each feature incrementally.

## Design

- [`GC.md`](GC.md) — pointer tagging, allocator, copying GC, shadow
  stack, root protection.
- [`EVAL.md`](EVAL.md) — tree-walking evaluator: state-machine loop,
  spaghetti-stack continuations, environments, closures, primitives,
  `call/cc`.

## Feature trackers

Each tracker is a phased checklist with a "source of truth" reference
to the legacy implementation it replaces.

- [`todos/parser.md`](todos/parser.md) — s-expression reader.
- [`todos/writer.md`](todos/writer.md) — s-expression writer.
- [`todos/chars.md`](todos/chars.md) — character (`mchar`) type.
- [`todos/eval.md`](todos/eval.md) — evaluator phases.

## Conventions

- A feature gets a tracker when it spans multiple phases or touches
  more than one subsystem. Single-file fixes don't need one.
- Phases mark items `[x]` only when both default and stress builds
  pass with the new code.
- Items deferred until another type or subsystem lands are listed in
  the tracker rather than dropped, so the design context survives.
