# Strings TODO

Tracker for adding strings to minim2. Implementation lives across
`include/minim.h` (tag, accessors), `src/alloc.c` (constructors),
`src/gc.c` (object_size/scan_fields arms), `src/read.c`
(`"..."` literals), `src/write.c` (write/display split), and
`src/prims.c` (string-aware primitives).

## Goal

Support strings as a first-class heap-managed value type, enough to
carry error messages, drive `display`, and host the bulk of R7RS
string operations.

## Design

- **Representation.** Typed object — primary tag `MTAG_TYPED_OBJ`
  (`0x7`), new secondary tag `MSEC_STRING = 0x4`. Chez does the
  same: strings aren't worth their own primary tag, since the
  hot-path predicate is rare relative to fixnum/pair/closure
  dispatch.
- **Header.** `(byte_count << 4) | MSEC_STRING`, identical shape
  to the vector header but with byte count in the upper bits. The
  GC's `object_size` arm for strings computes
  `8 + byte_count` rounded up to 16; `scan_fields` is a no-op
  (strings hold no mobjs).
- **Storage.** Bytes inline immediately after the header. No
  malloc-outside-GC; allocation is pure bump.
- **Encoding.** ASCII-only for v1. Bytes are 0x00..0x7F; passing
  a non-ASCII char to `make-string`/`string-set!` is `Merror`.
  Lifting to UTF-8 later is mostly a reader/writer/primitive
  change — the heap shape doesn't move. Documented as a
  limitation in this tracker and `docs/GC.md` Scope.
- **Mutability.** Every string is mutable; `string-set!` works on
  literals too. `string-immutable?` etc. deferred.
- **`write` / `display` split.** This is the natural time to
  separate them — strings are the only meaningful divergence
  today (chars already round-trip cleanly via the writer's
  canonical `#\<name>`/`#\x...` forms). `Mwrite` keeps current
  behavior; new `Mdisplay` is added alongside, and a
  `display`/`newline` primitive lands with the string primitives.

## Decisions

- **Encoding:** ASCII-only for v1.
- **Mutability:** all strings mutable.
- **`display` split:** add now, since strings make it meaningful.
- **Hex escapes (`\xHH;`):** deferred until a Unicode-aware reader
  pass.

## Phase 1 — heap representation
- [x] Added `MSEC_STRING = 0x4` to `include/minim.h`.
- [x] Predicate `Mstringp`, length `Mstring_length`, byte buffer
      `Mstring_bytes` (raw `char *`), char accessors
      `Mstring_ref(s, i)` / `Mstring_set(s, i, c)`. Char-as-byte
      conversion is the ASCII-only contract; the boundary that
      enforces ASCII is the `make-string` / reader layer above
      (still to come), not the C constructors.
- [x] `minim_string_size(byte_count)` helper in `gc.h` mirroring
      `minim_vector_size` — header word + bytes inline, padded to
      16.
- [x] `Mstring(length, fill_char)` and `Mstring_from_bytes(src,
      len)` constructors in `src/alloc.c`. No mobj args, so no
      `MINIM_GC_PROTECT` needed inside.
- [x] GC: `object_size` arm switches on the secondary tag — for
      `MSEC_STRING` it reads byte count and returns
      `minim_string_size`; for everything else, slot count and
      `minim_vector_size`. Scan-loop size computation gets the
      same fork. `scan_fields`'s `MTAG_TYPED_OBJ` branch
      short-circuits for `MSEC_STRING` since strings hold no
      mobjs (walking them as slots would interpret bytes as
      tagged words and corrupt the heap).
- [x] `MTAG_MASK` / immediate-tag invariants unchanged — strings
      ride the typed-obj path.
- [x] `tests/test_alloc.c`: `test_string_smoke` covers empty
      string, fill, set/ref round-trip, `from_bytes`, and
      distinct allocations; `test_string_disjoint` confirms only
      `Mstringp` fires and `Mprocedurep` doesn't.
      `tests/test_gc.c`: `test_string_survives_gc` checks
      length / contents preservation across direct collection,
      string-in-vector slots after collection, and resilience to
      heap churn.
- [x] Default + stress pass.

## Phase 2 — reader
- [ ] `"..."` literal recognition in `src/read.c`.
- [ ] Escapes: `\\`, `\"`, `\n`, `\t`, `\r`. Anything else after
      a backslash is `Merror`.
- [ ] Tests in `tests/test_parser.c` covering empty string,
      simple ASCII, every supported escape, and unterminated /
      bad-escape error paths.
- [ ] Default + stress pass.

## Phase 3 — writer
- [ ] Split: keep `Mwrite` (canonical / readable form), add
      `Mdisplay(v, FILE *)` (raw form). Internally factor through
      a single helper that takes a mode flag.
- [ ] String formatting:
      - `write` mode: `"..."` with `\\`, `\"`, `\n`, `\t`, `\r`
        escapes; control chars below 0x20 render as `\x%02X;` (or
        a TODO if we defer `\xHH;` reader-side too — for v1 they
        can render as `?` since round-tripping isn't required for
        non-ASCII bytes that can't enter a string anyway).
      - `display` mode: raw bytes verbatim.
- [ ] Tests in `tests/test_writer.c`:
      - `write` round-trips for plain + escape-heavy strings.
      - `display` emits raw content (capture via `open_memstream`).
- [ ] Default + stress pass.

## Phase 4 — primitives
- [ ] Predicates / measurement: `string?`, `string-length`.
- [ ] Construction: `make-string` (1 or 2 args), `string`
      (varargs of chars), `string-copy`.
- [ ] Indexing: `string-ref`, `string-set!`. Bounds are not
      checked at this layer per the "no type checking" rule
      already in `prims.c`.
- [ ] Comparison: `string=?`. (Lex `<?`/`>?` etc. deferred.)
- [ ] Conversion: `string->list`, `list->string`,
      `string->symbol`, `symbol->string`. The symbol-side
      converters bridge to the existing intern table.
- [ ] Concat: `string-append` (varargs).
- [ ] I/O: `display`, `newline`. `display` goes to stdout.
- [ ] Tests in `tests/test_eval.c`. Default + stress pass.

## Phase 5 — doc cleanup
- [ ] `docs/GC.md`: Scope lists strings; `MTAG_TYPED_OBJ` row
      mentions `MSEC_STRING` alongside vector/kont/env/prim.
- [ ] `docs/todos/parser.md`: Phase 4 strings item ticked, with
      a one-line summary of the supported escapes.
- [ ] `docs/todos/writer.md`: Phase 3 string item ticked; the
      "out of scope" note about `display` divergence updated to
      reflect the split having landed.
- [ ] `docs/todos/eval.md`: Phase 6 string-aware primitives
      ticked off.

## Out of scope for v1

- Non-ASCII characters in strings. `Mstring_set` of a codepoint
  > 0x7F is `Merror`; the reader rejects bytes ≥ 0x80 in literals.
  Lifting is mostly a reader/writer/primitive change.
- Hex / Unicode escapes (`\xHH;`, `\uNNNN`).
- Immutable string distinction.
- Lexicographic comparison primitives beyond `string=?`.
- String ports (`open-input-string`, `with-output-to-string`).
- `string-fill!`, `string-for-each`, `string-map`.
- Bytevectors `#u8(...)` — adjacent feature, not part of this
  effort.
