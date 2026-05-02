#include "minim.h"
#include "gc.h"
#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_BUF_MAX 256

/* Cached interned symbol for `quote`. Registered as a global root on
 * first use; cleared by parser_shutdown so a Mshutdown/Minit cycle
 * does not leave a dangling pointer. */
static mobj quote_sym = 0;

void parser_shutdown(void) {
    quote_sym = 0;
}

static mobj get_quote_sym(void) {
    if (quote_sym == 0) {
        quote_sym = Mintern("quote");
        minim_protect(&quote_sym);
    }
    return quote_sym;
}

/* ----------------------------------------------------------------------
 * mreader: peek / read
 * -------------------------------------------------------------------- */

void mreader_init_string(mreader *r, const char *s) {
    r->kind = MREADER_STRING;
    r->u.s.buf = s;
    r->u.s.pos = 0;
    r->u.s.len = strlen(s);
    r->peeked = -1;
}

void mreader_init_file(mreader *r, FILE *fp) {
    r->kind = MREADER_FILE;
    r->u.fp = fp;
    r->peeked = -1;
}

static int reader_get(mreader *r) {
    if (r->peeked != -1) {
        int c = r->peeked;
        r->peeked = -1;
        return c;
    }
    switch (r->kind) {
    case MREADER_STRING:
        if (r->u.s.pos >= r->u.s.len) return EOF;
        return (unsigned char)r->u.s.buf[r->u.s.pos++];
    case MREADER_FILE:
        return fgetc(r->u.fp);
    }
    return EOF;
}

static int reader_peek(mreader *r) {
    if (r->peeked == -1) r->peeked = reader_get(r);
    return r->peeked;
}

/* ----------------------------------------------------------------------
 * Lexical predicates
 * -------------------------------------------------------------------- */

static int is_delimiter(int c) {
    return c == EOF || isspace(c) ||
           c == '(' || c == ')' ||
           c == '[' || c == ']' ||
           c == '{' || c == '}' ||
           c == '"' || c == ';';
}

static int is_symbol_char(int c) {
    return c != EOF && !is_delimiter(c);
}

static int is_close_paren(int c) {
    return c == ')' || c == ']' || c == '}';
}

static int matching_paren(int open, int close) {
    return (open == '(' && close == ')') ||
           (open == '[' && close == ']') ||
           (open == '{' && close == '}');
}

/* ----------------------------------------------------------------------
 * Errors — abort for now; Phase 5 of the TODO swaps to conditions.
 * -------------------------------------------------------------------- */

static void parse_error(const char *msg) {
    fprintf(stderr, "minim: parse error: %s\n", msg);
    abort();
}

static void parse_error_c(const char *msg, int c) {
    if (c == EOF)
        fprintf(stderr, "minim: parse error: %s (got EOF)\n", msg);
    else
        fprintf(stderr, "minim: parse error: %s (got %c)\n", msg, c);
    abort();
}

/* ----------------------------------------------------------------------
 * Whitespace / comment skipping
 * -------------------------------------------------------------------- */

static mobj read_datum(mreader *r);

static void skip_block_comment(mreader *r) {
    /* The leading `#|` is already consumed. */
    int depth = 1;
    while (depth > 0) {
        int c = reader_get(r);
        if (c == EOF) parse_error("unterminated block comment");
        if (c == '#') {
            int n = reader_peek(r);
            if (n == '|') { reader_get(r); depth++; }
        } else if (c == '|') {
            int n = reader_peek(r);
            if (n == '#') { reader_get(r); depth--; }
        }
    }
}

static void skip_whitespace(mreader *r) {
    for (;;) {
        int c = reader_peek(r);
        if (c == EOF) return;
        if (isspace(c)) { reader_get(r); continue; }
        if (c == ';') {
            reader_get(r);
            for (;;) {
                int n = reader_get(r);
                if (n == EOF || n == '\n') break;
            }
            continue;
        }
        return;
    }
}

/* ----------------------------------------------------------------------
 * Numbers and symbols
 * -------------------------------------------------------------------- */

static int hex_digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int all_decimal_digits(const char *s) {
    if (*s == '+' || *s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* Read the body of a token (symbol or number) into buf. The first char
 * is already in `first`. Returns the length written. */
static size_t read_token_body(mreader *r, int first, char *buf, size_t cap) {
    size_t i = 0;
    if (i + 1 >= cap) parse_error("token too long");
    buf[i++] = (char)first;
    while (is_symbol_char(reader_peek(r))) {
        if (i + 1 >= cap) parse_error("token too long");
        buf[i++] = (char)reader_get(r);
    }
    buf[i] = '\0';
    return i;
}

/* `first` is already the first character of the token. */
static mobj read_atom(mreader *r, int first) {
    char buf[READ_BUF_MAX];
    read_token_body(r, first, buf, sizeof(buf));

    /* Decimal fixnum? Accept optional leading +/-. */
    if (all_decimal_digits(buf)) {
        intptr_t sign = 1;
        const char *p = buf;
        if (*p == '+') p++;
        else if (*p == '-') { sign = -1; p++; }
        intptr_t n = 0;
        for (; *p; p++) n = n * 10 + (*p - '0');
        return Mfixnum(sign * n);
    }

    /* Otherwise it's a symbol. */
    return Mintern(buf);
}

/* `#x` already consumed; first hex digit (or sign) is the next read. */
static mobj read_hex_fixnum(mreader *r) {
    int c = reader_get(r);
    intptr_t sign = 1;
    if (c == '+') { c = reader_get(r); }
    else if (c == '-') { sign = -1; c = reader_get(r); }

    if (hex_digit_val(c) < 0)
        parse_error_c("expected hex digit after #x", c);

    intptr_t n = 0;
    for (;;) {
        int v = hex_digit_val(c);
        if (v < 0) break;
        n = n * 16 + v;
        c = reader_peek(r);
        if (hex_digit_val(c) < 0) break;
        reader_get(r);
    }
    if (!is_delimiter(reader_peek(r)))
        parse_error_c("expected delimiter after hex fixnum", reader_peek(r));
    return Mfixnum(sign * n);
}

/* ----------------------------------------------------------------------
 * Lists and vectors
 * -------------------------------------------------------------------- */

static mobj read_list(mreader *r, int open_paren) {
    MINIM_GC_FRAME_BEGIN;
    mobj head = Mnull, tail = Mnull, x = Mnull, cell = Mnull;
    MINIM_GC_PROTECT(head);
    MINIM_GC_PROTECT(tail);
    MINIM_GC_PROTECT(x);
    MINIM_GC_PROTECT(cell);

    for (;;) {
        skip_whitespace(r);
        int c = reader_peek(r);

        if (c == EOF) parse_error("unterminated list");

        if (is_close_paren(c)) {
            reader_get(r);
            if (!matching_paren(open_paren, c))
                parse_error_c("paren mismatch", c);
            MINIM_GC_RETURN(head);
        }

        /* Dotted-pair separator: `.` flanked by delimiters. */
        if (c == '.') {
            reader_get(r);
            int n = reader_peek(r);
            if (is_delimiter(n)) {
                if (Mnullp(head)) parse_error("dotted pair with no car");
                skip_whitespace(r);
                x = read_datum(r);
                Mset_cdr(tail, x);
                skip_whitespace(r);
                int e = reader_get(r);
                if (!is_close_paren(e))
                    parse_error_c("expected close paren after dotted cdr", e);
                if (!matching_paren(open_paren, e))
                    parse_error_c("paren mismatch", e);
                MINIM_GC_RETURN(head);
            } else {
                /* `.` was the first char of a token (symbol). */
                x = read_atom(r, '.');
            }
        } else {
            x = read_datum(r);
        }

        cell = Mcons(x, Mnull);
        if (Mnullp(head)) {
            head = cell;
            tail = cell;
        } else {
            Mset_cdr(tail, cell);
            tail = cell;
        }
    }
    /* unreachable */
    MINIM_GC_RETURN(head);
}

static mobj read_vector(mreader *r) {
    /* Strategy: build a list, count it, then allocate the vector and
     * copy slots. We don't have list-to-vector in the runtime yet. */
    MINIM_GC_FRAME_BEGIN;
    mobj head = Mnull, tail = Mnull, x = Mnull, cell = Mnull, v = Mnull;
    MINIM_GC_PROTECT(head);
    MINIM_GC_PROTECT(tail);
    MINIM_GC_PROTECT(x);
    MINIM_GC_PROTECT(cell);
    MINIM_GC_PROTECT(v);

    size_t length = 0;
    for (;;) {
        skip_whitespace(r);
        int c = reader_peek(r);
        if (c == EOF) parse_error("unterminated vector");
        if (c == ')') { reader_get(r); break; }

        x = read_datum(r);
        cell = Mcons(x, Mnull);
        if (Mnullp(head)) {
            head = cell;
            tail = cell;
        } else {
            Mset_cdr(tail, cell);
            tail = cell;
        }
        length++;
    }

    v = Mvector(length, Mfixnum(0));
    mobj cur = head;
    for (size_t i = 0; i < length; i++) {
        Mvector_set(v, i, Mcar(cur));
        cur = Mcdr(cur);
    }
    MINIM_GC_RETURN(v);
}

/* ----------------------------------------------------------------------
 * #-prefixed syntax
 * -------------------------------------------------------------------- */

static mobj read_hash(mreader *r) {
    int c = reader_get(r);
    switch (c) {
    case 't':
        if (!is_delimiter(reader_peek(r)))
            parse_error_c("expected delimiter after #t", reader_peek(r));
        return Mtrue;
    case 'f':
        if (!is_delimiter(reader_peek(r)))
            parse_error_c("expected delimiter after #f", reader_peek(r));
        return Mfalse;
    case '(':
        return read_vector(r);
    case 'x':
        return read_hex_fixnum(r);
    case ';': {
        /* Datum comment — read and discard one datum, then continue. */
        skip_whitespace(r);
        if (reader_peek(r) == EOF)
            parse_error("datum comment with nothing to discard");
        (void)read_datum(r);
        return read_datum(r);
    }
    case '|':
        skip_block_comment(r);
        return read_datum(r);
    case '%': {
        /* `#%name` — system symbol. The `#%` is part of the name. */
        char buf[READ_BUF_MAX];
        size_t i = 0;
        buf[i++] = '#';
        buf[i++] = '%';
        while (is_symbol_char(reader_peek(r))) {
            if (i + 1 >= sizeof(buf)) parse_error("symbol too long");
            buf[i++] = (char)reader_get(r);
        }
        buf[i] = '\0';
        return Mintern(buf);
    }
    case EOF:
        parse_error("unexpected EOF after #");
        return Meof;
    default:
        parse_error_c("unknown # syntax", c);
        return Meof;
    }
}

/* ----------------------------------------------------------------------
 * Top-level dispatch
 * -------------------------------------------------------------------- */

static mobj read_datum(mreader *r) {
    skip_whitespace(r);
    int c = reader_get(r);

    if (c == EOF) return Meof;
    if (c == '(' || c == '[' || c == '{') return read_list(r, c);
    if (c == '#') return read_hash(r);

    if (c == '\'') {
        MINIM_GC_FRAME_BEGIN;
        mobj inner = Mnull, tail = Mnull, lst = Mnull, q = Mnull;
        MINIM_GC_PROTECT(inner);
        MINIM_GC_PROTECT(tail);
        MINIM_GC_PROTECT(lst);
        MINIM_GC_PROTECT(q);
        inner = read_datum(r);
        tail = Mcons(inner, Mnull);
        q = get_quote_sym();
        lst = Mcons(q, tail);
        MINIM_GC_RETURN(lst);
    }

    if (is_close_paren(c))
        parse_error_c("unexpected close paren", c);

    /* Sign prefix: `+` or `-` followed by a digit ⇒ signed number;
     * otherwise it's a symbol like `+` or `-`. */
    if ((c == '+' || c == '-') && isdigit(reader_peek(r))) {
        return read_atom(r, c);
    }

    if (is_symbol_char(c)) return read_atom(r, c);

    parse_error_c("unexpected character", c);
    return Meof; /* unreachable */
}

mobj Mread(mreader *r) {
    return read_datum(r);
}
