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
    SCOLOR_MAX      = 24,
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
    SRULE_STRING,
    SRULE_LINE_COMMENT,
    SRULE_BLOCK_COMMENT,
    SRULE_NUMBER,
};

/* ---- syntax flags (language-specific tokenizer modes) ---- */
#define SYNFLAG_TAGS        0x01    /* HTML/Vue: <tag> detection        */
#define SYNFLAG_ANNOTATIONS 0x02    /* Java/Swift: @Annotation detection */
#define SYNFLAG_CSS_PROPS   0x04    /* CSS: property: detection          */
#define SYNFLAG_HYPHEN_WORDS 0x08   /* Elisp: - is a word char          */
#define SYNFLAG_DOLLAR_VARS  0x10   /* Shell/PHP: $ is a word start     */
#define SYNFLAG_CHAR_LIT     0x20   /* Elisp: ?a character literals     */

/* ---- data structures ---- */

struct syn_rule {
    int                  sr_type;       /* SRULE_* */
    char                *sr_start;      /* start delimiter / keyword */
    char                *sr_end;        /* end delimiter (NULL = to EOL) */
    struct syn_rule     *sr_next;
};

struct syn_kw {                         /* keyword hash entry */
    char                *kw_word;
    int                  kw_color;      /* SCOLOR_KEYWORD or SCOLOR_TYPE */
    struct syn_kw       *kw_next;
};

#define SYN_HASHSIZE 256

struct syntax_table {
    char                *st_lang;       /* "c", "python", "sh" */
    struct syn_rule     *st_rules;      /* linked list of rules */
    struct syn_kw       *st_kw[SYN_HASHSIZE]; /* keyword hash */
    char               **st_strings;    /* string delimiter pairs */
    int                  st_nstrings;
    char               **st_line_comments; /* line comment prefixes */
    int                  st_nline_comments;
    char                *st_block_start;   /* block comment start */
    char                *st_block_end;     /* block comment end */
    int                  st_flags;         /* SYNFLAG_* bitmask */
};

/* ---- functions ---- */

struct syntax_table *syntax_load(const char *path);
void                 syntax_free(struct syntax_table *st);
struct syntax_table *syntax_find(const char *lang);
void                 syntax_match_line(struct line *prev, struct line *lp,
                                       char *hue, int ncol);

/* commands */
int  syntax_on(int f, int n);
int  syntax_off(int f, int n);
int  syntax_mode(int f, int n);
int  syntax_reload(int f, int n);

#endif /* SYNTAX_H */
