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

- [ ] Add the three `Mchar` / `Mcharp` / `Mchar_val` definitions to
      `include/minim.h` next to the existing immediates. The
      `mchar` typedef already exists (`unsigned int`).
- [ ] Confirm `is_self_evaluating` already covers chars via the
      `Mimmediatep` arm — no change expected.
- [ ] Confirm `minim_is_leaf` (in `src/gc.c`) already returns true
      for chars — no change expected.
- [ ] Tests in `tests/test_tags.c`:
      - `Mchar(c)` round-trip via `Mchar_val` for codepoints
        spanning ASCII printables, control characters, and a
        non-ASCII codepoint (e.g. 0x1F600).
      - `Mcharp` true for char values, false for `Mtrue`/`Mfalse`/
        `Mnull`/`Mvoid`/`Meof`/`MFORWARD_MARKER`/fixnums/heap
        pointers (use a fresh pair).
      - `Mbooleanp(some_char)` is false.
      - `Mchar('A') != Mtrue` (and similar) — distinctness.

## Phase 2 — reader

The reader currently has zero `#\` handling. The lexer already
treats `\` as a non-delimiter symbol char, but `#` followed by `\`
is not yet recognized as the char prefix.

- [ ] `#\<single>` form: any single character followed by a
      delimiter. `#\A` → codepoint 0x41, `#\(` → 0x28.
- [ ] Named chars per R7RS, recognized by reading all subsequent
      symbol-chars and matching the result:

      | name        | codepoint |
      |-------------|----------:|
      | `alarm`     |      0x07 |
      | `backspace` |      0x08 |
      | `delete`    |      0x7F |
      | `escape`    |      0x1B |
      | `newline`   |      0x0A |
      | `null`      |      0x00 |
      | `return`    |      0x0D |
      | `space`     |      0x20 |
      | `tab`       |      0x09 |

- [ ] `#\xHH...` hex form (R7RS): `#\x41` → `A`, `#\x10FFFF` → max
      Unicode. Variable-length hex digits, terminated by a
      delimiter. Errors on overflow past `0x10FFFF`.
- [ ] Disambiguation: when `#\` is followed by a single symbol-char
      that's then immediately a delimiter, it's a single-char.
      Otherwise read the whole token and match either a hex literal
      (starts with `x`) or a named char. Unknown → `Merror`.
- [ ] Tests in `tests/test_parser.c`:
      - Each R7RS named char parses to its codepoint.
      - `#\A`, `#\a`, `#\0`, `#\(`, `#\)` — single-char cases that
        could otherwise be confused with named-char prefixes.
      - `#\x41`, `#\xFF`, `#\x10FFFF` — hex form across a range.
      - `#\xZZ`, `#\bogus` — error paths via the `Merror` longjmp
        pattern used by `test_let_unbound_error`.

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
