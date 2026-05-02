# Characters TODO

Tracker for adding the character (`mchar`) value type to minim2.
Characters are immediates — no heap allocation, identical to other
immediate values in their GC treatment.

The encoding follows Chez Scheme: low byte holds the tag, the upper
24+ bits hold the Unicode codepoint. The predicate is a single
8-bit mask.

## Encoding

| Operation       | Definition                                          |
|-----------------|-----------------------------------------------------|
| `Mchar(c)`      | `(mobj)(((uintptr_t)(c) << 8) \| 0x16)`             |
| `Mcharp(v)`     | `((v) & 0xFF) == 0x16`                              |
| `Mchar_val(v)`  | `(mchar)((v) >> 8)`                                 |

Why `0x16` for the low byte:

- Bits 0-2 are `110` = `MTAG_IMMEDIATE`, so the GC's existing
  `minim_is_leaf` test naturally covers chars (no tracing).
- Bits 3-7 form an "immediate subtag" (5 bits = 32 values). With
  the existing immediates' shifted layout, subtag values 0/1/4/5/6/7
  are taken (`#f`/`#t`/`()`/void/eof/forward-marker). Subtag `2`
  (the value `0b00010`, i.e. `0x10`) is unused — combined with the
  primary tag this gives `0x16`. Picking `2` matches Chez's
  `type_char = 0x16` exactly.
- The 8-bit-boundary tag check is one `and` + one `cmp`. The upper
  24 bits stay completely clean, so codepoint extraction is one
  shift with no mask.

A 32-bit codepoint comfortably holds anything Unicode reaches
(currently ≤ 0x10FFFF, 21 bits). No need for surrogate pairs or
multi-word encoding.

### Distinctness checks

- `Mcharp(c) → true` for any `c`; `Mcharp(Mtrue)` etc. are false
  because their low bytes are `0x06`, `0x0E`, `0x26`, `0x2E`,
  `0x36`, `0x3E`.
- `Mbooleanp(c) = (v & 0xF7) == 0x06` — for any char `v`, the low
  byte after the mask is still `0x16` (the `0x10` bit isn't cleared
  by `0xF7`), so chars never accidentally match the boolean check.
- Heap pointers carry primary tags 1/2/3/5/7, never `MTAG_IMMEDIATE`,
  so no char value ever collides with a real heap address.
- `MFORWARD_MARKER = 0x3E` ≠ `0x16`; no GC confusion.

## Phase 1 — runtime representation

- [x] Added `MCHAR_TAG = 0x16`, `Mchar(c)`, `Mchar_val(v)`,
      `Mcharp(v)` to `include/minim.h`. The encoding follows Chez:
      low byte is the type tag, codepoint sits in the upper bits.
- [x] `is_self_evaluating` and `minim_is_leaf` already cover chars
      via their existing `Mimmediatep` / `MTAG_IMMEDIATE` arms — no
      change required.
- [x] Tests in `tests/test_tags.c`: 12-codepoint round-trip
      (including `0x00`, `'A'`, `0x7F`, `0xFFFF`, `0x10000`, 😀,
      `0x10FFFF`), disjointness from every other predicate
      including `Mbooleanp`, and a direct check of the bit
      encoding. Default and stress configs both pass.

## Phase 2 — reader

- [x] Added `#\<spec>` handling to `src/read.c`. `read_char`
      consumes the spec and dispatches:
      - Length 1 (or first char non-alphabetic) → that single
        character. `#\(`, `#\1`, `#\!` all parse to their literal
        codepoint without trying to extend into a name.
      - `xHHHH` with all hex digits → Unicode hex literal. Range-
        checks against `0x10FFFF`.
      - Otherwise looks up the R7RS named-char table (`alarm`,
        `backspace`, `delete`, `escape`, `newline`, `null`,
        `return`, `space`, `tab`).
      - Unknown name or malformed spec → `Merror`.
- [x] A delimiter must follow the spec; otherwise `Merror`. This
      makes `#\(a` an error (the `a` after the literal `(` isn't a
      delimiter), forcing the user to whitespace-separate.
- [x] Tests in `tests/test_parser.c`: every R7RS named char (9
      cases), a representative spread of single chars (including
      ones that could be confused with name prefixes — `#\a`,
      `#\n`, `#\s`, `#\t`, `#\x` — plus delimiter-shaped chars
      like `#\(`/`#\)`/`#\.`), 9-case hex range up to `#\x10FFFF`,
      and a recoverable-error suite covering hex-not-a-name (`xZZ`),
      unknown names (`bogus`), Chez aliases that R7RS doesn't have
      (`esc`/`nul`/`linefeed`), Unicode overflow, EOF after `#\`,
      and missing delimiter. Default and stress configs both pass.
- Note: the writer still emits `#<unknown-immediate>` for chars in
      `Mwrite`'s switch — Phase 3 adds the proper output arm.

## Phase 3 — writer

- [x] Added a `write_char` helper to `src/write.c` and an
      `Mcharp(v) → write_char(v, out)` short-circuit at the top of
      the `MTAG_IMMEDIATE` arm. Spelling:
      - 9 R7RS named-char codepoints → `#\<name>`.
      - Printable ASCII 0x21–0x7E (excluding 0x20/0x7F, which are
        named) → `#\<single>`.
      - Otherwise → `#\x%X` (uppercase hex, unpadded; the reader
        accepts mixed case).
- [x] Tests in `tests/test_writer.c`: each canonical name (9), a
      printable-ASCII spread including boundaries (0x21 / 0x7E)
      and confusables (`a`, `x`, `(`, `)`), and hex output for
      out-of-range codepoints up to `0x10FFFF`.

## Phase 4 — round-trip

- [x] `test_char_roundtrip` in `tests/test_writer.c` covers:
      - All 9 named chars round-trip identically.
      - Printable single chars (`#\A`, `#\a`, `#\(`) identity.
      - `#\x1F600` (emoji) identity via hex.
      - Hex-case normalization: `#\xff` → `#\xFF` (uppercase).
      - Hex-to-name normalization where applicable: `#\x41` →
        `#\A`, `#\x20` → `#\space`, `#\xA` → `#\newline`. Reader
        accepts hex form, writer prefers the canonical name or
        printable-ASCII spelling — both halves agree on what the
        canonical form is. Default and stress configs both pass.

## Phase 5 — doc / cross-reference cleanup

- [x] Updated the `MTAG_IMMEDIATE` row in `docs/GC.md` to mention
      chars (low byte `0x16`, codepoint in upper bits) and added a
      char-encoding block alongside the existing `Mimmediate` block.
      The "Why this layout" bullet that listed characters as a
      future addition now treats them as present.
- [x] `docs/todos/parser.md` Phase 4 character item ticked off,
      with a one-line summary of the disambiguation rule. Chez
      aliases (`esc`, `linefeed`, `nul`, etc.) explicitly removed
      from the named-char list — only R7RS canonical names are
      accepted.
- [x] `docs/todos/writer.md` Phase 4 character item ticked off,
      with the canonical-name spelling rules.
- [x] `docs/EVAL.md` Scope updated: characters now appear in the
      "value types supported in v1" list with a link to this doc;
      the "out of scope" line drops the `characters` item.

## Phase 6 — primitive procedures (NOT implemented as part of this work)

The following char-aware primitives need to exist eventually but
belong with the rest of the primitive work in
`docs/todos/eval.md` Phase 6 (primitives). Listed here only as a
reference for what the type ultimately needs to support; do not
add them as part of the chars feature itself.

- [ ] `char?` — predicate.
- [ ] `char=?`, `char<?`, `char>?`, `char<=?`, `char>=?` —
      comparison.
- [ ] `char->integer`, `integer->char` — codepoint conversion.
- [ ] `char-upcase`, `char-downcase`, `char-alphabetic?`,
      `char-numeric?`, `char-whitespace?` — proper Unicode requires
      tables; v1 can ship ASCII-only and document the limitation.

## Out of scope for v1

- Strings — a separate effort; will reference but not block char
  work.
- `#!fold-case` reader directive.
- Locale-dependent comparison.
- Source-position tracking on char tokens.
- Reader extensibility (`define-reader-ctor` etc.).
