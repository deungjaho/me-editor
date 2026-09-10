/* Public domain. */

/*
 * Syntax highlighting for mg.
 *
 * Data-driven tokenizer: language rules are loaded from .syntax
 * files at runtime.  The tokenizer scans visible lines and assigns
 * a color code per byte.  Multi-line state (block comments) is
 * carried via l_synstate on each line.
 */

#ifndef SYNTAX_H
#define SYNTAX_H

#include "def.h"  /* for struct line, struct buffer, curbp, etc. */

/* ---- color codes ---- */
/* These are indices into the rgb_table in tty.c.
 * Values start at 16 to avoid collision with CNONE(0), CTEXT(1), CMODE(2). */
enum {
    SCOLOR_DEFAULT  = 16,   /* #BDB89E sand (default text) */
    SCOLOR_KEYWORD  = 17,   /* #BDB89E sand                */
    SCOLOR_TYPE     = 18,   /* #BDB89E sand                */
    SCOLOR_STRING   = 19,   /* #9EEDE1 cyan/aqua           */
    SCOLOR_COMMENT  = 20,   /* #75CE76 lime green          */
    SCOLOR_NUMBER   = 21,   /* #9EEDE1 cyan (data values)  */
    SCOLOR_PREPROC  = 22,   /* #BDB89E sand                */
    SCOLOR_CONTROL  = 23,   /* #FF6B9D rose/magenta        */
    SCOLOR_RULER    = 24,   /* #3B4252 subtle dark gray vertical ruler line */
    SCOLOR_MAX      = 25,
};

/*
 * Region highlight: we OR the hue value with REGION_BIT to indicate
 * the byte is inside the selected region.  The low bits retain the
 * original syntax color so the foreground is preserved.
 */
#define REGION_BIT  0x40
#define HUE_IN_REGION(h)  ((h) & REGION_BIT)
#define HUE_BASE(h)       ((h) & ~REGION_BIT)

/* ---- line syntax state ---- */
enum {
    SYNSTATE_DEFAULT          = 0,
    SYNSTATE_IN_BLOCK_COMMENT = 1,
    SYNSTATE_IN_TRIPLE_DQUOTE = 2,  /* inside """ ... """ */
    SYNSTATE_IN_TRIPLE_SQUOTE = 3,  /* inside ''' ... ''' */
    SYNSTATE_IN_BACKTICK      = 4,  /* inside ` ... ` */
};

/* ---- syntax rule types ---- */
enum {
    SRULE_KEYWORD,
    SRULE_TYPE,
    SRULE_CONTROL,
    SRULE_CONSTANT,
    SRULE_STRING,
    SRULE_LINE_COMMENT,
    SRULE_BLOCK_COMMENT,
    SRULE_LIFETIME,
    SRULE_ANNOTATION,
    SRULE_TAG,
    SRULE_CHAR_LIT,
    SRULE_CSS_PROP,
    SRULE_PREFIX,
    SRULE_INI_SECTION,
    SRULE_MD_HEADER,
    SRULE_MD_QUOTE,
    SRULE_MD_LIST,
};

/* ---- word syntax flags ---- */
#define SYNFLAG_DOLLAR_VARS  0x01   /* Shell/PHP: $ is a word start/char */
#define SYNFLAG_HYPHEN_WORDS 0x02   /* Elisp: - is a word char           */
#define SYNFLAG_JSON_KEYS    0x04   /* JSON: strings followed by ':' are keys */
#define SYNFLAG_INI_KEYS     0x08   /* INI/TOML: words followed by '=' are keys */
#define SYNFLAG_FIXED_TYPES  0x10   /* Match fixed width types: u8..u256, i8..i128, s8..s64, f32/f64 */
#define SYNFLAG_NO_NUMBERS   0x20   /* Disable number literal highlighting (e.g. for Markdown) */
#define SYNFLAG_PAIR_SQUOTE  0x40   /* Auto-pair single quote ' */
#define SYNFLAG_NO_AUTOPAIR  0x80   /* Disable auto-pairing in this mode */

/* ---- data structures ---- */

struct syntax_table;
struct syn_rule;

typedef int (*syn_rule_match_fn)(
    struct syntax_table *st,
    const struct syn_rule *rule,
    const char *text,
    int pos,
    int textlen,
    int *match_len,
    int *color,
    int *state
);

struct syn_rule {
    int                  sr_type;       /* SRULE_* */
    int                  sr_color;      /* SCOLOR_* */
    char                *sr_start;      /* start delimiter / keyword */
    char                *sr_end;        /* end delimiter (NULL = to EOL) */
    syn_rule_match_fn    sr_match;      /* matcher function */
    struct syn_rule     *sr_next;
};

struct syn_kw {                         /* keyword hash entry */
    char                *kw_word;
    int                  kw_color;      /* SCOLOR_* */
    struct syn_kw       *kw_next;
};

#define SYN_HASHSIZE 256

struct syntax_table {
    char                *st_lang;       /* "c", "python", "sh" */
    struct syn_rule     *st_rules;      /* linked list of rules */
    struct syn_kw       *st_kw[SYN_HASHSIZE]; /* keyword hash */
    char                *st_block_start;   /* block comment start */
    char                *st_block_end;     /* block comment end */
    char                *st_line_comment;   /* line comment prefix, e.g. "//", "#", ";" */
    int                  st_flags;         /* SYNFLAG_* bitmask */
    struct syntax_table *st_next;          /* singly linked list of loaded tables */
};

/* ---- functions ---- */

struct syntax_table *syntax_load(const char *path);
void                 syntax_free(struct syntax_table *st);
struct syntax_table *syntax_find(const char *lang);
void                 syntax_sync_lookback(struct buffer *bp, struct line *target_lp);
void                 syntax_match_dired(struct line *lp, char *hue, int ncol);
void                 syntax_match_line(struct line *prev, struct line *lp,
                                       char *hue, int ncol);

/* commands */
int  syntax_on(int f, int n);
int  syntax_off(int f, int n);
int  syntax_mode(int f, int n);
int  syntax_reload(int f, int n);

#endif /* SYNTAX_H */
