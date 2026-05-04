# Imports & Core Library TODO

Tracker for the module/import mechanism and the bundled core
Scheme library. Implementation lives mostly in `src/eval.c` (or a
split-out `src/module.c` if it grows); the core library lives at
`lib/core.scm` and is embedded in the runtime binary at build time.

Cross-references: `docs/EVAL.md` ("Modules and imports", added in
Phase 4) and `docs/todos/eval.md` (the pair accessors land as
library code, not Phase 6 primitives).

## Goal

`(import (prefix #%kernel $))` exposes kernel primitives with a
`$` prefix. The bundled core library uses this to define R7RS pair
accessors (`caar`/`cadr`/.../`cddddr`) and to re-export kernel
primitives under their canonical names so existing user code keeps
working.

The kernel module holds primitives; user-visible names enter the
top-level env via `import`. The core library, auto-loaded during
`Minit`, is the bootstrap that re-establishes conventional names
plus the new accessors.

## Design

- **Kernel module representation.** A single global `kernel_env`,
  alist-shaped like `top_level_env` and registered as a GC root.
  Primitives flow in via `prim_register`. Until Phase 3 cuts over,
  `prim_register` *also* populates `top_level_env`, so behavior
  stays identical and existing tests don't break — Phase 3 deletes
  that line once `lib/core.scm` re-exports the canonical names.
- **Module references.** `#%kernel` is a system symbol (the parser
  already accepts `#%name`). Resolution is a switch on the
  symbol's identity — until other modules exist, only `#%kernel`
  is recognized; everything else is `Merror`.
- **Import special form.** Recognized in `step_eval` like
  `define`/`set!`/`if`. For v1 only `(import (prefix module-ref
  prefix-sym))` is accepted; other shapes are `Merror`. Top-level
  only — body-position imports are rejected like internal `define`
  is today (`Menvp(eval_env)` check). Result is `Mvoid`.
- **Core library bundling.** `lib/core.scm` is read at configure
  time and embedded in the binary as a `static const char[]`. A
  build-time CMake step (`file(READ ...)` plus a tiny
  `configure_file` template) writes a generated header into the
  build directory; runtime sources `#include` it.
- **Bootstrap order.** `Minit` runs `gc_init` → `symbol_init` →
  `eval_init` (which calls `prims_register_all`, populating
  `kernel_env`) → `core_lib_load` (reads/evals every form in the
  embedded string). A failure during core-lib load aborts; this
  is bootstrap, not user code.

## Decisions

- **Compat:** core.scm re-exports every kernel primitive under its
  canonical name. Existing tests stay unchanged.
- **Variants:** bare `(import M)` and `(prefix M P)` for v1.
  Multiple specs in a single `import` form are allowed (R7RS).
  `only`/`except`/`rename` are out of scope.
- **Embedding:** build-time, embedded as a C string. No runtime
  file loading.
- **Body imports:** top-level only.

## Phase 1 — kernel env
- [x] Add `kernel_env` global in `src/eval.c`; register with
      `minim_protect` in `eval_init`; reset in `eval_shutdown`.
- [x] Helpers `kernel_env_define(sym, val)` and
      `kernel_env_lookup(sym, *out)` mirroring the `top_env_*`
      pair. `kernel_env_lookup` is unused until Phase 2 wires up
      import; it sits as `static` and the compiler is fine with
      that (no `-Werror=unused-function`).
- [x] `prim_register` populates both `kernel_env` and
      `top_level_env`. The `top_env_define` call here goes away
      in Phase 3.
- [x] No new tests this phase — the change is purely additive
      (dual population means top_level_env behaves identically).
      The existing suite passing is the contract.
- [x] Default + `MINIM_GC_STRESS=ON` configs both pass.

## Phase 2 — `import` special form
- [x] Cache `import_sym`, `prefix_kw_sym`, and `kernel_module_sym`
      in `src/symbol.c` alongside the other special-form symbols.
      Identity comparison handles module-ref / spec-keyword
      matching with no strcmp at runtime.
- [x] Recognize `(import spec ...)` in `step_eval`. Each spec is
      either a bare module-ref symbol (`(import M)`) or a prefixed
      import (`(prefix M P)`); multi-spec is allowed. Everything
      else is `Merror`. Body-position imports rejected via the
      same `Menvp(eval_env)` check used for internal `define`.
- [x] Module resolution: hardcoded identity check against
      `kernel_module_sym`; everything else is `Merror`.
- [x] `do_import(module_ref, prefix_str)` walks the kernel env
      and installs each binding into `top_level_env`. With
      `prefix_str == NULL` (bare form) names are kept unchanged;
      otherwise each name is `Mintern`'d as `<prefix><name>` and
      installed via `top_env_define`. Symbol-name pointers are
      stable across GCs (intern table owns the malloc'd storage),
      so the prefix string can be read once before the loop;
      `cur`/`val`/`installed_sym` ride the protect stack across
      the per-iteration allocations.
- [x] New `tests/test_import.c` covering: basic prefix import
      (`$car`, `$+`, etc.), arbitrary prefix symbols (`k:`, `_`),
      idempotent re-import, body-position rejection, bare import
      (`(import #%kernel)`), multi-spec (`(import M (prefix M P))`),
      unknown module rejection in both forms, and a battery of
      malformed-spec cases (zero specs, non-symbol-non-pair spec,
      non-prefix keyword, wrong-length prefix spec, non-symbol
      module-ref, non-symbol prefix).
- [x] Default + stress pass.

## Phase 3 — core library
- [ ] `lib/core.scm`:
      - `(import (prefix #%kernel $))` first.
      - One `(define <name> $<name>)` line per kernel primitive.
      - `(define (caar p) ($car ($car p)))` ... up to `cddddr`
        (28 procedures: every `c[ad]+r` of length 2..4).
- [ ] Build-time embedding. CMake reads `lib/core.scm` and emits
      `core_lib_data.h` containing
      `static const char minim_core_lib[] = "...";`. Runtime
      `#include`s the generated header. Build target depends on
      the source `.scm` so editing it triggers a rebuild.
- [ ] `core_lib_load(void)` in `src/system.c`: drive
      `Mread`/`Meval` over the embedded string until `Meof`.
      Called from `Minit` after `eval_init`. Errors abort.
- [ ] Drop `top_env_define` from `prim_register`. Kernel env is
      now the only home for primitives; `core_lib_load` is what
      populates `top_level_env` with the canonical names.
- [ ] Tests in `tests/test_eval.c`: the R7RS pair accessors at
      depths 2/3/4, plus a few compositional cases (e.g. `(cadr
      '(1 2 3))` → 2, `(cddr '(1 2 3 4))` → `(3 4)`).
- [ ] Default + stress pass.

## Phase 4 — docs cleanup
- [ ] `docs/EVAL.md`: short "Modules and imports" section pointing
      here.
- [ ] `docs/todos/eval.md`: note that pair accessors are library
      code, not Phase 6 primitives. The Phase 6 "Pair / list" line
      stays as is — `cons`/`car`/`cdr`/`list` remain primitives.

## Out of scope for v1

- `only`/`except`/`rename` import forms.
- Module references that aren't system symbols (e.g. `(scheme
  base)`).
- User-defined modules / `define-library` / `export`.
- Body-position imports.
- Loading from a path (a `load` primitive, file-relative imports).
- Re-export forms in user libraries.
- Hygienic macro layer that would normally accompany imports.
- A proper "module" heap-object kind. The kernel is just an alist;
  any user-defined modules would warrant typing it.
