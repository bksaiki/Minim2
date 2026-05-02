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

- [ ] In `Mwrite`'s immediate switch (`src/write.c`), add an arm
      that fires when `Mcharp(v)` is true.
- [ ] Spelling rules:
      - If the codepoint is in the R7RS named-char set,
        emit `#\<name>`. Specifically: 0x00 → `#\null`,
        0x07 → `#\alarm`, 0x08 → `#\backspace`, 0x09 → `#\tab`,
        0x0A → `#\newline`, 0x0D → `#\return`, 0x1B → `#\escape`,
        0x20 → `#\space`, 0x7F → `#\delete`.
      - Else if the codepoint is in printable ASCII (33–126,
        inclusive of `!`–`~`), emit `#\<single>`.
      - Else emit `#\x<hex>` with no `0` padding (so `0x0C` →
        `#\xC`).
- [ ] Tests in `tests/test_writer.c`:
      - Each canonical name's output verified.
      - Printable-ASCII writes as `#\X`.
      - Codepoints outside the named set and outside printable ASCII
        write as `#\x...` (e.g. `#\xC` for FF, `#\x1F600` for the
        emoji).

## Phase 4 — round-trip

The reader and writer must agree on the canonical forms, so any
char written by `Mwrite` must be readable back by `Mread` to the
same value.

- [ ] Tests in `tests/test_writer.c` (extending the existing
      `test_roundtrip`):
      - `#\A`, `#\a`, `#\space`, `#\newline`, `#\null`,
        `#\backspace`, `#\delete`, `#\escape` round-trip.
      - `#\x1F600` round-trips via the hex form.

## Phase 5 — doc / cross-reference cleanup

- [ ] Update the `MTAG_IMMEDIATE` row in `docs/GC.md` to list chars
      (low byte `0x16`, codepoint in upper bits).
- [ ] Tick off the "Characters" item in `docs/agents/parser.md`
      Phase 4 and `docs/agents/writer.md` Phase 4. Both currently
      say "needs the char type" — once the type lands, the syntax
      handlers from this doc satisfy them.
- [ ] Cross-link this doc from `docs/EVAL.md` Scope, since chars
      become a sixth supported value type.

## Phase 6 — primitive procedures (NOT implemented as part of this work)

The following char-aware primitives need to exist eventually but
belong with the rest of the primitive work in
`docs/agents/eval.md` Phase 6 (primitives). Listed here only as a
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
