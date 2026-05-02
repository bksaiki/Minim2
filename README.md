# Minim2

A Scheme implementation. Successor to the original `minim` prototype,
rebuilt around a precise copying GC, a state-machine evaluator with
spaghetti-stack continuations, and an incremental, type-by-type
runtime.

## Building

Requires CMake ≥ 3.15 and a C11 compiler.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The runtime is packaged as a static library (`libminim_rt.a`); the
`minim` REPL binary and the test executables all link against it.

### GC stress mode

`MINIM_GC_STRESS=ON` triggers a full collection on every allocation.
Used to flush out unprotected `mobj` C locals. CI runs both:

```sh
cmake -B build-stress -DMINIM_GC_STRESS=ON
cmake --build build-stress
```

## Running the REPL

```sh
./build/minim
```

`Ctrl-D` exits. Errors print a message and return to the prompt.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Test suites live in `tests/`:

| target          | covers                                              |
|-----------------|-----------------------------------------------------|
| `test_tags`     | pointer-tag layout and predicates                   |
| `test_alloc`    | constructors, type round-trips                      |
| `test_gc`       | collection correctness, forwarding, root scanning   |
| `test_parser`   | reader (`Mread`)                                    |
| `test_writer`   | writer (`Mwrite`) and reader/writer round-trip      |
| `test_eval`     | evaluator end-to-end                                |

Every test must pass under both default and stress configurations.

## Layout

```
include/   public headers (minim.h, gc.h) and internal.h
src/       runtime sources — alloc, gc, read, write, symbol, eval, system, main
tests/     ctest-driven test suites
docs/      design docs and per-feature TODO trackers (see docs/README.md)
```

## Documentation

Design docs and per-feature trackers are under [`docs/`](docs/). Start
at [`docs/README.md`](docs/README.md). AI agents working in this repo
should also read [`AGENTS.md`](AGENTS.md).
