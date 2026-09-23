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

static struct syntax_table *g_tables = NULL;  /* loaded tables linked list */

/* ---- keyword hash ---- */

static unsigned int
kw_hash(const char *s, int len, int case_insensitive)
{
    unsigned int h = 5381;
    int i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (case_insensitive)
            c = (unsigned char)tolower(c);
        h = ((h << 5) + h) + c;
    }
    return h % SYN_HASHSIZE;
}

static void
kw_add(struct syntax_table *st, const char *word, int color)
{
    int ci = (st->st_flags & SYNFLAG_CASE_INSENSITIVE);
    unsigned int h = kw_hash(word, strlen(word), ci);
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
    int ci = (st->st_flags & SYNFLAG_CASE_INSENSITIVE);
    unsigned int h = kw_hash(word, len, ci);
    struct syn_kw *e;
    for (e = st->st_kw[h]; e != NULL; e = e->kw_next) {
        if (strlen(e->kw_word) == (size_t)len) {
            if (ci) {
                if (strncasecmp(e->kw_word, word, len) == 0)
                    return e->kw_color;
            } else {
                if (memcmp(e->kw_word, word, len) == 0)
                    return e->kw_color;
            }
        }
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

/* ---- forward declarations of helpers ---- */
static int is_word_start(unsigned char c);
static int is_word_char(unsigned char c);
static int is_word_char_ext(unsigned char c, int flags);
static int is_word_start_ext(unsigned char c, int flags);
static int match_at(const char *text, int pos, int textlen, const char *pat);
static int find_delim(const char *text, int pos, int textlen, const char *delim);
static int find_quote_end(const char *text, int pos, int textlen, char quote);

/* ---- syntax rule matcher functions ---- */

static int
match_rule_string(struct syntax_table *st, const struct syn_rule *rule,
                  const char *text, int pos, int textlen,
                  int *match_len, int *color, int *state)
{
    (void)st;
    if (!match_at(text, pos, textlen, rule->sr_start))
        return 0;

    int slen = strlen(rule->sr_start);
    *color = rule->sr_color;

    if (slen == 1 || (slen == 2 && rule->sr_start[0] == 'b' && (rule->sr_start[1] == '"' || rule->sr_start[1] == '\''))) {
        char quote = (slen == 1) ? rule->sr_start[0] : rule->sr_start[1];
        int end = find_quote_end(text, pos + (slen - 1), textlen, quote);
        *match_len = end - pos;
        if (quote == '`' && pos + *match_len >= textlen &&
            !(pos + *match_len > 0 && text[pos + *match_len - 1] == '`'))
            *state = SYNSTATE_IN_BACKTICK;

        /* JSON keys detection: if string is followed by ':', mark as key */
        if ((st->st_flags & SYNFLAG_JSON_KEYS) && rule->sr_start[0] == '"') {
            int k = end;
            while (k < textlen && (text[k] == ' ' || text[k] == '\t'))
                k++;
            if (k < textlen && text[k] == ':')
                *color = SCOLOR_KEYWORD;
        }

        return 1;
    }

    int end = find_delim(text, pos + slen, textlen, rule->sr_end);
    if (end < 0) {
        *match_len = textlen - pos;
        if (strcmp(rule->sr_start, "\"\"\"") == 0)
            *state = SYNSTATE_IN_TRIPLE_DQUOTE;
        else
            *state = SYNSTATE_IN_TRIPLE_SQUOTE;
    } else {
        *match_len = end - pos;
    }
    return 1;
}

static int
match_rule_line_comment(struct syntax_table *st, const struct syn_rule *rule,
                        const char *text, int pos, int textlen,
                        int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (!match_at(text, pos, textlen, rule->sr_start))
        return 0;
    *color = rule->sr_color;
    *match_len = textlen - pos;
    return 1;
}

static int
match_rule_lifetime(struct syntax_table *st, const struct syn_rule *rule,
                    const char *text, int pos, int textlen,
                    int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (text[pos] != '\'' || pos + 1 >= textlen ||
        !is_word_start((unsigned char)text[pos + 1]))
        return 0;

    /* Single-quoted character literal check: 'X' or '\X' */
    if (text[pos + 1] == '\\') {
        int j = pos + 2;
        while (j < textlen && text[j] != '\'')
            j++;
        if (j < textlen && text[j] == '\'')
            return 0; /* character literal, let string rule handle it */
    } else if (pos + 2 < textlen && text[pos + 2] == '\'') {
        return 0; /* character literal, let string rule handle it */
    }

    int end = pos + 1;
    while (end < textlen && is_word_char((unsigned char)text[end]))
        end++;

    /* Rust lifetimes are short identifiers like 'a, 'de, 'static (rarely exceed 16 chars) */
    if (end - pos > 16)
        return 0;

    /* If closing quote exists right after identifier (e.g. 'foo'), it's a string */
    if (end < textlen && text[end] == '\'')
        return 0;

    *color = rule->sr_color;
    *match_len = end - pos;
    return 1;
}

static int
match_rule_annotation(struct syntax_table *st, const struct syn_rule *rule,
                      const char *text, int pos, int textlen,
                      int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (text[pos] != '@' || pos + 1 >= textlen ||
        !is_word_start((unsigned char)text[pos + 1]))
        return 0;

    /* Prevent matching emails like user@domain.com: left char must not be an identifier char */
    if (pos > 0 && is_word_char((unsigned char)text[pos - 1]))
        return 0;

    int end = pos + 1;
    while (end < textlen && is_word_char((unsigned char)text[end]))
        end++;

    *color = rule->sr_color;
    *match_len = end - pos;
    return 1;
}

static int
match_rule_tag(struct syntax_table *st, const struct syn_rule *rule,
               const char *text, int pos, int textlen,
               int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (text[pos] != '<' || pos + 1 >= textlen ||
        (!is_word_start((unsigned char)text[pos + 1]) && text[pos + 1] != '/'))
        return 0;

    int end = pos + 1;
    if (text[end] == '/')
        end++;

    if (end >= textlen || !is_word_start((unsigned char)text[end]))
        return 0;

    while (end < textlen && is_word_char((unsigned char)text[end]))
        end++;

    while (end < textlen && text[end] == '-' && end + 1 < textlen &&
           is_word_start((unsigned char)text[end + 1])) {
        end++;
        while (end < textlen && is_word_char((unsigned char)text[end]))
            end++;
    }

    *color = rule->sr_color;
    *match_len = end - pos;
    return 1;
}

static int
match_rule_char_lit(struct syntax_table *st, const struct syn_rule *rule,
                    const char *text, int pos, int textlen,
                    int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (text[pos] != '?' || pos + 1 >= textlen)
        return 0;

    unsigned char nc = (unsigned char)text[pos + 1];
    if (nc == ' ' || nc == '\t' || nc == ';')
        return 0;

    int end = pos + 2;
    if (nc == '\\' && end < textlen)
        end++;

    *color = rule->sr_color;
    *match_len = end - pos;
    return 1;
}

static int
match_rule_prefix(struct syntax_table *st, const struct syn_rule *rule,
                  const char *text, int pos, int textlen,
                  int *match_len, int *color, int *state)
{
    (void)state;
    if (!match_at(text, pos, textlen, rule->sr_start))
        return 0;

    int slen = strlen(rule->sr_start);
    int end = pos + slen;

    /* Special shell support: ${VAR} or $(cmd) */
    if (slen == 1 && rule->sr_start[0] == '$' && end < textlen) {
        if (text[end] == '{') {
            end++;
            while (end < textlen && text[end] != '}')
                end++;
            if (end < textlen && text[end] == '}')
                end++;
            *color = rule->sr_color;
            *match_len = end - pos;
            return 1;
        } else if (text[end] == '(') {
            end++;
            while (end < textlen && text[end] != ')')
                end++;
            if (end < textlen && text[end] == ')')
                end++;
            *color = rule->sr_color;
            *match_len = end - pos;
            return 1;
        }
    }

    /* Scan trailing identifier characters */
    while (end < textlen && is_word_char_ext((unsigned char)text[end], st->st_flags))
        end++;

    /* If prefix only matched itself and nothing followed, still color if it was non-empty */
    *color = rule->sr_color;
    *match_len = end - pos;
    return 1;
}

static int
match_rule_ini_section(struct syntax_table *st, const struct syn_rule *rule,
                       const char *text, int pos, int textlen,
                       int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    if (text[pos] != '[')
        return 0;

    /* Check if this is the start of the line or only preceded by whitespace */
    int k;
    for (k = 0; k < pos; k++) {
        if (text[k] != ' ' && text[k] != '\t')
            return 0;
    }

    int end = pos + 1;
    while (end < textlen && text[end] != ']')
        end++;

    if (end < textlen && text[end] == ']') {
        *color = rule->sr_color;
        *match_len = (end + 1) - pos;
        return 1;
    }
    return 0;
}

static int
match_rule_md_header(struct syntax_table *st, const struct syn_rule *rule,
                     const char *text, int pos, int textlen,
                     int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    /* Markdown headers (# ## ### ...) only at start of line (or after spaces) */
    if (text[pos] != '#')
        return 0;

    int k;
    for (k = 0; k < pos; k++) {
        if (text[k] != ' ' && text[k] != '\t')
            return 0;
    }

    /* Entire line becomes header color */
    *color = rule->sr_color;
    *match_len = textlen - pos;
    return 1;
}

static int
match_rule_md_quote(struct syntax_table *st, const struct syn_rule *rule,
                    const char *text, int pos, int textlen,
                    int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    /* Blockquote (>) only at start of line (or after spaces) */
    if (text[pos] != '>')
        return 0;

    int k;
    for (k = 0; k < pos; k++) {
        if (text[k] != ' ' && text[k] != '\t')
            return 0;
    }

    *color = rule->sr_color;
    *match_len = textlen - pos;
    return 1;
}

static int
match_rule_md_list(struct syntax_table *st, const struct syn_rule *rule,
                   const char *text, int pos, int textlen,
                   int *match_len, int *color, int *state)
{
    (void)st;
    (void)state;
    (void)rule;
    /* Bullet or numbered list at line start: '- ', '* ', '+ ', '1. ', '12. ' */
    int k;
    for (k = 0; k < pos; k++) {
        if (text[k] != ' ' && text[k] != '\t')
            return 0;
    }

    unsigned char c = (unsigned char)text[pos];
    if ((c == '-' || c == '*' || c == '+') && pos + 1 < textlen &&
        (text[pos + 1] == ' ' || text[pos + 1] == '\t')) {
        *color = SCOLOR_CONTROL;
        *match_len = 1;
        return 1;
    }

    if (c >= '0' && c <= '9') {
        int end = pos;
        while (end < textlen && text[end] >= '0' && text[end] <= '9')
            end++;
        if (end < textlen && text[end] == '.' && end + 1 < textlen &&
            (text[end + 1] == ' ' || text[end + 1] == '\t')) {
            *color = SCOLOR_CONTROL;
            *match_len = (end + 1) - pos;
            return 1;
        }
    }

    return 0;
}

static void
syn_rule_add(struct syntax_table *st, int type, int color,
             char *start, char *end, syn_rule_match_fn fn)
{
    struct syn_rule *r = calloc(1, sizeof(*r));
    if (r == NULL)
        return;
    r->sr_type = type;
    r->sr_color = color;
    r->sr_start = start;
    r->sr_end = end;
    r->sr_match = fn;

    /* Append to end of rule list to maintain declaration order */
    if (st->st_rules == NULL) {
        st->st_rules = r;
    } else {
        struct syn_rule *curr = st->st_rules;
        while (curr->sr_next != NULL)
            curr = curr->sr_next;
        curr->sr_next = r;
    }
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
        } else if (strncmp(p, "constant:", 9) == 0) {
            kw_add(st, skip_ws(p + 9), SCOLOR_NUMBER);
        } else if (strncmp(p, "string:", 7) == 0) {
            char *s, *e;
            parse_delim(p + 7, &s, &e);
            if (s) {
                syn_rule_add(st, SRULE_STRING, SCOLOR_STRING,
                             s, e ? e : strdup(s), match_rule_string);
            }
        } else if (strncmp(p, "prefix:", 7) == 0) {
            char *spec = skip_ws(p + 7);
            int color = SCOLOR_CONTROL;
            if (strncmp(spec, "control", 7) == 0) {
                color = SCOLOR_CONTROL;
                spec = skip_ws(spec + 7);
            } else if (strncmp(spec, "keyword", 7) == 0) {
                color = SCOLOR_KEYWORD;
                spec = skip_ws(spec + 7);
            } else if (strncmp(spec, "type", 4) == 0) {
                color = SCOLOR_TYPE;
                spec = skip_ws(spec + 4);
            } else if (strncmp(spec, "string", 6) == 0) {
                color = SCOLOR_STRING;
                spec = skip_ws(spec + 6);
            } else if (strncmp(spec, "comment", 7) == 0) {
                color = SCOLOR_COMMENT;
                spec = skip_ws(spec + 7);
            }
            if (*spec != '\0') {
                syn_rule_add(st, SRULE_PREFIX, color,
                             strdup(spec), NULL, match_rule_prefix);
            }
        } else if (strncmp(p, "line_comment:", 13) == 0) {
            char *s, *e;
            parse_delim(p + 13, &s, &e);
            if (s) {
                if (st->st_line_comment == NULL)
                    st->st_line_comment = strdup(s);
                syn_rule_add(st, SRULE_LINE_COMMENT, SCOLOR_COMMENT,
                             s, NULL, match_rule_line_comment);
            }
            free(e);
        } else if (strncmp(p, "block_comment:", 14) == 0) {
            char *s, *e;
            parse_delim(p + 14, &s, &e);
            if (s && e) {
                st->st_block_start = s;
                st->st_block_end = e;
            } else {
                free(s);
                free(e);
            }
        } else if (strncmp(p, "number:", 7) == 0) {
            /* numbers auto-detected */
        } else if (strncmp(p, "flags:", 6) == 0) {
            char *spec = skip_ws(p + 6);
            char *tok = strtok(spec, " \t");
            while (tok) {
                if (strcmp(tok, "tags") == 0) {
                    syn_rule_add(st, SRULE_TAG, SCOLOR_CONTROL,
                                 strdup("<"), NULL, match_rule_tag);
                } else if (strcmp(tok, "annotations") == 0) {
                    syn_rule_add(st, SRULE_ANNOTATION, SCOLOR_CONTROL,
                                 strdup("@"), NULL, match_rule_annotation);
                } else if (strcmp(tok, "lifetime") == 0) {
                    syn_rule_add(st, SRULE_LIFETIME, SCOLOR_KEYWORD,
                                 strdup("'"), NULL, match_rule_lifetime);
                } else if (strcmp(tok, "char_lit") == 0) {
                    syn_rule_add(st, SRULE_CHAR_LIT, SCOLOR_STRING,
                                 strdup("?"), NULL, match_rule_char_lit);
                } else if (strcmp(tok, "css_props") == 0) {
                    st->st_flags |= SRULE_CSS_PROP;
                } else if (strcmp(tok, "hyphen_words") == 0) {
                    st->st_flags |= SYNFLAG_HYPHEN_WORDS;
                } else if (strcmp(tok, "dollar_vars") == 0) {
                    st->st_flags |= SYNFLAG_DOLLAR_VARS;
                } else if (strcmp(tok, "json_keys") == 0) {
                    st->st_flags |= SYNFLAG_JSON_KEYS;
                } else if (strcmp(tok, "ini_section") == 0) {
                    syn_rule_add(st, SRULE_INI_SECTION, SCOLOR_CONTROL,
                                 strdup("["), NULL, match_rule_ini_section);
                } else if (strcmp(tok, "ini_keys") == 0) {
                    st->st_flags |= SYNFLAG_INI_KEYS;
                } else if (strcmp(tok, "md_headers") == 0) {
                    syn_rule_add(st, SRULE_MD_HEADER, SCOLOR_CONTROL,
                                 strdup("#"), NULL, match_rule_md_header);
                } else if (strcmp(tok, "md_quotes") == 0) {
                    syn_rule_add(st, SRULE_MD_QUOTE, SCOLOR_COMMENT,
                                 strdup(">"), NULL, match_rule_md_quote);
                } else if (strcmp(tok, "md_lists") == 0) {
                    syn_rule_add(st, SRULE_MD_LIST, SCOLOR_CONTROL,
                                 strdup("-"), NULL, match_rule_md_list);
                } else if (strcmp(tok, "fixed_types") == 0) {
                    st->st_flags |= SYNFLAG_FIXED_TYPES;
                } else if (strcmp(tok, "no_numbers") == 0) {
                    st->st_flags |= SYNFLAG_NO_NUMBERS;
                } else if (strcmp(tok, "pair_squote") == 0) {
                    st->st_flags |= SYNFLAG_PAIR_SQUOTE;
                } else if (strcmp(tok, "no_autopair") == 0) {
                    st->st_flags |= SYNFLAG_NO_AUTOPAIR;
                } else if (strcmp(tok, "case_insensitive") == 0) {
                    st->st_flags |= SYNFLAG_CASE_INSENSITIVE;
                }
                tok = strtok(NULL, " \t");
            }
        }
    }
    fclose(fp);

    /* register in global table (linked list) */
    st->st_next = g_tables;
    g_tables = st;

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
    free(st->st_line_comment);
    free(st->st_lang);
    free(st);
}

struct syntax_table *
syntax_find(const char *lang)
{
    struct syntax_table *curr = g_tables;
    while (curr != NULL) {
        if (curr->st_lang && strcmp(curr->st_lang, lang) == 0)
            return curr;
        curr = curr->st_next;
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
is_fixed_type(const char *word, int len)
{
    /* Match fixed width types like:
     * u8, u16, u32, u64, u128, u256
     * i8, i16, i32, i64, i128
     * s8, s16, s32, s64 (Jai signed integers)
     * f32, f64
     */
    if (len < 2 || len > 5)
        return 0;

    char prefix = word[0];
    if (prefix != 'u' && prefix != 'i' && prefix != 's' && prefix != 'f')
        return 0;

    int i;
    for (i = 1; i < len; i++) {
        if (word[i] < '0' || word[i] > '9')
            return 0;
    }
    return 1;
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

/* ---- dired highlighter ---- */

void
syntax_match_dired(struct line *lp, char *hue, int ncol)
{
    const char *text = lp->l_text;
    int textlen = llength(lp);

    memset(hue, SCOLOR_DEFAULT, textlen);

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
                memset(hue + name_start, SCOLOR_CONTROL, textlen - name_start);
            else if (is_link)
                memset(hue + name_start, SCOLOR_STRING, textlen - name_start);
            else
                memset(hue + name_start, SCOLOR_DEFAULT, textlen - name_start);
        }
    }
}

/* ---- sync lookback helper ---- */

void
syntax_sync_lookback(struct buffer *bp, struct line *target_lp)
{
    struct line *curr;
    int count = 0;
    int max_lookback = 1000;  /* Expanded lookback depth for large multi-line comment blocks */
    struct line **stack = NULL;
    int stack_cap = 0;
    int top = 0;

    if (bp == NULL || bp->b_syntax == NULL || target_lp == NULL)
        return;

    curr = lback(target_lp);
    while (curr != bp->b_headp && count < max_lookback) {
        if (curr->l_synstate != SYNSTATE_DEFAULT)
            break;  /* Found a known state anchor */

        if (top >= stack_cap) {
            int new_cap = stack_cap ? stack_cap * 2 : 128;
            struct line **new_stack = realloc(stack, new_cap * sizeof(struct line *));
            if (!new_stack)
                break;
            stack = new_stack;
            stack_cap = new_cap;
        }
        stack[top++] = curr;
        curr = lback(curr);
        count++;
    }

    /* Fast-forward scan through the stack to compute line states */
    struct line *prev = curr;
    while (top > 0) {
        struct line *lp = stack[--top];
        char dummy_hue[1];
        syntax_match_line(prev, lp, dummy_hue, 0);
        prev = lp;
    }
    if (stack)
        free(stack);
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

    /* default color for all bytes (entire line, not just visible width) */
    if (ncol > 0)
        memset(hue, SCOLOR_DEFAULT, textlen);

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

        /* ---- custom rules (strings, comments, tags, annotations, etc.) ---- */
        {
            struct syn_rule *r;
            int matched = 0;
            for (r = st->st_rules; r != NULL; r = r->sr_next) {
                if (r->sr_match == NULL)
                    continue;
                int match_len = 0;
                int color = SCOLOR_DEFAULT;
                if (r->sr_match(st, r, text, i, textlen, &match_len, &color, &state)) {
                    memset(hue + i, color, match_len);
                    i += match_len;
                    matched = 1;
                    break;
                }
            }
            if (matched)
                continue;
        }

        /* ---- number ---- */
        if (c >= '0' && c <= '9' && !(st->st_flags & SYNFLAG_NO_NUMBERS)) {
            int end = i;
            while (end < textlen) {
                unsigned char ch = (unsigned char)text[end];
                if (ch == '.') {
                    /* If followed by another '.', it's a range operator '..' (e.g. 0..n); stop here */
                    if (end + 1 < textlen && text[end + 1] == '.')
                        break;
                    /* A single '.' is only part of a float if followed by a digit */
                    if (end + 1 < textlen && text[end + 1] >= '0' && text[end + 1] <= '9') {
                        end++;
                        continue;
                    }
                    break;
                }
                if (is_word_char(ch)) {
                    end++;
                    continue;
                }
                break;
            }
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
            int color = kw_lookup(st, text + i, wlen);

            /* Dot-property demotion: words preceded by '.' (e.g. obj.string()) are member accesses, not types */
            if (color == SCOLOR_TYPE && i > 0 && text[i - 1] == '.') {
                if (i == 1 || text[i - 2] != '.')
                    color = SCOLOR_DEFAULT;
            }

            if (color != SCOLOR_DEFAULT) {
                memset(hue + i, color, wlen);
            } else if ((st->st_flags & SYNFLAG_FIXED_TYPES) &&
                       is_fixed_type(text + i, wlen)) {
                /* Exact fixed width types (e.g. u64, u128, i64, s64, f64) unless preceded by '.' */
                if (!(i > 0 && text[i - 1] == '.' && (i == 1 || text[i - 2] != '.')))
                    memset(hue + i, SCOLOR_TYPE, wlen);
            } else if ((st->st_flags & SRULE_CSS_PROP) &&
                       end < textlen && text[end] == ':') {
                /* CSS property name: only consider if preceded by indentation or ';' (not after selectors like a:hover) */
                int is_prop = 0;
                int k;
                for (k = 0; k < i; k++) {
                    if (text[k] != ' ' && text[k] != '\t') {
                        is_prop = (text[k] == ';' || text[k] == '{');
                        break;
                    }
                }
                /* If line starts with indentation, it's inside a rule block */
                if (i > 0 && (text[0] == ' ' || text[0] == '\t'))
                    is_prop = 1;
                if (is_prop && !(end + 1 < textlen && text[end + 1] == ':'))
                    memset(hue + i, SCOLOR_CONTROL, wlen);
            } else if ((st->st_flags & SYNFLAG_INI_KEYS)) {
                /* INI/TOML key: word followed by '=' */
                int k = end;
                while (k < textlen && (text[k] == ' ' || text[k] == '\t'))
                    k++;
                if (k < textlen && text[k] == '=')
                    memset(hue + i, SCOLOR_KEYWORD, wlen);
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
    struct syntax_table *curr, *next;

    /* First clear all dangling syntax pointers from all active buffers */
    for (bp = bheadp; bp != NULL; bp = bp->b_bufp) {
        bp->b_syntax = NULL;
    }

    /* free all cached tables in linked list */
    curr = g_tables;
    while (curr != NULL) {
        next = curr->st_next;
        syntax_free(curr);
        curr = next;
    }
    g_tables = NULL;

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
    else if (strcmp(ext, "sol") == 0)
        lang = "solidity";
    else if (strcmp(ext, "json") == 0 || strcmp(ext, "jsonc") == 0)
        lang = "json";
    else if (strcmp(ext, "ini") == 0 || strcmp(ext, "conf") == 0 ||
             strcmp(ext, "cfg") == 0)
        lang = "ini";
    else if (strcmp(ext, "toml") == 0)
        lang = "toml";
    else if (strcmp(ext, "move") == 0)
        lang = "move";
    else if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0)
        lang = "markdown";
    else if (strcmp(ext, "xml") == 0 || strcmp(ext, "svg") == 0 ||
             strcmp(ext, "plist") == 0)
        lang = "xml";
    else if (strcmp(ext, "sql") == 0)
        lang = "sql";
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
