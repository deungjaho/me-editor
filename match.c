/*	$OpenBSD: match.c,v 1.25 2023/04/21 13:39:37 op Exp $	*/

/* This file is in the public domain. */

/*
 * Parenthesis matching & auto-pairing routines for me.
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "def.h"
#include "key.h"
#include "syntax.h"

/*
 * Balance table for brackets.
 */
static struct balance {
	char	left, right;
} bal[] = {
	{ '(', ')' },
	{ '[', ']' },
	{ '{', '}' },
	{ '\0', '\0' }
};

/*
 * Auto-pairing insertion:
 * Intercepts '(', '[', '{', '"', '\'' and manages smart-close / skip-over.
 */
int
autopair_insert(int f, int n)
{
	int c;
	char rchar = '\0';
	struct line *lp;
	int doto;

	if (n < 0)
		return (FALSE);
	if (n == 0)
		return (TRUE);

	c = key.k_chars[key.k_count - 1];
	lp = curwp->w_dotp;
	doto = curwp->w_doto;

	/* Check if auto-pairing is disabled for this buffer */
	if (curbp->b_syntax && (curbp->b_syntax->st_flags & SYNFLAG_NO_AUTOPAIR))
		return (selfinsert(f, n));

	/*
	 * Skip-over check:
	 * If user types a closing bracket or quote and the character under dot
	 * is already that exact character, simply step forward without inserting.
	 */
	if (doto < llength(lp)) {
		char next_c = lgetc(lp, doto);
		if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') &&
		    next_c == c) {
			curwp->w_doto++;
			curwp->w_rflag |= WFMOVE;
			return (TRUE);
		}
	}

	/* Determine if `c` is an opening delimiter */
	if (c == '(')
		rchar = ')';
	else if (c == '[')
		rchar = ']';
	else if (c == '{')
		rchar = '}';
	else if (c == '"')
		rchar = '"';
	else if (c == '\'') {
		/*
		 * Single-quote auto-pairing is language-specific:
		 * Only pair if buffer has SYNFLAG_PAIR_SQUOTE flag set (JS, TS, Python, Shell, etc.).
		 * Also don't pair if preceding char is an alphanumeric (like don't, it's).
		 */
		int pair_squote = 0;
		if (curbp->b_syntax && (curbp->b_syntax->st_flags & SYNFLAG_PAIR_SQUOTE))
			pair_squote = 1;

		if (pair_squote && doto > 0 && isalnum((unsigned char)lgetc(lp, doto - 1)))
			pair_squote = 0;

		if (pair_squote)
			rchar = '\'';
	}

	/* If not an auto-paired delimiter, fall back to selfinsert */
	if (rchar == '\0')
		return (selfinsert(f, n));

	/*
	 * Don't pair if immediately followed by an alphanumeric character
	 * (e.g. wrapping or typing inside a word)
	 */
	if (doto < llength(lp)) {
		char next_c = lgetc(lp, doto);
		if (isalnum((unsigned char)next_c))
			return (selfinsert(f, n));
	}

	/* Insert opening and closing delimiter pair, leave cursor between them */
	if (linsert(1, c) == FALSE)
		return (FALSE);
	if (linsert(1, rchar) == FALSE)
		return (FALSE);

	/* Put dot back by 1 character */
	curwp->w_doto--;
	curwp->w_rflag |= WFMOVE;
	return (TRUE);
}

/*
 * Smart backspace:
 * If deleting between matching pair () [] {} "" '', delete both.
 * Supports rapid consecutive deletes, multi-count deletes, and boundary edge cases.
 */
int
smart_backdel(int f, int n)
{
	if (n < 0)
		return (FALSE);

	while (n > 0) {
		struct line *lp = curwp->w_dotp;
		int doto = curwp->w_doto;

		if (doto > 0 && doto < llength(lp)) {
			char prev_c = lgetc(lp, doto - 1);
			char next_c = lgetc(lp, doto);
			if ((prev_c == '(' && next_c == ')') ||
			    (prev_c == '[' && next_c == ']') ||
			    (prev_c == '{' && next_c == '}') ||
			    (prev_c == '"' && next_c == '"') ||
			    (prev_c == '\'' && next_c == '\'')) {
				/* Delete right partner first, then left char */
				(void)forwdel(FFRAND, 1);
				if (backdel(f, 1) == FALSE)
					return (FALSE);
				n--;
				continue;
			}
		}

		/* Regular backdel for this step */
		if (backdel(f, 1) == FALSE)
			return (FALSE);
		n--;
	}
	return (TRUE);
}

/*
 * Bidirectional search for a matching parenthesis/bracket.
 * If dot is on or right after a paren/bracket, searches in the matching
 * direction (forward for opening, backward for closing).
 *
 * Returns 1 if found, with out_lp and out_off set to the matching location,
 * and out_lineno set to the buffer line number. Returns 0 otherwise.
 */
int
find_match_paren(struct line *cur_lp, int cur_off,
                 struct line **out_lp, int *out_off, int *out_lineno)
{
	char ch, target, partner;
	int dir; /* +1 for forward, -1 for backward */
	int i, depth;
	struct line *lp;
	int off;
	int line_no;

	if (cur_lp == NULL)
		return (0);

	/* Check char at dot */
	ch = (cur_off < llength(cur_lp)) ? lgetc(cur_lp, cur_off) : '\0';
	dir = 0;

	for (i = 0; bal[i].left != '\0'; i++) {
		if (ch == bal[i].left) {
			target = bal[i].left;
			partner = bal[i].right;
			dir = 1;
			break;
		}
		if (ch == bal[i].right) {
			target = bal[i].right;
			partner = bal[i].left;
			dir = -1;
			break;
		}
	}

	/* If not on a paren, check char right before dot (common after typing a closing paren) */
	if (dir == 0 && cur_off > 0) {
		ch = lgetc(cur_lp, cur_off - 1);
		for (i = 0; bal[i].left != '\0'; i++) {
			if (ch == bal[i].left) {
				target = bal[i].left;
				partner = bal[i].right;
				dir = 1;
				cur_off--;
				break;
			}
			if (ch == bal[i].right) {
				target = bal[i].right;
				partner = bal[i].left;
				dir = -1;
				cur_off--;
				break;
			}
		}
	}

	if (dir == 0)
		return (0);

	/* Search loop */
	lp = cur_lp;
	off = cur_off;
	depth = 0;
	line_no = curwp->w_dotline;

	if (dir == 1) {
		/* Forward search */
		for (;;) {
			off++;
			while (off >= llength(lp)) {
				lp = lforw(lp);
				line_no++;
				if (lp == curbp->b_headp)
					return (0);
				off = 0;
			}
			char c = lgetc(lp, off);
			if (c == target) {
				depth++;
			} else if (c == partner) {
				if (depth == 0) {
					*out_lp = lp;
					*out_off = off;
					*out_lineno = line_no;
					return (1);
				}
				depth--;
			}
		}
	} else {
		/* Backward search */
		for (;;) {
			if (off == 0) {
				lp = lback(lp);
				line_no--;
				if (lp == curbp->b_headp)
					return (0);
				off = llength(lp);
			} else {
				off--;
				char c = lgetc(lp, off);
				if (c == target) {
					depth++;
				} else if (c == partner) {
					if (depth == 0) {
						*out_lp = lp;
						*out_off = off;
						*out_lineno = line_no;
						return (1);
					}
					depth--;
				}
			}
		}
	}
	return (0);
}

/* Compatibility dummy for older call sites */
int
showmatch(int f, int n)
{
	return selfinsert(f, n);
}

/*
 * Get the single-line comment prefix for the current buffer.
 * Defaults to "// " or "# " if not specified by the syntax table.
 */
static const char *
get_comment_prefix(struct buffer *bp)
{
	if (bp && bp->b_syntax && bp->b_syntax->st_line_comment)
		return bp->b_syntax->st_line_comment;

	return "//";
}

/*
 * Toggle line comment on current line or lines in selected region.
 * Bound to Alt-; (M-;) and Cmd-/
 */
int
comment_line(int f, int n)
{
	struct line *start_lp, *end_lp, *lp;
	const char *prefix;
	int plen, s;
	int has_region = FALSE;

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read-only");
		return (FALSE);
	}

	prefix = get_comment_prefix(curbp);
	plen = strlen(prefix);

	/* Check if there is an active region */
	if (curwp->w_markp != NULL) {
		if (curwp->w_markline < curwp->w_dotline ||
		    (curwp->w_markline == curwp->w_dotline &&
		     curwp->w_marko <= curwp->w_doto)) {
			start_lp = curwp->w_markp;
			end_lp = curwp->w_dotp;
		} else {
			start_lp = curwp->w_dotp;
			end_lp = curwp->w_markp;
		}
		has_region = TRUE;
	} else {
		start_lp = curwp->w_dotp;
		end_lp = curwp->w_dotp;
	}

	/*
	 * First pass: determine if ALL non-empty lines are already commented.
	 * If so, we uncomment. Otherwise, we comment them.
	 */
	int all_commented = TRUE;
	int non_empty_lines = 0;

	for (lp = start_lp;; lp = lforw(lp)) {
		int textlen = llength(lp);
		int k = 0;
		while (k < textlen && (lp->l_text[k] == ' ' || lp->l_text[k] == '\t'))
			k++;
		if (k < textlen) {
			non_empty_lines++;
			if (textlen - k < plen || memcmp(&lp->l_text[k], prefix, plen) != 0) {
				all_commented = FALSE;
				break;
			}
		}
		if (lp == end_lp || lp == curbp->b_headp)
			break;
	}

	if (non_empty_lines == 0)
		all_commented = FALSE;

	/*
	 * Second pass: apply comment or uncomment
	 */
	undo_boundary_enable(FFRAND, 0);

	for (lp = start_lp;; lp = lforw(lp)) {
		int textlen = llength(lp);
		int k = 0;
		while (k < textlen && (lp->l_text[k] == ' ' || lp->l_text[k] == '\t'))
			k++;

		if (all_commented) {
			/* Uncomment */
			if (k < textlen && textlen - k >= plen &&
			    memcmp(&lp->l_text[k], prefix, plen) == 0) {
				int dellen = plen;
				/* If followed by a space, also delete that space */
				if (k + plen < textlen && lp->l_text[k + plen] == ' ')
					dellen++;
				curwp->w_dotp = lp;
				curwp->w_doto = k;
				(void)ldelete((RSIZE)dellen, KNONE);
			}
		} else {
			/* Comment: insert prefix at indentation */
			if (textlen > 0 || !has_region) {
				curwp->w_dotp = lp;
				curwp->w_doto = k;
				for (int p = 0; p < plen; p++)
					(void)linsert(1, prefix[p]);
				(void)linsert(1, ' ');
			}
		}

		if (lp == end_lp || lp == curbp->b_headp)
			break;
	}

	undo_boundary_enable(FFRAND, 1);
	lchange(WFFULL);

	return (TRUE);
}
