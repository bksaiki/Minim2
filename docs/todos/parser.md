# Parser TODO

Tracker for the s-expression reader. Source of truth for the legacy
implementation: `old_parser.c` at the project root. The new parser lives
in `src/read.c` with its public API in `include/minim.h`.

The runtime supports six concrete types — fixnum, pair, flonum,
symbol, vector, character — plus five immediates (`#t`, `#f`, `'()`,
`eof`, `void`). Reader features that depend on types we haven't built
yet (strings, boxes) are intentionally deferred and listed below.

## Phase 1 — input source abstraction
- [x] `mreader` struct that wraps either a C string or a `FILE *`
- [x] `mreader_init_string`, `mreader_init_file`
- [x] One-character peek/get with lookahead state inside the reader

## Phase 2 — features whose types already exist
- [x] Skip whitespace, line comments (`;`), block comments (`#|...|#`),
      datum comments (`#;`)
- [x] Booleans `#t` / `#f`
- [x] Decimal fixnums with optional sign
- [x] Hex fixnums `#x...` with optional sign
- [x] Lists with `(...)`, `[...]`, `{...}` and matching-paren check
- [x] Dotted pairs `(a . b)` (the `.` must sit between delimiters)
- [x] Vectors `#(...)`
- [x] Quoted expressions `'expr` desugar to `(quote expr)`
- [x] Symbols (interned), including the `#%name` "system symbol" syntax
- [x] EOF returns the `Meof` immediate
- [x] Reader-state shutdown hook resets the cached `quote` symbol

## Phase 3 — runtime supports the type, parser does not yet
- [ ] Flonums (`1.5`, `1e10`, `.5`, `+inf.0`, `-nan.0`) — runtime has
      `Mflonum` but no syntax recognition.
- [ ] Quasiquote `` ` ``, unquote `,`, unquote-splicing `,@` desugaring.

## Phase 4 — features blocked on missing types
These cannot be implemented until the corresponding runtime type lands.
The legacy `old_parser.c` already has the syntax handling and can be
ported once the type exists.

- [x] Characters `#\<name>` / `#\<single>` / `#\xHH...` — landed
      via `docs/todos/chars.md`. R7RS named chars only: `alarm`,
      `backspace`, `delete`, `escape`, `newline`, `null`, `return`,
      `space`, `tab`. Hex form accepts up to `0x10FFFF`. Disambig:
      first non-alphabetic char or first-only-then-delimiter is a
      single-char form; otherwise read until delimiter and match
      hex (`x[0-9a-fA-F]+`) or named.
- [ ] Strings `"..."` with `\n`/`\t`/`\\`/`\'`/`\"` escapes — needs a
      mutable string type.
- [ ] Boxes `#&datum` — needs a box type. (Commented out in legacy.)
- [ ] Hex/Unicode escapes inside strings (`\xNN;`, `\uNNNN`).

## Phase 5 — error handling and ergonomics
- [ ] Track line/column inside `mreader` and surface them on errors,
      instead of `abort()` with a bare message.
- [ ] Quoted symbol form `|...|` (R7RS) for symbols whose names
      contain delimiters, whitespace, or otherwise collide with other
      tokens. Counterpart to the writer's escape-emit work in
      `writer.md` — both ends must agree before round-tripping
      arbitrary symbol names is safe. Internal `|` and `\` escape as
      `\|` and `\\`.
- [ ] Surface parse errors as a Scheme condition once we have an
      exception mechanism, rather than aborting the runtime.
- [ ] Symbol max length is currently a fixed 256-byte buffer; consider a
      growable buffer or an explicit limit constant.
- [ ] Configurable case-folding for symbols (R7RS `#!fold-case`).

## Out of scope for now
- Reader macros / `read-syntax` / source-tracking objects.
- Bytevectors `#u8(...)`.
- Numeric tower: ratios, complex, bignums, exact/inexact prefixes.
- `#;`-style nesting beyond a single datum.
