/* The hints file parser. See hints.h for the shape it accepts and why. */

#include "hints.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char       *text;      /* the file, comments blanked, NUL-terminated */
    size_t      pos;
    int         line;
    const char *path;
    char       *err;
    size_t      errlen;
} lex;

/* Every failure goes through here, so the message always carries the file and
 * the line the reader has to look at. */
static int err_at(lex *L, int line, const char *msg) {
    if (L->err && L->errlen)
        snprintf(L->err, L->errlen, "%s:%d: %s", L->path ? L->path : "(hints)",
                 line, msg);
    return -1;
}

static int at_end(const lex *L) { return L->text[L->pos] == '\0'; }
static char cur(const lex *L)   { return L->text[L->pos]; }

static void advance(lex *L) {
    if (L->text[L->pos] == '\n') L->line++;
    if (L->text[L->pos]) L->pos++;
}

static void skip_ws(lex *L) {
    for (;;) {
        char c = cur(L);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(L); continue; }
        return;
    }
}

/* Blanks `#` comments in place. Nothing in the supported subset is a string, so
 * a `#` is always a comment here; a quoted value is not part of the grammar and
 * comes back as a parse error at the line that holds it. */
static void blank_comments(char *text) {
    int in_comment = 0;
    for (char *p = text; *p; p++) {
        if (in_comment) {
            if (*p == '\n') in_comment = 0;
            else *p = ' ';
        } else if (*p == '#') {
            in_comment = 1;
            *p = ' ';
        }
    }
}

static char *slurp(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && errlen) snprintf(err, errlen, "%s: cannot open the hints file", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (err && errlen) snprintf(err, errlen, "%s: cannot read the hints file", path); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); if (err && errlen) snprintf(err, errlen, "%s: cannot read the hints file", path); return NULL; }
    rewind(f);

    char *b = (char *)malloc((size_t)n + 4);
    if (!b) { fclose(f); if (err && errlen) snprintf(err, errlen, "%s: out of memory", path); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';

    /* An editor on Windows may leave a byte-order mark; TOML parsers skip it and
     * so does this one, rather than reporting an unknown character on line 1. */
    if (got >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
        (unsigned char)b[2] == 0xBF) {
        memmove(b, b + 3, got - 3 + 1);
    }
    return b;
}

/* A bare name: letters, digits, `_`, `-`. Covers the table and key names the
 * roadmap lists, and stops readers from writing something the next kind cannot
 * share. */
static int parse_name(lex *L, char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c = cur(L);
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) break;
        if (n + 1 >= cap) return err_at(L, L->line, "name is too long");
        buf[n++] = c;
        advance(L);
    }
    buf[n] = '\0';
    if (!n) return err_at(L, L->line, "expected a name");
    return 0;
}

static int parse_address(lex *L, uint32_t *out) {
    char digits[32];
    size_t n = 0;
    int base = 10;

    if (cur(L) == '0' && (L->text[L->pos + 1] == 'x' || L->text[L->pos + 1] == 'X')) {
        base = 16;
        advance(L);
        advance(L);
    }

    for (;;) {
        char c = cur(L);
        if (c == '_') { advance(L); continue; }     /* TOML digit separator */
        int hex = (c >= '0' && c <= '9');
        if (base == 16 && !hex) {
            char l = (char)(c | 0x20);
            hex = (l >= 'a' && l <= 'f');
        }
        if (!hex) break;
        if (n + 1 >= sizeof digits) return err_at(L, L->line, "address is too long");
        digits[n++] = c;
        advance(L);
    }
    if (!n) return err_at(L, L->line, "expected an address, e.g. 0x0031D93C");
    digits[n] = '\0';

    char *end = NULL;
    unsigned long v = strtoul(digits, &end, base);
    if (!end || *end) return err_at(L, L->line, "that is not a number");
    if (v > 0xFFFFFFFFul)
        return err_at(L, L->line, "address does not fit in 32 bits");
    if (v & 3u)
        return err_at(L, L->line, "address is not 4-byte aligned, so no instruction starts there");

    *out = (uint32_t)v;
    return 0;
}

static int push_label(lex *L, a_hints *h, uint32_t addr) {
    for (int i = 0; i < h->nlabels; i++)
        if (h->labels[i] == addr) return 0;         /* listing it twice is not an error */

    /* realloc-per-entry keeps this simple; a hints file has tens of addresses,
     * not thousands, and the read happens once per emit. */
    uint32_t *p = (uint32_t *)realloc(h->labels, (size_t)(h->nlabels + 1) * sizeof *p);
    if (!p) return err_at(L, L->line, "out of memory");
    h->labels = p;
    h->labels[h->nlabels++] = addr;
    return 0;
}

/* `at = [ ... ]`, with the `[` already consumed. */
static int parse_at_array(lex *L, a_hints *h) {
    /* Reported against the line the array was opened on: by the time the file
     * runs out, the reader has been moved past it, and "line 41: not closed"
     * for an array that starts on line 3 sends them to the wrong place. */
    const int opened_at = L->line;

    for (;;) {
        skip_ws(L);
        if (at_end(L)) return err_at(L, opened_at, "the `at` array is not closed with `]`");
        if (cur(L) == ']') { advance(L); return 0; }

        uint32_t addr;
        if (parse_address(L, &addr) != 0) return -1;
        if (push_label(L, h, addr) != 0) return -1;

        skip_ws(L);
        if (cur(L) == ',') { advance(L); continue; }
        if (cur(L) == ']') { advance(L); return 0; }
        if (at_end(L)) return err_at(L, opened_at, "the `at` array is not closed with `]`");
        return err_at(L, L->line, "expected `,` or `]` after an address");
    }
}

int a_hints_load(const char *path, a_hints *out, char *err, size_t errlen) {
    if (out) { out->labels = NULL; out->nlabels = 0; }
    if (err && errlen) err[0] = '\0';
    if (!path) return 0;                 /* no hints file is not a failure */

    char *text = slurp(path, err, errlen);
    if (!text) return -1;
    blank_comments(text);

    lex L = { text, 0, 1, path, err, errlen };
    int in_label = 0;
    int rc = 0;

    for (;;) {
        skip_ws(&L);
        if (at_end(&L)) break;

        if (cur(&L) == '[' && L.text[L.pos + 1] == '[') {
            advance(&L);
            advance(&L);
            skip_ws(&L);

            char name[64];
            if (parse_name(&L, name, sizeof name) != 0) { rc = -1; break; }
            skip_ws(&L);
            if (cur(&L) != ']' || L.text[L.pos + 1] != ']') {
                rc = err_at(&L, L.line, "expected `]]` to close the table name");
                break;
            }
            advance(&L);
            advance(&L);

            if (strcmp(name, "label") != 0) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "unknown hints table `%s`; this build understands `label`",
                         name);
                rc = err_at(&L, L.line, msg);
                break;
            }
            in_label = 1;
            continue;
        }

        if (!in_label) {
            rc = err_at(&L, L.line,
                        "a key outside a table; every hint sits in one, e.g. [[label]]");
            break;
        }

        char key[64];
        int keyline = L.line;
        if (parse_name(&L, key, sizeof key) != 0) { rc = -1; break; }
        skip_ws(&L);
        if (cur(&L) != '=') {
            char msg[160];
            snprintf(msg, sizeof msg, "expected `=` after `%s`", key);
            rc = err_at(&L, L.line, msg);
            break;
        }
        advance(&L);
        skip_ws(&L);

        if (strcmp(key, "at") != 0) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "unknown key `%s` in [[label]]; this build understands `at`", key);
            rc = err_at(&L, keyline, msg);
            break;
        }

        if (cur(&L) != '[') {
            rc = err_at(&L, L.line, "`at` takes an array of addresses, e.g. at = [0x0031D93C]");
            break;
        }
        advance(&L);
        if (parse_at_array(&L, out) != 0) { rc = -1; break; }
    }

    free(text);
    if (rc != 0) a_hints_free(out);
    return rc;
}

void a_hints_free(a_hints *h) {
    if (!h) return;
    free(h->labels);
    h->labels = NULL;
    h->nlabels = 0;
}
