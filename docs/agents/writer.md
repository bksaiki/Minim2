# Writer TODO

Tracker for the s-expression printer. Source of truth for the legacy
implementation: `old_writer.c` at the project root. The new writer lives
in `src/write.c` with its public API in `include/minim.h`.

The runtime in v1 supports six concrete types — fixnum, pair, flonum,
symbol, vector — plus four immediates (`#t`, `#f`, `'()`, `eof`). Writer
features that depend on types we haven't built yet (characters, strings,
procedures, ports, modules) are intentionally deferred.

## Phase 1 — output sink
- [x] `Mwrite(mobj, FILE *)` writes one datum to a stream
- [x] Tests capture output via `open_memstream` and `strcmp`

## Phase 2 — features whose types already exist
- [x] Immediates: `#t`, `#f`, `()`, `#<eof>`
- [x] Fixnums: decimal, signed
- [x] Flonums: shortest reasonable form, with `.0` suffix for
      integer-valued doubles, plus `+inf.0` / `-inf.0` / `+nan.0`
- [x] Symbols: bare name (no `|...|` quoting yet)
- [x] Lists / pairs: proper and improper, with `.` between cdr and tail
- [x] Vectors: `#(...)`
- [x] Round-trip with `Mread`: writing a parsed datum and re-parsing it
      produces an `eq?`/structurally-equal value for the supported types

## Phase 3 — runtime supports the type, writer does not yet
- [ ] `display` mode for upcoming string/char types — currently the API
      is `write`-style only; once strings exist we need `Mdisplay` or a
      mode flag.
- [ ] Quote/quasiquote/unquote-splicing pretty form: write `(quote x)`
      back as `'x`, `(quasiquote x)` as `` `x ``, etc.

## Phase 4 — features blocked on missing types
These cannot be implemented until the corresponding runtime type lands.
The legacy `old_writer.c` already has the syntax for each.

- [x] Characters — landed via `docs/agents/chars.md`. Writer
      emits the R7RS canonical name for `0x00`/`0x07`/`0x08`/`0x09`
      /`0x0A`/`0x0D`/`0x1B`/`0x20`/`0x7F` (`null`, `alarm`,
      `backspace`, `tab`, `newline`, `return`, `escape`, `space`,
      `delete`), printable ASCII (`0x21`–`0x7E` excluding the named
      ones) as `#\<single>`, otherwise `#\x%X` uppercase unpadded.
- [ ] Strings `"..."` with escapes (`\n`, `\t`, `\\`, `\"`, etc.).
- [x] Procedures: `#<procedure>` for anonymous closures, plus
      `#<procedure:name>` for closures whose name slot is bound and
      for primitives (which always carry their name). Continuations
      print as `#<continuation>` for now.
- [ ] Ports: `#<input-port>`, `#<output-port>`.
- [ ] Modules: `#<module:path subpath>`.
- [ ] Multiple-values marker `#<mvvalues>` and `#<void>` / `#<unbound>`
      sentinels.
- [ ] Boxes `#&datum`.

## Phase 5 — robustness and ergonomics
- [ ] Cycle detection in lists/vectors so circular structures print
      something instead of looping. Plan:
      - **Stopgap (cheap)**: depth/object-count cap inside `Mwrite`
        that aborts or emits `...` once exceeded. No extra data
        structures; fixes the infinite-loop symptom without proper
        labeling.
      - **Proper fix**: implement [SRFI-38](https://srfi.schemers.org/srfi-38/srfi-38.html)
        ("External Representation for Data With Shared Structure"). Two
        passes: first walks the datum into a hash map keyed by heap
        pointer, recording any object reached more than once as
        "shared"; second pass prints, emitting `#N=` on the first
        occurrence of a shared object and `#N#` on later references.
        Handles both cycles and acyclic shared sub-structure. Racket
        and Chez both use this representation. Defer until we have
        another feature that wants a heap-pointer hash map (e.g.
        `equal?`, structural equality), so the infrastructure pays for
        itself twice.
- [ ] Escape symbol names that aren't bare-readable. Today `Mwrite`
      emits the interned name verbatim, which silently breaks the
      "output is parseable s-expression" contract for perfectly valid
      symbols whose names contain delimiters or look like other
      datums. Examples that round-trip incorrectly today:
      - `"a b"` — prints `a b`, parses as two tokens.
      - `"("` — prints `(`, parses as the start of a list.
      - `"#t"` — prints `#t`, parses as the boolean.
      - `"42"` — prints `42`, parses as a fixnum.
      Until this lands, `Mwrite` is only safe for symbols whose names
      are bare-readable (no delimiters, doesn't start with `#`, isn't
      all-digits/sign-prefixed-digits, etc.). Fix: emit `|...|`
      around the name when needed (R7RS), with internal `|` and `\`
      escaped as `\|` and `\\`. Once both `Mread` and `Mwrite` agree
      on the `|...|` form, this can be exposed as a general printer.
- [ ] Optional pretty-printer (line breaks, indentation) once the basic
      writer is settled.
- [ ] Configurable output radix for fixnums (`#x`, `#b`, `#o`).

## Out of scope for now
- `(display ...)` distinct from `(write ...)` — collapse to a single
  function until strings/chars land.
- Bytevectors `#u8(...)`.
- Source-location tracking on output.
- Custom user-defined writer hooks.
