/* Public domain. */

/*
 * Syntax highlighting engine for mg.
 *
 * Data-driven: language rules are loaded from .syntax files.
 * The tokenizer scans one line at a time, carrying block-comment
 * state across lines via l_synstate.
 */

#include <sys/queue.h>
#include <sys/types.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "kbd.h"
#include "syntax.h"

/* ---- globals ---- */

static struct syntax_table *g_tables[16];  /* loaded tables */
static int                  g_ntables = 0;

/* ---- keyword hash ---- */

static unsigned int
kw_hash(const char *s, int len)
{
    unsigned int h = 5381;
    int i;
    for (i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)s[i];
    return h % SYN_HASHSIZE;
}

static void
kw_add(struct syntax_table *st, const char *word, int color)
{
    unsigned int h = kw_hash(word, strlen(word));
    struct syn_kw *e = malloc(sizeof(*e));
    if (e == NULL)
        return;
    e->kw_word = strdup(word);
    e->kw_color = color;
    e->kw_next = st->st_kw[h];
    st->st_kw[h] = e;
}

static int
kw_lookup(struct syntax_table *st, const char *word, int len)
{
    unsigned int h = kw_hash(word, len);
    struct syn_kw *e;
    for (e = st->st_kw[h]; e != NULL; e = e->kw_next) {
        if (strlen(e->kw_word) == (size_t)len &&
            memcmp(e->kw_word, word, len) == 0)
            return e->kw_color;
    }
    return SCOLOR_DEFAULT;
}

/* ---- syntax file parser ---- */

static char *
skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static char *
trim_eol(char *s)
{
    int len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
           s[len-1] == ' ' || s[len-1] == '\t'))
        s[--len] = '\0';
    return s;
}

/*
 * Parse a delimiter spec like:  " "  or  ' '  or  block-comment delimiters.
 * Returns the start and end strings via out_start/out_end.
 * For single-char delimiters (e.g. string quotes), start==end.
 * For two-token delimiters (e.g. block comments), start and end differ.
 */
static void
parse_delim(const char *spec, char **out_start, char **out_end)
{
    const char *p = spec;
    char buf[64];
    int len;

    *out_start = NULL;
    *out_end = NULL;

    /* skip leading whitespace */
    while (*p == ' ' || *p == '\t')
        p++;

    /* read first token */
    len = 0;
    while (*p && *p != ' ' && *p != '\t' && len < 63)
        buf[len++] = *p++;
    buf[len] = '\0';
    if (len > 0)
        *out_start = strdup(buf);

    /* skip whitespace */
    while (*p == ' ' || *p == '\t')
        p++;

    /* read second token (optional) */
    len = 0;
    while (*p && *p != ' ' && *p != '\t' && len < 63)
        buf[len++] = *p++;
    buf[len] = '\0';
    if (len > 0)
        *out_end = strdup(buf);
}

struct syntax_table *
syntax_load(const char *path)
{
    FILE *fp;
    char line[256];
    struct syntax_table *st;

    if ((fp = fopen(path, "r")) == NULL)
        return NULL;

    st = calloc(1, sizeof(*st));
    if (st == NULL) {
        fclose(fp);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = skip_ws(line);
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        p = trim_eol(p);

        if (strncmp(p, "keyword:", 8) == 0) {
            kw_add(st, skip_ws(p + 8), SCOLOR_KEYWORD);
        } else if (strncmp(p, "type:", 5) == 0) {
            kw_add(st, skip_ws(p + 5), SCOLOR_TYPE);
        } else if (strncmp(p, "control:", 8) == 0) {
            kw_add(st, skip_ws(p + 8), SCOLOR_CONTROL);
        } else if (strncmp(p, "string:", 7) == 0) {
            char *s, *e;
            parse_delim(p + 7, &s, &e);
            if (s) {
                struct syn_rule *r = calloc(1, sizeof(*r));
                r->sr_type = SRULE_STRING;
                r->sr_start = s;
                r->sr_end = e ? e : strdup(s);
                r->sr_next = st->st_rules;
                st->st_rules = r;
            }
        } else if (strncmp(p, "line_comment:", 13) == 0) {
            char *s, *e;
            parse_delim(p + 13, &s, &e);
            if (s) {
                struct syn_rule *r = calloc(1, sizeof(*r));
                r->sr_type = SRULE_LINE_COMMENT;
                r->sr_start = s;
                r->sr_next = st->st_rules;
                st->st_rules = r;
            }
            free(e);
        } else if (strncmp(p, "block_comment:", 14) == 0) {
            char *s, *e;
            parse_delim(p + 14, &s, &e);
            if (s && e) {
                /* store as the table-level block comment */
                st->st_block_start = s;
                st->st_block_end = e;
            } else {
                free(s);
                free(e);
            }
        } else if (strncmp(p, "number:", 7) == 0) {
            /* numbers are auto-detected (0-9), this is a no-op for now */
        } else if (strncmp(p, "flags:", 6) == 0) {
            char *spec = skip_ws(p + 6);
            char *tok = strtok(spec, " \t");
            while (tok) {
                if (strcmp(tok, "tags") == 0)
                    st->st_flags |= SYNFLAG_TAGS;
                else if (strcmp(tok, "annotations") == 0)
                    st->st_flags |= SYNFLAG_ANNOTATIONS;
                else if (strcmp(tok, "css_props") == 0)
                    st->st_flags |= SYNFLAG_CSS_PROPS;
                else if (strcmp(tok, "hyphen_words") == 0)
                    st->st_flags |= SYNFLAG_HYPHEN_WORDS;
                else if (strcmp(tok, "dollar_vars") == 0)
                    st->st_flags |= SYNFLAG_DOLLAR_VARS;
                else if (strcmp(tok, "char_lit") == 0)
                    st->st_flags |= SYNFLAG_CHAR_LIT;
                tok = strtok(NULL, " \t");
            }
        }
    }
    fclose(fp);

    /* register in global table */
    if (g_ntables < 15)
        g_tables[g_ntables++] = st;

    return st;
}

void
syntax_free(struct syntax_table *st)
{
    int i;
    if (st == NULL)
        return;
    for (i = 0; i < SYN_HASHSIZE; i++) {
        struct syn_kw *e = st->st_kw[i];
        while (e) {
            struct syn_kw *next = e->kw_next;
            free(e->kw_word);
            free(e);
            e = next;
        }
    }
    /* free rules */
    struct syn_rule *r = st->st_rules;
    while (r) {
        struct syn_rule *next = r->sr_next;
        free(r->sr_start);
        free(r->sr_end);
        free(r);
        r = next;
    }
    free(st->st_block_start);
    free(st->st_block_end);
    free(st);
}

struct syntax_table *
syntax_find(const char *lang)
{
    int i;
    for (i = 0; i < g_ntables; i++) {
        if (g_tables[i]->st_lang && strcmp(g_tables[i]->st_lang, lang) == 0)
            return g_tables[i];
    }
    return NULL;
}

/* ---- tokenizer helpers ---- */

static int
is_word_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static int
is_word_char(unsigned char c)
{
    return is_word_start(c) || (c >= '0' && c <= '9');
}

/* word char with optional hyphen/dollar support */
static int
is_word_char_ext(unsigned char c, int flags)
{
    return is_word_char(c) ||
           ((flags & SYNFLAG_HYPHEN_WORDS) && c == '-') ||
           ((flags & SYNFLAG_DOLLAR_VARS) && c == '$');
}

/* word start with optional $ support (for Shell/PHP) */
static int
is_word_start_ext(unsigned char c, int flags)
{
    return is_word_start(c) ||
           ((flags & SYNFLAG_DOLLAR_VARS) && c == '$');
}

static int
match_at(const char *text, int pos, int textlen, const char *pat)
{
    int plen;
    if (pat == NULL)
        return 0;
    plen = strlen(pat);
    if (pos + plen > textlen)
        return 0;
    return memcmp(text + pos, pat, plen) == 0;
}

static int
find_delim(const char *text, int pos, int textlen, const char *delim)
{
    int dlen;
    if (delim == NULL)
        return -1;
    dlen = strlen(delim);
    while (pos + dlen <= textlen) {
        if (memcmp(text + pos, delim, dlen) == 0)
            return pos + dlen;
        pos++;
    }
    return -1;
}

static int
find_quote_end(const char *text, int pos, int textlen, char quote)
{
    pos++;  /* skip opening quote */
    while (pos < textlen) {
        if (text[pos] == '\\' && pos + 1 < textlen) {
            pos += 2;
            continue;
        }
        if (text[pos] == quote)
            return pos + 1;
        pos++;
    }
    return textlen;  /* unclosed — color to EOL */
}

/* ---- main tokenizer ---- */

void
syntax_match_line(struct line *prev, struct line *lp,
                  char *hue, int ncol)
{
    struct syntax_table *st;
    const char *text;
    int textlen, i, state;

    text = lp->l_text;
    textlen = llength(lp);

    /* default color for all bytes */
    memset(hue, SCOLOR_DEFAULT, ncol > textlen ? ncol : textlen);

    /* ---- dired mode: highlight ls -al output ---- */
    if (curbp && curbp->b_nmodes >= 1 && curbp->b_modes[1] != NULL &&
        !(curbp->b_flag & BFSYNOFF)) {
        const char *mn = curbp->b_modes[1]->p_name;
        if (mn && strcmp(mn, "dired") == 0) {
            /* Skip leading spaces to find permission chars */
            int pstart = 0;
            while (pstart < textlen && text[pstart] == ' ')
                pstart++;
            /* Line starting with permission chars: -dlcrwxst... */
            if (pstart + 10 <= textlen &&
                (text[pstart] == '-' || text[pstart] == 'd' ||
                text[pstart] == 'l' || text[pstart] == 'c' ||
                text[pstart] == 'b' || text[pstart] == 'p' ||
                text[pstart] == 's')) {
                int is_dir = (text[pstart] == 'd');
                int is_link = (text[pstart] == 'l');
                /* permission field: 10 chars from pstart */
                memset(hue + pstart, SCOLOR_COMMENT, 10);
                /* find filename: 9th space-delimited field */
                {
                    int off = 0, field = 0, name_start = -1;
                    while (off < textlen) {
                        if (text[off] == ' ') {
                            if (++field == 9) {
                                off++;
                                while (off < textlen && text[off] == ' ')
                                    off++;
                                name_start = off;
                                break;
                            }
                            while (off < textlen && text[off] == ' ')
                                off++;
                        } else {
                            off++;
                        }
                    }
                    if (name_start >= 0 && name_start < textlen) {
                        if (is_dir)
                            memset(hue + name_start, SCOLOR_CONTROL,
                                   textlen - name_start);
                        else if (is_link)
                            memset(hue + name_start, SCOLOR_STRING,
                                   textlen - name_start);
                        else
                            memset(hue + name_start, SCOLOR_DEFAULT,
                                   textlen - name_start);
                    }
                }
            }
            return;
        }
    }

    st = curbp ? curbp->b_syntax : NULL;
    if (st == NULL || (curbp->b_flag & BFSYNOFF))
        return;

    /* recover state from previous line */
    state = prev ? prev->l_synstate : SYNSTATE_DEFAULT;

    for (i = 0; i < textlen; ) {
        unsigned char c = (unsigned char)text[i];

        /* ---- inside block comment ---- */
        if (state == SYNSTATE_IN_BLOCK_COMMENT) {
            int end = find_delim(text, i, textlen, st->st_block_end);
            if (end < 0) {
                memset(hue + i, SCOLOR_COMMENT, textlen - i);
                state = SYNSTATE_IN_BLOCK_COMMENT;
                break;
            }
            memset(hue + i, SCOLOR_COMMENT, end - i);
            i = end;
            state = SYNSTATE_DEFAULT;
            continue;
        }

        /* ---- inside triple-quoted string ---- */
        if (state == SYNSTATE_IN_TRIPLE_DQUOTE ||
            state == SYNSTATE_IN_TRIPLE_SQUOTE) {
            const char *delim = (state == SYNSTATE_IN_TRIPLE_DQUOTE)
                ? "\"\"\"" : "'''";
            int end = find_delim(text, i, textlen, delim);
            if (end < 0) {
                memset(hue + i, SCOLOR_STRING, textlen - i);
                break;  /* stays in triple-quote state */
            }
            memset(hue + i, SCOLOR_STRING, end - i);
            i = end;
            state = SYNSTATE_DEFAULT;
            continue;
        }

        /* ---- inside backtick string ---- */
        if (state == SYNSTATE_IN_BACKTICK) {
            int end = find_quote_end(text, i, textlen, '`');
            memset(hue + i, SCOLOR_STRING, end - i);
            i = end;
            /* check if actually closed (not just ran off end) */
            if (end > 0 && end <= textlen && text[end - 1] == '`')
                state = SYNSTATE_DEFAULT;
            /* else: unclosed, stays in backtick state */
            continue;
        }

        /* ---- whitespace ---- */
        if (c == ' ' || c == '\t') {
            hue[i] = SCOLOR_DEFAULT;
            i++;
            continue;
        }

        /* ---- block comment start ---- */
        if (st->st_block_start && match_at(text, i, textlen,
                                           st->st_block_start)) {
            int start = i;
            int slen = strlen(st->st_block_start);
            int end = find_delim(text, i + slen, textlen,
                                 st->st_block_end);
            if (end < 0) {
                memset(hue + start, SCOLOR_COMMENT, textlen - start);
                state = SYNSTATE_IN_BLOCK_COMMENT;
                break;
            }
            memset(hue + start, SCOLOR_COMMENT, end - start);
            i = end;
            continue;
        }

        /* ---- line comment ---- */
        {
            struct syn_rule *r;
            int matched = 0;
            for (r = st->st_rules; r != NULL; r = r->sr_next) {
                if (r->sr_type == SRULE_LINE_COMMENT &&
                    match_at(text, i, textlen, r->sr_start)) {
                    memset(hue + i, SCOLOR_COMMENT, textlen - i);
                    i = textlen;
                    matched = 1;
                    break;
                }
            }
            if (matched)
                continue;
        }

        /* ---- annotation: @Word (only when SYNFLAG_ANNOTATIONS) ---- */
        if ((st->st_flags & SYNFLAG_ANNOTATIONS) &&
            c == '@' && i + 1 < textlen &&
            is_word_start((unsigned char)text[i + 1])) {
            int end = i + 1;
            while (end < textlen && is_word_char((unsigned char)text[end]))
                end++;
            memset(hue + i, SCOLOR_CONTROL, end - i);
            i = end;
            continue;
        }

        /* ---- HTML/XML tag names: <tag  </tag (only SYNFLAG_TAGS) ----
         * Like Vim: only highlight '<' + optional '/' + tag name.
         * '>', '/>', attributes, and '=' keep default color. */
        if ((st->st_flags & SYNFLAG_TAGS) &&
            c == '<' && i + 1 < textlen &&
            (is_word_start((unsigned char)text[i + 1]) || text[i + 1] == '/')) {
            int start = i;
            int end = i + 1;
            /* skip optional '/' for closing tags */
            if (text[end] == '/')
                end++;
            if (end < textlen && is_word_start((unsigned char)text[end])) {
                while (end < textlen && is_word_char((unsigned char)text[end]))
                    end++;
                /* also handle hyphenated tags like <t-config-provider> */
                while (end < textlen && text[end] == '-' &&
                       end + 1 < textlen &&
                       is_word_start((unsigned char)text[end + 1])) {
                    end++;
                    while (end < textlen && is_word_char((unsigned char)text[end]))
                        end++;
                }
                /* highlight '<' + optional '/' + tag name */
                memset(hue + start, SCOLOR_CONTROL, end - start);
                i = end;
                continue;
            }
            /* not a valid tag, fall through */
        }

        /* ---- Elisp character literal: ?a, ?\n, ?(  ---- */
        if ((st->st_flags & SYNFLAG_CHAR_LIT) && c == '?' &&
            i + 1 < textlen) {
            unsigned char nc = (unsigned char)text[i + 1];
            /* ? followed by whitespace or ; is not a char literal */
            if (nc != ' ' && nc != '\t' && nc != ';') {
                int end;
                if (nc == '\\') {
                    /* ?\X — include escape sequence */
                    end = i + 2;
                    /* consume one more char for \n, \t, \\, etc. */
                    if (end < textlen)
                        end++;
                } else {
                    /* ?X — single character */
                    end = i + 2;
                }
                memset(hue + i, SCOLOR_STRING, end - i);
                i = end;
                continue;
            }
        }

        /* ---- Rust lifetime / label: 'a, 'static (not a char literal) ---- */
        if (c == '\'' && i + 1 < textlen &&
            is_word_start((unsigned char)text[i + 1])) {
            /* check if it's a char literal: 'X' or '\X' */
            if (text[i + 1] == '\\') {
                /* escape: find closing ' */
                int j = i + 2;
                while (j < textlen && text[j] != '\'')
                    j++;
                if (j < textlen && text[j] == '\'') {
                    /* it's a char literal — let string matching handle it */
                } else {
                    /* no closing quote — treat as lifetime */
                    int end = i + 1;
                    while (end < textlen && is_word_char((unsigned char)text[end]))
                        end++;
                    memset(hue + i, SCOLOR_KEYWORD, end - i);
                    i = end;
                    continue;
                }
            } else if (i + 2 < textlen && text[i + 2] == '\'') {
                /* 'X' — char literal, let string matching handle it */
            } else {
                /* 'a or 'static — lifetime */
                int end = i + 1;
                while (end < textlen && is_word_char((unsigned char)text[end]))
                    end++;
                memset(hue + i, SCOLOR_KEYWORD, end - i);
                i = end;
                continue;
            }
        }

        /* ---- string (from syntax rules: ", ', `, """, ''') ---- */
        {
            struct syn_rule *r;
            int matched = 0;
            for (r = st->st_rules; r != NULL; r = r->sr_next) {
                if (r->sr_type != SRULE_STRING)
                    continue;
                if (match_at(text, i, textlen, r->sr_start)) {
                    int slen = strlen(r->sr_start);
                    if (slen == 1) {
                        /* single-char: escape-aware search */
                        int end = find_quote_end(text, i, textlen,
                                                  r->sr_start[0]);
                        memset(hue + i, SCOLOR_STRING, end - i);
                        i = end;
                        /* unclosed backtick → multi-line */
                        if (r->sr_start[0] == '`' && i >= textlen &&
                            !(i > 0 && text[i - 1] == '`'))
                            state = SYNSTATE_IN_BACKTICK;
                    } else {
                        /* multi-char (triple-quote): search for end delim */
                        int end = find_delim(text, i + slen, textlen,
                                             r->sr_end);
                        if (end < 0) {
                            /* unclosed — spans to next line(s) */
                            memset(hue + i, SCOLOR_STRING, textlen - i);
                            if (strcmp(r->sr_start, "\"\"\"") == 0)
                                state = SYNSTATE_IN_TRIPLE_DQUOTE;
                            else
                                state = SYNSTATE_IN_TRIPLE_SQUOTE;
                            i = textlen;
                            break;
                        }
                        memset(hue + i, SCOLOR_STRING, end - i);
                        i = end;
                    }
                    matched = 1;
                    break;
                }
            }
            if (matched)
                continue;
        }

        /* ---- number ---- */
        if (c >= '0' && c <= '9') {
            int end = i;
            while (end < textlen && (is_word_char((unsigned char)text[end]) ||
                   text[end] == '.'))
                end++;
            memset(hue + i, SCOLOR_NUMBER, end - i);
            i = end;
            continue;
        }

        /* ---- #/@ prefixed keywords (C preprocessor, CSS at-rules, Jai #directives) ---- */
        if ((c == '#' || c == '@') && i + 1 < textlen &&
            is_word_start((unsigned char)text[i + 1])) {
            int end = i + 1;
            while (end < textlen && is_word_char_ext((unsigned char)text[end], st->st_flags))
                end++;
            int wlen = end - i;
            int color = kw_lookup(st, text + i, wlen);
            if (color != SCOLOR_DEFAULT) {
                memset(hue + i, color, wlen);
                i = end;
                continue;
            }
            /* not a known keyword — leave prefix as default, scan word normally */
            hue[i] = SCOLOR_DEFAULT;
            i++;
            /* fall through to let the word be scanned normally */
        }

        /* ---- keyword / type ---- */
        if (is_word_start_ext(c, st->st_flags)) {
            int end = i;
            while (end < textlen && is_word_char_ext((unsigned char)text[end], st->st_flags))
                end++;
            int wlen = end - i;
            /* boolean/null constants → same color as numbers/strings */
            if ((wlen == 4 && strncmp(text + i, "true", 4) == 0) ||
                (wlen == 4 && strncmp(text + i, "True", 4) == 0) ||
                (wlen == 4 && strncmp(text + i, "TRUE", 4) == 0) ||
                (wlen == 5 && strncmp(text + i, "false", 5) == 0) ||
                (wlen == 5 && strncmp(text + i, "False", 5) == 0) ||
                (wlen == 5 && strncmp(text + i, "FALSE", 5) == 0) ||
                (wlen == 4 && strncmp(text + i, "null", 4) == 0) ||
                (wlen == 4 && strncmp(text + i, "Null", 4) == 0) ||
                (wlen == 4 && strncmp(text + i, "NULL", 4) == 0) ||
                (wlen == 4 && strncmp(text + i, "None", 4) == 0) ||
                (wlen == 3 && strncmp(text + i, "nil", 3) == 0) ||
                (wlen == 3 && strncmp(text + i, "Nil", 3) == 0) ||
                (wlen == 3 && strncmp(text + i, "NIL", 3) == 0))
                memset(hue + i, SCOLOR_NUMBER, wlen);
            else {
                int color = kw_lookup(st, text + i, wlen);
                if (color != SCOLOR_DEFAULT)
                    memset(hue + i, color, wlen);
                /* CSS property name: word followed by ':' (only SYNFLAG_CSS_PROPS) */
                else if ((st->st_flags & SYNFLAG_CSS_PROPS) &&
                         end < textlen && text[end] == ':')
                    memset(hue + i, SCOLOR_CONTROL, wlen);
            }
            i = end;
            continue;
        }

        /* ---- default ---- */
        hue[i] = SCOLOR_DEFAULT;
        i++;
    }

    lp->l_synstate = (short)state;
}

/* ---- commands ---- */

int
syntax_on(int f, int n)
{
    if (curbp == NULL)
        return FALSE;
    curbp->b_flag &= ~BFSYNOFF;
    curwp->w_rflag |= WFFULL;
    ewprintf("Syntax highlighting on");
    return TRUE;
}

int
syntax_off(int f, int n)
{
    if (curbp == NULL)
        return FALSE;
    curbp->b_flag |= BFSYNOFF;
    curwp->w_rflag |= WFFULL;
    ewprintf("Syntax highlighting off");
    return TRUE;
}

int
syntax_mode(int f, int n)
{
    char buf[64], *p;
    struct syntax_table *st;

    if (curbp == NULL)
        return FALSE;
    p = eread("Syntax mode: ", buf, sizeof(buf), EFNEW);
    if (p == NULL || p[0] == '\0')
        return ABORT;

    /* already loaded? reuse the cached table */
    st = syntax_find(buf);
    if (st == NULL) {
        char path[256];
        const char *home = getenv("HOME");
        if (home == NULL)
            home = ".";
        snprintf(path, sizeof(path), "%s/.mg-syntax/%s.syntax",
            home, buf);
        st = syntax_load(path);
        if (st == NULL) {
            dobeep();
            ewprintf("Cannot load syntax file: %s", path);
            return FALSE;
        }
        st->st_lang = strdup(buf);
    }
    curbp->b_syntax = st;
    curbp->b_flag &= ~BFSYNOFF;
    curwp->w_rflag |= WFFULL;
    ewprintf("Syntax mode: %s", buf);
    return TRUE;
}

int
syntax_reload(int f, int n)
{
    struct buffer *bp;
    int i;

    /* free all cached tables */
    for (i = 0; i < g_ntables; i++)
        syntax_free(g_tables[i]);
    g_ntables = 0;

    /* re-detect syntax for every buffer that has a filename */
    for (bp = bheadp; bp != NULL; bp = bp->b_bufp) {
        if (bp->b_fname[0] != '\0')
            bp->b_syntax = syntax_detect(bp->b_fname);
    }

    /* force full redisplay on all windows */
    if (curwp)
        curwp->w_rflag |= WFFULL;
    ewprintf("Syntax reloaded");
    return TRUE;
}

/* ---- auto-detect by filename extension ---- */

struct syntax_table *
syntax_detect(const char *filename)
{
    const char *ext;
    const char *lang;
    char path[256];
    const char *home;

    if (filename == NULL)
        return NULL;

    /* Check for known dotfiles (no extension) first. */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (strcmp(base, ".zshrc") == 0 || strcmp(base, ".zshenv") == 0 ||
        strcmp(base, ".zprofile") == 0 || strcmp(base, ".zlogin") == 0 ||
        strcmp(base, ".bashrc") == 0 || strcmp(base, ".bash_profile") == 0 ||
        strcmp(base, ".bash_login") == 0 || strcmp(base, ".bash_logout") == 0 ||
        strcmp(base, ".profile") == 0 || strcmp(base, ".bash_aliases") == 0 ||
        strcmp(base, ".kshrc") == 0 || strcmp(base, ".shrc") == 0 ||
        strcmp(base, ".envrc") == 0 || strcmp(base, ".shellrc") == 0) {
        lang = "sh";
        goto found;
    }

    ext = strrchr(filename, '.');
    if (ext == NULL)
        return NULL;
    ext++;

    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0)
        lang = "c";
    else if (strcmp(ext, "py") == 0 || strcmp(ext, "pyw") == 0)
        lang = "python";
    else if (strcmp(ext, "sh") == 0 || strcmp(ext, "bash") == 0 ||
             strcmp(ext, "zsh") == 0 || strcmp(ext, "ksh") == 0)
        lang = "sh";
    else if (strcmp(ext, "js") == 0 || strcmp(ext, "mjs") == 0 ||
             strcmp(ext, "cjs") == 0 || strcmp(ext, "jsx") == 0)
        lang = "js";
    else if (strcmp(ext, "ts") == 0 || strcmp(ext, "tsx") == 0)
        lang = "ts";
    else if (strcmp(ext, "go") == 0)
        lang = "go";
    else if (strcmp(ext, "rs") == 0)
        lang = "rust";
    else if (strcmp(ext, "java") == 0)
        lang = "java";
    else if (strcmp(ext, "php") == 0 || strcmp(ext, "phtml") == 0 ||
             strcmp(ext, "php3") == 0 || strcmp(ext, "php4") == 0 ||
             strcmp(ext, "php5") == 0 || strcmp(ext, "phps") == 0)
        lang = "php";
    else if (strcmp(ext, "css") == 0 || strcmp(ext, "scss") == 0 ||
             strcmp(ext, "sass") == 0 || strcmp(ext, "less") == 0)
        lang = "css";
    else if (strcmp(ext, "vue") == 0)
        lang = "vue";
    else if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0 ||
             strcmp(ext, "xhtml") == 0 || strcmp(ext, "shtml") == 0)
        lang = "html";
    else if (strcmp(ext, "dart") == 0)
        lang = "dart";
    else if (strcmp(ext, "jai") == 0)
        lang = "jai";
    else if (strcmp(ext, "el") == 0 || strcmp(ext, "elc") == 0 ||
             strcmp(ext, "emacs") == 0)
        lang = "elisp";
    else
        return NULL;

found:
    /* already loaded? */
    struct syntax_table *st = syntax_find(lang);
    if (st != NULL)
        return st;

    home = getenv("HOME");
    if (home == NULL)
        home = ".";
    snprintf(path, sizeof(path), "%s/.mg-syntax/%s.syntax", home, lang);
    st = syntax_load(path);
    if (st != NULL)
        st->st_lang = strdup(lang);
    return st;
}
