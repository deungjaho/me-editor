/*	$OpenBSD: display.c,v 1.52 2023/04/21 13:39:37 op Exp $	*/

/* This file is in the public domain. */

/*
 * The functions in this file handle redisplay. The
 * redisplay system knows almost nothing about the editing
 * process; the editing functions do, however, set some
 * hints to eliminate a lot of the grinding. There is more
 * that can be done; the "vtputc" interface is a real
 * pig.
 */

#include <sys/queue.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#include "def.h"
#include "kbd.h"
#include "utf8.h"
#include "syntax.h"

/*
 * A video structure holds one screen line.  Each column has a
 * UTF8_MAX_BYTES-byte slot in v_text (holding the raw UTF-8 bytes
 * of the character that starts there) and a width entry in v_wid
 * (0 = continuation column of a wide char, 1 = normal, 2 = CJK).
 */
struct video {
	short	v_hash;		/* Hash code, for compares.	 */
	short	v_flag;		/* Flag word.			 */
	short	v_color;	/* Color of the line.		 */
	int	v_cost;		/* Cost of display.		 */
	char	*v_text;	/* UTF-8 bytes, ncol*UTF8_MAX_BYTES */
	char	*v_wid;		/* Display width per column.	 */
	char	*v_hue;		/* Syntax color per column.	 */
};

#define VSLOT(vp, col)	(&(vp)->v_text[(col) * UTF8_MAX_BYTES])

#define VFCHG	0x0001			/* Changed.			 */
#define VFHBAD	0x0002			/* Hash and cost are bad.	 */
#define VFEXT	0x0004			/* extended line (beyond ncol)	 */

/*
 * SCORE structures hold the optimal
 * trace trajectory, and the cost of redisplay, when
 * the dynamic programming redisplay code is used.
 */
struct score {
	int	s_itrace;	/* "i" index for track back.	 */
	int	s_jtrace;	/* "j" index for trace back.	 */
	int	s_cost;		/* Display cost.		 */
};

void	vtmove(int, int);
void	vtputc(int, struct mgwin *);
void	vtputchar(struct line *, int *, struct mgwin *, const char *);
void	vtpute(int, struct mgwin *);
void	vtputechar(struct line *, int *, struct mgwin *, const char *);
int	vtputs(const char *, struct mgwin *);
void	vteeol(void);
void	updext(int, int);
void	modeline(struct mgwin *, int);
void	setscores(int, int);
void	traceback(int, int, int, int);
void	ucopy(struct video *, struct video *);
void	uline(int, struct video *, struct video *);
void	hash(struct video *);


int	sgarbf = TRUE;		/* TRUE if screen is garbage.	 */
int	vtrow = HUGE;		/* Virtual cursor row.		 */
int	vtcol = HUGE;		/* Virtual cursor column.	 */
int	tthue = CNONE;		/* Current color.		 */
int	ttrow = HUGE;		/* Physical cursor row.		 */
int	ttcol = HUGE;		/* Physical cursor column.	 */
int	tttop = HUGE;		/* Top of scroll region.	 */
int	ttbot = HUGE;		/* Bottom of scroll region.	 */
int	lbound = 0;		/* leftmost bound of the current */
				/* line being displayed		 */
static char	vt_hue = 0;	/* current syntax hue for vtputc */
static char	*g_hue = NULL;	/* per-byte syntax colors for current line */
static int	g_hue_size = 0;	/* allocated size of g_hue */
static int	vt_rightcol = 0;	/* right column limit for current window */

/* Ensure g_hue can hold at least `need` bytes. */
static void
ensure_hue(int need)
{
	if (need > g_hue_size) {
		int newsize = g_hue_size ? g_hue_size * 2 : 1024;
		while (newsize < need)
			newsize *= 2;
		g_hue = realloc(g_hue, newsize);
		if (g_hue == NULL)
			panic("ensure_hue: out of memory");
		g_hue_size = newsize;
	}
}

/* Ensure g_hue can hold both the line length and the terminal width. */
static inline void
ensure_hue_line(struct line *lp)
{
	int need = llength(lp) + 1;
	if (ncol > need)
		need = ncol;
	ensure_hue(need);
}

/* ---- transient region highlight ---- */
static int	reg_active = 0;
static int	reg_start_line = 0, reg_start_off = 0;
static int	reg_end_line = 0, reg_end_off = 0;

/*
 * Compute the region boundaries from mark and dot.
 * Called at the top of update() so the rendering loops can apply
 * the highlight per-line.
 */
static void
compute_region(void)
{
	if (curwp->w_markp == NULL) {
		reg_active = 0;
		return;
	}
	reg_active = 1;
	if (curwp->w_markline < curwp->w_dotline ||
	    (curwp->w_markline == curwp->w_dotline &&
	     curwp->w_marko <= curwp->w_doto)) {
		reg_start_line = curwp->w_markline;
		reg_start_off  = curwp->w_marko;
		reg_end_line   = curwp->w_dotline;
		reg_end_off    = curwp->w_doto;
	} else {
		reg_start_line = curwp->w_dotline;
		reg_start_off  = curwp->w_doto;
		reg_end_line   = curwp->w_markline;
		reg_end_off    = curwp->w_marko;
	}
}

/*
 * Override g_hue[] with CREGION for bytes that fall inside the
 * mark-dot region.  line_num is the 1-based buffer line number of lp.
 */
static void
apply_region_hue(struct line *lp, int line_num)
{
	int j, start_j, end_j;

	if (!reg_active)
		return;
	if (line_num < reg_start_line || line_num > reg_end_line)
		return;
	start_j = 0;
	end_j = llength(lp);
	if (line_num == reg_start_line)
		start_j = reg_start_off;
	if (line_num == reg_end_line)
		end_j = reg_end_off;
	for (j = start_j; j < end_j && j < llength(lp); j++)
		g_hue[j] |= REGION_BIT;
}

struct video	**vscreen;		/* Edge vector, virtual.	 */
struct video	**pscreen;		/* Edge vector, physical.	 */
struct video	 *video;		/* Actual screen data.		 */

/*
 * After vteeol(), mark trailing blank columns with REGION_BIT for
 * lines that are fully inside the region (not the first or last line,
 * which may be partially selected).  This makes the selection extend
 * to the right edge of the screen on fully-selected lines, like Emacs.
 */
static void
apply_region_eol(int line_num)
{
	struct video *vp;
	int col;

	if (!reg_active)
		return;
	/* Only fully-enclosed lines get full-width highlight. */
	if (line_num <= reg_start_line || line_num >= reg_end_line)
		return;
	vp = vscreen[vtrow];
	for (col = 0; col < ncol; col++) {
		int h = vp->v_hue[col];
		/* Mark any non-region column that is a blank space. */
		if (!HUE_IN_REGION(h) && VSLOT(vp, col)[0] == ' ' &&
		    vp->v_wid[col] == 1)
			vp->v_hue[col] = h | REGION_BIT;
	}
}
struct video	  blanks;		/* Blank line image.		 */

/*
 * This matrix is written as an array because
 * we do funny things in the "setscores" routine, which
 * is very compute intensive, to make the subscripts go away.
 * It would be "SCORE	score[NROW][NROW]" in old speak.
 * Look at "setscores" to understand what is up.
 */
struct score *score;			/* [NROW * NROW] */

static int	 linenos = TRUE;
static int	 colnos = FALSE;

/* Is macro recording enabled? */
extern int macrodef;
/* Is working directory global? */
extern int globalwd;

/*
 * Since we don't have variables (we probably should) these are command
 * processors for changing the values of mode flags.
 */
int
linenotoggle(int f, int n)
{
	if (f & FFARG)
		linenos = n > 0;
	else
		linenos = !linenos;

	sgarbf = TRUE;

	return (TRUE);
}

int
colnotoggle(int f, int n)
{
	if (f & FFARG)
		colnos = n > 0;
	else
		colnos = !colnos;

	sgarbf = TRUE;

	return (TRUE);
}

/*
 * Reinit the display data structures, this is called when the terminal
 * size changes.
 */
int
vtresize(int force, int newrow, int newcol)
{
	int	 i;
	int	 rowchanged, colchanged;
	static	 int first_run = 1;
	struct video	*vp;

	if (newrow < 1 || newcol < 1)
		return (FALSE);

	rowchanged = (newrow != nrow);
	colchanged = (newcol != ncol);

#define TRYREALLOC(a, n) do {					\
		void *tmp;					\
		if ((tmp = realloc((a), (n))) == NULL) {	\
			panic("out of memory in display code");	\
		}						\
		(a) = tmp;					\
	} while (0)

#define TRYREALLOCARRAY(a, n, m) do {				\
		void *tmp;					\
		if ((tmp = reallocarray((a), (n), (m))) == NULL) {\
			panic("out of memory in display code");	\
		}						\
		(a) = tmp;					\
	} while (0)

	/* No update needed */
	if (!first_run && !force && !rowchanged && !colchanged)
		return (TRUE);

	if (first_run)
		memset(&blanks, 0, sizeof(blanks));

	if (rowchanged || first_run) {
		int vidstart;

		/*
		 * This is not pretty.
		 */
		if (nrow == 0)
			vidstart = 0;
		else
			vidstart = 2 * (nrow - 1);

		/*
		 * We're shrinking, free some internal data.
		 */
		if (newrow < nrow) {
			for (i = 2 * (newrow - 1); i < 2 * (nrow - 1); i++) {
				free(video[i].v_text);
				free(video[i].v_wid);
				free(video[i].v_hue);
				video[i].v_text = NULL;
				video[i].v_wid = NULL;
				video[i].v_hue = NULL;
			}
		}

		TRYREALLOCARRAY(score, newrow, newrow * sizeof(struct score));
		TRYREALLOCARRAY(vscreen, (newrow - 1), sizeof(struct video *));
		TRYREALLOCARRAY(pscreen, (newrow - 1), sizeof(struct video *));
		TRYREALLOCARRAY(video, (newrow - 1), 2 * sizeof(struct video));

		/*
		 * Zero-out the entries we just allocated.
		 */
		for (i = vidstart; i < 2 * (newrow - 1); i++)
			memset(&video[i], 0, sizeof(struct video));

		/*
		 * Reinitialize vscreen and pscreen arrays completely.
		 */
		vp = &video[0];
		for (i = 0; i < newrow - 1; ++i) {
			vscreen[i] = vp;
			++vp;
			pscreen[i] = vp;
			++vp;
		}
	}
	if (rowchanged || colchanged || first_run) {
		for (i = 0; i < 2 * (newrow - 1); i++) {
			TRYREALLOC(video[i].v_text, newcol * UTF8_MAX_BYTES);
			TRYREALLOC(video[i].v_wid, newcol);
			TRYREALLOC(video[i].v_hue, newcol);
		}
		TRYREALLOC(blanks.v_text, newcol * UTF8_MAX_BYTES);
		TRYREALLOC(blanks.v_wid, newcol);
		TRYREALLOC(blanks.v_hue, newcol);
	}

	nrow = newrow;
	ncol = newcol;

	if (ttrow > nrow)
		ttrow = nrow;
	if (ttcol > ncol)
		ttcol = ncol;

	first_run = 0;
	return (TRUE);
}

#undef TRYREALLOC
#undef TRYREALLOCARRAY

/*
 * Initialize the data structures used
 * by the display code. The edge vectors used
 * to access the screens are set up. The operating
 * system's terminal I/O channel is set up. Fill the
 * "blanks" array with ASCII blanks. The rest is done
 * at compile time. The original window is marked
 * as needing full update, and the physical screen
 * is marked as garbage, so all the right stuff happens
 * on the first call to redisplay.
 */
void
vtinit(void)
{
	int	i;

	ttopen();
	ttinit();

	/*
	 * ttinit called ttresize(), which called vtresize(), so our data
	 * structures are setup correctly.
	 */

	blanks.v_color = CTEXT;
	for (i = 0; i < ncol; ++i) {
		memset(VSLOT(&blanks, i), 0, UTF8_MAX_BYTES);
		VSLOT(&blanks, i)[0] = ' ';
		blanks.v_wid[i] = 1;
		blanks.v_hue[i] = SCOLOR_DEFAULT;
	}
}

/*
 * Tidy up the virtual display system
 * in anticipation of a return back to the host
 * operating system. Right now all we do is position
 * the cursor to the last line, erase the line, and
 * close the terminal channel.
 */
void
vttidy(void)
{
	ttcolor(CTEXT);
	ttnowindow();		/* No scroll window.	 */
	ttmove(nrow - 1, 0);	/* Echo line.		 */
	tteeol();
	tttidy();
	ttflush();
	ttclose();
}

/*
 * Move the virtual cursor to an origin
 * 0 spot on the virtual display screen. I could
 * store the column as a character pointer to the spot
 * on the line, which would make "vtputc" a little bit
 * more efficient. No checking for errors.
 */
void
vtmove(int row, int col)
{
	vtrow = row;
	vtcol = col;
}

/*
 * Write a single-byte character to the virtual display,
 * dealing with long lines and the display of unprintable
 * things like control characters. Also expand tabs every 8
 * columns. This code only puts printing characters into
 * the virtual display image. Special care must be taken when
 * expanding tabs. On a screen whose width is not a multiple
 * of 8, it is possible for the virtual cursor to hit the
 * right margin before the next tab stop is reached. This
 * makes the tab code loop if you are not careful.
 * Three guesses how we found this.
 */
void
vtputc(int c, struct mgwin *wp)
{
	struct video	*vp;
	int		 target;

	c &= 0xff;

	vp = vscreen[vtrow];
	if (vtcol >= vt_rightcol) {
		memset(VSLOT(vp, vt_rightcol - 1), 0, UTF8_MAX_BYTES);
		VSLOT(vp, vt_rightcol - 1)[0] = '$';
		vp->v_wid[vt_rightcol - 1] = 1;
		vp->v_hue[vt_rightcol - 1] = vt_hue;
	} else if (c == '\t') {
		target = ntabstop(vtcol, wp->w_bufp->b_tabw);
		do {
			vtputc(' ', wp);
		} while (vtcol < vt_rightcol && vtcol < target);
	} else if (ISCTRL(c)) {
		vtputc('^', wp);
		vtputc(CCHR(c), wp);
	} else if (isprint(c)) {
		memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
		VSLOT(vp, vtcol)[0] = c;
		vp->v_wid[vtcol] = 1;
		vp->v_hue[vtcol] = vt_hue;
		vtcol++;
	} else {
		char bf[5];

		snprintf(bf, sizeof(bf), "\\%o", c);
		vtputs(bf, wp);
	}
}

/*
 * Write one character (possibly multi-byte) from line lp at
 * byte offset *pj, advancing *pj past it.  Handles UTF-8
 * sequences with proper East Asian display width.
 */
void
vtputchar(struct line *lp, int *pj, struct mgwin *wp, const char *hue)
{
	struct video	*vp;
	int		 j, c, len, w, col;

	j = *pj;
	c = (unsigned char)lgetc(lp, j);
	vt_hue = hue ? hue[j] : 0;

	if (c < 0x80) {
		vtputc(c, wp);
		*pj = j + 1;
		return;
	}

	vp = vscreen[vtrow];
	w = utf8_width(&lp->l_text[j], llength(lp) - j, &len);
	if (w < 0) {
		/* invalid UTF-8 — render the raw byte */
		vtputc(c, wp);
		*pj = j + 1;
		return;
	}
	if (w == 0) {
		/* combining char — skip without advancing column */
		*pj = j + len;
		return;
	}
	if (vtcol + w > vt_rightcol) {
		/* overflow */
		memset(VSLOT(vp, vt_rightcol - 1), 0, UTF8_MAX_BYTES);
		VSLOT(vp, vt_rightcol - 1)[0] = '$';
		vp->v_wid[vt_rightcol - 1] = 1;
		vp->v_hue[vt_rightcol - 1] = vt_hue;
		*pj = j + len;
		return;
	}
	/* write the slot */
	memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
	memcpy(VSLOT(vp, vtcol), &lp->l_text[j], len);
	vp->v_wid[vtcol] = w;
	vp->v_hue[vtcol] = vt_hue;
	/* mark continuation columns */
	for (col = 1; col < w; col++) {
		memset(VSLOT(vp, vtcol + col), 0, UTF8_MAX_BYTES);
		vp->v_wid[vtcol + col] = 0;
		vp->v_hue[vtcol + col] = vt_hue;
	}
	vtcol += w;
	*pj = j + len;
}

/*
 * Put a character to the virtual screen in an extended line.  If we are not
 * yet on left edge, don't print it yet.  Check for overflow on the right
 * margin.
 */
void
vtpute(int c, struct mgwin *wp)
{
	struct video *vp;
	int target;

	c &= 0xff;

	vp = vscreen[vtrow];
	if (vtcol >= vt_rightcol) {
		memset(VSLOT(vp, vt_rightcol - 1), 0, UTF8_MAX_BYTES);
		VSLOT(vp, vt_rightcol - 1)[0] = '$';
		vp->v_wid[vt_rightcol - 1] = 1;
		vp->v_hue[vt_rightcol - 1] = vt_hue;
	} else if (c == '\t') {
		target = ntabstop(vtcol + lbound, wp->w_bufp->b_tabw);
		do {
			vtpute(' ', wp);
		} while (((vtcol + lbound) < target) && vtcol < vt_rightcol);
	} else if (ISCTRL(c) != FALSE) {
		vtpute('^', wp);
		vtpute(CCHR(c), wp);
	} else if (isprint(c)) {
		if (vtcol >= 0) {
			memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
			VSLOT(vp, vtcol)[0] = c;
			vp->v_wid[vtcol] = 1;
			vp->v_hue[vtcol] = vt_hue;
		}
		++vtcol;
	} else {
		char bf[5], *cp;

		snprintf(bf, sizeof(bf), "\\%o", c);
		for (cp = bf; *cp != '\0'; cp++)
			vtpute(*cp, wp);
	}
}

/*
 * Extended-line variant of vtputchar: writes one character
 * from lp at *pj, but only renders it once vtcol >= 0 (i.e.
 * past the left scroll bound).
 */
void
vtputechar(struct line *lp, int *pj, struct mgwin *wp, const char *hue)
{
	struct video *vp;
	int j, c, len, w, col;

	j = *pj;
	c = (unsigned char)lgetc(lp, j);
	vt_hue = hue ? hue[j] : 0;

	if (c < 0x80) {
		vtpute(c, wp);
		*pj = j + 1;
		return;
	}

	vp = vscreen[vtrow];
	w = utf8_width(&lp->l_text[j], llength(lp) - j, &len);
	if (w < 0) {
		vtpute(c, wp);
		*pj = j + 1;
		return;
	}
	if (w == 0) {
		*pj = j + len;
		return;
	}
	if (vtcol >= 0 && vtcol + w <= ncol) {
		memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
		memcpy(VSLOT(vp, vtcol), &lp->l_text[j], len);
		vp->v_wid[vtcol] = w;
		vp->v_hue[vtcol] = vt_hue;
		for (col = 1; col < w; col++) {
			memset(VSLOT(vp, vtcol + col), 0, UTF8_MAX_BYTES);
			vp->v_wid[vtcol + col] = 0;
			vp->v_hue[vtcol + col] = vt_hue;
		}
	}
	vtcol += w;
	*pj = j + len;
}

/*
 * Erase from the end of the software cursor to the end of the line on which
 * the software cursor is located. The display routines will decide if a
 * hardware erase to end of line command should be used to display this.
 */
void
vteeol(void)
{
	struct video *vp;

	vp = vscreen[vtrow];
	while (vtcol < vt_rightcol) {
		memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
		VSLOT(vp, vtcol)[0] = ' ';
		vp->v_wid[vtcol] = 1;
		vp->v_hue[vtcol] = SCOLOR_DEFAULT;
		vtcol++;
	}
}

/*
 * Make sure that the display is
 * right. This is a three part process. First,
 * scan through all of the windows looking for dirty
 * ones. Check the framing, and refresh the screen.
 * Second, make sure that "currow" and "curcol" are
 * correct for the current window. Third, make the
 * virtual and physical screens the same.
 */
void
update(int modelinecolor)
{
	struct line	*lp;
	struct mgwin	*wp;
	struct video	*vp1;
	struct video	*vp2;
	int	 c, i, j;
	int	 hflag;
	int	 currow, curcol;
	int	 offs, size;
	int	 line_num;		/* buffer line number of current screen line */

	compute_region();

	if (charswaiting())
		return;
	if (sgarbf) {		/* must update everything */
		wp = wheadp;
		while (wp != NULL) {
			wp->w_rflag |= WFMODE | WFFULL;
			wp = wp->w_wndp;
		}
	}
	/*
	 * When the mark is set, cursor movement changes the region,
	 * so force a full redraw to update the highlight.
	 */
	if (reg_active) {
		wp = wheadp;
		while (wp != NULL) {
			if (wp->w_rflag & WFMOVE)
				wp->w_rflag |= WFFULL;
			wp = wp->w_wndp;
		}
	}
	if (linenos || colnos) {
		wp = wheadp;
		while (wp != NULL) {
			wp->w_rflag |= WFMODE;
			wp = wp->w_wndp;
		}
	}
	hflag = FALSE;			/* Not hard. */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		/*
		 * Nothing to be done.
		 */
		if (wp->w_rflag == 0)
			continue;

		if ((wp->w_rflag & WFFRAME) == 0) {
			lp = wp->w_linep;
			for (i = 0; i < wp->w_ntrows; ++i) {
				if (lp == wp->w_dotp)
					goto out;
				if (lp == wp->w_bufp->b_headp)
					break;
				lp = lforw(lp);
			}
		}
		/*
		 * Put the middle-line in place.
		 */
		i = wp->w_frame;
		if (i > 0) {
			--i;
			if (i >= wp->w_ntrows)
				i = wp->w_ntrows - 1;
		} else if (i < 0) {
			i += wp->w_ntrows;
			if (i < 0)
				i = 0;
		} else
			i = wp->w_ntrows / 2; /* current center, no change */

		/*
		 * Find the line.
		 */
		lp = wp->w_dotp;
		while (i != 0 && lback(lp) != wp->w_bufp->b_headp) {
			--i;
			lp = lback(lp);
		}
		wp->w_linep = lp;
		wp->w_rflag |= WFFULL;	/* Force full.		 */
	out:
		lp = wp->w_linep;	/* Try reduced update.	 */
		i = wp->w_toprow;
		vt_rightcol = wp->w_leftcol + wp->w_ntcols;
		if ((wp->w_rflag & ~WFMODE) == WFEDIT) {
			while (lp != wp->w_dotp) {
				++i;
				lp = lforw(lp);
			}
			vscreen[i]->v_color = CTEXT;
			vscreen[i]->v_flag |= (VFCHG | VFHBAD);
			vtmove(i, wp->w_leftcol);
			ensure_hue_line(lp);
			syntax_match_line(lback(lp), lp, g_hue, ncol);
			apply_region_hue(lp, wp->w_dotline);
			for (j = 0; j < llength(lp); )
				vtputchar(lp, &j, wp, g_hue);
			vteeol();
		} else if ((wp->w_rflag & (WFEDIT | WFFULL)) != 0) {
			hflag = TRUE;
			/* compute buffer line number of first screen line */
			line_num = wp->w_dotline;
			{
				struct line *tlp = wp->w_linep;
				while (tlp != wp->w_dotp &&
				    tlp != wp->w_bufp->b_headp) {
					line_num--;
					tlp = lforw(tlp);
				}
			}
			while (i < wp->w_toprow + wp->w_ntrows) {
				vscreen[i]->v_color = CTEXT;
				vscreen[i]->v_flag |= (VFCHG | VFHBAD);
				vtmove(i, wp->w_leftcol);
				if (lp != wp->w_bufp->b_headp) {
					ensure_hue_line(lp);
					syntax_match_line(lback(lp), lp,
					    g_hue, ncol);
					apply_region_hue(lp, line_num);
					for (j = 0; j < llength(lp); )
						vtputchar(lp, &j, wp, g_hue);
				}
				vteeol();
				apply_region_eol(line_num);
				if (lp != wp->w_bufp->b_headp) {
					lp = lforw(lp);
					line_num++;
				}
				++i;
			}
		}
		if ((wp->w_rflag & WFMODE) != 0)
			modeline(wp, modelinecolor);
		wp->w_rflag = 0;
		wp->w_frame = 0;
	}
	/* Render vertical separators between horizontally split windows */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_wndp != NULL &&
		    wp->w_wndp->w_toprow == wp->w_toprow &&
		    wp->w_wndp->w_leftcol == wp->w_leftcol + wp->w_ntcols + 1) {
			int sepcol = wp->w_leftcol + wp->w_ntcols;
			int r;
			for (r = wp->w_toprow; r <= wp->w_toprow + wp->w_ntrows; r++) {
				struct video *vp = vscreen[r];
				memset(VSLOT(vp, sepcol), 0, UTF8_MAX_BYTES);
				VSLOT(vp, sepcol)[0] = '|';
				vp->v_wid[sepcol] = 1;
				vp->v_hue[sepcol] = 0;
				vp->v_flag |= VFCHG;
			}
		}
	}
	lp = curwp->w_linep;	/* Cursor location. */
	currow = curwp->w_toprow;
	while (lp != curwp->w_dotp) {
		++currow;
		lp = lforw(lp);
	}
	curcol = curwp->w_leftcol;
	i = 0;
	while (i < curwp->w_doto) {
		c = lgetc(lp, i);
		if (c == '\t') {
			curcol = curwp->w_leftcol + ntabstop(curcol - curwp->w_leftcol, curwp->w_bufp->b_tabw);
			i++;
		} else if ((unsigned char)c >= 0x80) {
			int len, w;
			w = utf8_width(&lp->l_text[i],
			    llength(lp) - i, &len);
			if (w < 0) {
				curcol += 4;	/* \ooo */
				i++;
			} else {
				curcol += w;
				i += len;
			}
		} else if (ISCTRL(c) != FALSE) {
			curcol += 2;
			i++;
		} else if (isprint(c)) {
			curcol++;
			i++;
		} else {
			char bf[5];
			snprintf(bf, sizeof(bf), "\\%o", c);
			curcol += strlen(bf);
			i++;
		}
	}
	if (curcol >= curwp->w_leftcol + curwp->w_ntcols - 1) {	/* extended line. */
		/* flag we are extended and changed */
		vscreen[currow]->v_flag |= VFEXT | VFCHG;
		updext(currow, curcol);	/* and output extended line */
	} else
		lbound = 0;	/* not extended line */

	/*
	 * Make sure no lines need to be de-extended because the cursor is no
	 * longer on them.
	 */
	wp = wheadp;
	while (wp != NULL) {
		lp = wp->w_linep;
		i = wp->w_toprow;
		while (i < wp->w_toprow + wp->w_ntrows) {
			if (vscreen[i]->v_flag & VFEXT) {
				/* always flag extended lines as changed */
				vscreen[i]->v_flag |= VFCHG;
				if ((wp != curwp) || (lp != wp->w_dotp) ||
				    (curcol < wp->w_leftcol + wp->w_ntcols - 1)) {
					vt_rightcol = wp->w_leftcol + wp->w_ntcols;
					vtmove(i, wp->w_leftcol);
					ensure_hue_line(lp);
					syntax_match_line(lback(lp), lp,
					    g_hue, ncol);
					for (j = 0; j < llength(lp); )
						vtputchar(lp, &j, wp, g_hue);
					vteeol();
					/* this line no longer is extended */
					vscreen[i]->v_flag &= ~VFEXT;
				}
			}
			lp = lforw(lp);
			++i;
		}
		/* if garbaged then fix up mode lines */
		if (sgarbf != FALSE)
			vscreen[i]->v_flag |= VFCHG;
		/* and onward to the next window */
		wp = wp->w_wndp;
	}

	if (sgarbf != FALSE) {	/* Screen is garbage.	 */
		sgarbf = FALSE;	/* Erase-page clears.	 */
		epresf = FALSE;	/* The message area.	 */
		tttop = HUGE;	/* Forget where you set. */
		ttbot = HUGE;	/* scroll region.	 */
		tthue = CNONE;	/* Color unknown.	 */
		ttcolor(CTEXT);	/* Reset terminal to default bg before clear */
		ttmove(0, 0);
		tteeop();
		for (i = 0; i < nrow - 1; ++i) {
			uline(i, vscreen[i], &blanks);
			ucopy(vscreen[i], pscreen[i]);
		}
		ttmove(currow, curcol - lbound);
		ttflush();
		return;
	}
	if (hflag != FALSE) {			/* Hard update?		*/
		for (i = 0; i < nrow - 1; ++i) {/* Compute hash data.	*/
			hash(vscreen[i]);
			hash(pscreen[i]);
		}
		offs = 0;			/* Get top match.	*/
		while (offs != nrow - 1) {
			vp1 = vscreen[offs];
			vp2 = pscreen[offs];
			if (vp1->v_color != vp2->v_color
			    || vp1->v_hash != vp2->v_hash)
				break;
			uline(offs, vp1, vp2);
			ucopy(vp1, vp2);
			++offs;
		}
		if (offs == nrow - 1) {		/* Might get it all.	*/
			ttmove(currow, curcol - lbound);
			ttflush();
			return;
		}
		size = nrow - 1;		/* Get bottom match.	*/
		while (size != offs) {
			vp1 = vscreen[size - 1];
			vp2 = pscreen[size - 1];
			if (vp1->v_color != vp2->v_color
			    || vp1->v_hash != vp2->v_hash)
				break;
			uline(size - 1, vp1, vp2);
			ucopy(vp1, vp2);
			--size;
		}
		if ((size -= offs) == 0)	/* Get screen size.	*/
			panic("Illegal screen size in update");
		setscores(offs, size);		/* Do hard update.	*/
		traceback(offs, size, size, size);
		for (i = 0; i < size; ++i)
			ucopy(vscreen[offs + i], pscreen[offs + i]);
		ttmove(currow, curcol - lbound);
		ttflush();
		return;
	}
	for (i = 0; i < nrow - 1; ++i) {	/* Easy update.		*/
		vp1 = vscreen[i];
		vp2 = pscreen[i];
		if ((vp1->v_flag & VFCHG) != 0) {
			uline(i, vp1, vp2);
			ucopy(vp1, vp2);
		}
	}
	ttmove(currow, curcol - lbound);
	ttflush();
}

/*
 * Update a saved copy of a line,
 * kept in a video structure. The "vvp" is
 * the one in the "vscreen". The "pvp" is the one
 * in the "pscreen". This is called to make the
 * virtual and physical screens the same when
 * display has done an update.
 */
void
ucopy(struct video *vvp, struct video *pvp)
{
	vvp->v_flag &= ~VFCHG;		/* Changes done.	 */
	pvp->v_flag = vvp->v_flag;	/* Update model.	 */
	pvp->v_hash = vvp->v_hash;
	pvp->v_cost = vvp->v_cost;
	pvp->v_color = vvp->v_color;
	bcopy(vvp->v_text, pvp->v_text, ncol * UTF8_MAX_BYTES);
	bcopy(vvp->v_wid, pvp->v_wid, ncol);
	bcopy(vvp->v_hue, pvp->v_hue, ncol);
}

/*
 * updext: update the extended line which the cursor is currently on at a
 * column greater than the terminal width. The line will be scrolled right or
 * left to let the user see where the cursor is.
 */
void
updext(int currow, int curcol)
{
	struct line	*lp;			/* pointer to current line */
	int	 j;			/* index into line */
	int	 wcols = curwp->w_ntcols;	/* window column width */

	if (wcols < 2)
		return;

	/*
	 * calculate what column the left bound should be
	 * (force cursor into middle half of screen)
	 */
	lbound = (curcol - curwp->w_leftcol) - ((curcol - curwp->w_leftcol) % (wcols >> 1)) - (wcols >> 2);

	/*
	 * scan through the line outputting characters to the virtual screen
	 * once we reach the left edge
	 */
	vt_rightcol = curwp->w_leftcol + curwp->w_ntcols;
	vtmove(currow, curwp->w_leftcol - lbound);	/* start scanning offscreen */
	lp = curwp->w_dotp;			/* line to output */
	ensure_hue_line(lp);
	syntax_match_line(lback(lp), lp, g_hue, ncol);
	for (j = 0; j < llength(lp); )		/* until the end-of-line */
		vtputechar(lp, &j, curwp, g_hue);
	vteeol();				/* truncate the virtual line */
	memset(VSLOT(vscreen[currow], 0), 0, UTF8_MAX_BYTES);
	VSLOT(vscreen[currow], 0)[0] = '$';	/* '$' in column 1 */
	vscreen[currow]->v_wid[0] = 1;
	vscreen[currow]->v_hue[0] = 0;
}

/*
 * Update a single line. This routine only
 * uses basic functionality (no insert and delete character,
 * but erase to end of line). The "vvp" points at the video
 * structure for the line on the virtual screen, and the "pvp"
 * is the same for the physical screen. Avoid erase to end of
 * line when updating CMODE color lines, because of the way that
 * reverse video works on most terminals.
 */
void
uline(int row, struct video *vvp, struct video *pvp)
{
	int	 col, first, last, erase_from, k, w;
	char	*vslot, *pslot;
	int	 nbflag;

	/*
	 * Helper: are two columns identical (bytes + width + hue)?
	 */
#define SAME(vp, pp, c) \
	((vp)->v_wid[(c)] == (pp)->v_wid[(c)] && \
	 (vp)->v_hue[(c)] == (pp)->v_hue[(c)] && \
	 memcmp(VSLOT(vp, c), VSLOT(pp, c), UTF8_MAX_BYTES) == 0)

	/*
	 * Helper: is column c a blank?
	 */
#define ISBLANK(vp, c) \
	((vp)->v_wid[(c)] == 1 && VSLOT(vp, c)[0] == ' ')

	if (vvp->v_color != pvp->v_color) {	/* Wrong color, do a	 */
		ttmove(row, 0);			/* full redraw.		 */
#ifdef	STANDOUT_GLITCH
		if (pvp->v_color != CTEXT && magic_cookie_glitch >= 0)
			tteeol();
#endif
		ttcolor(vvp->v_color);
#ifdef	STANDOUT_GLITCH
		first = magic_cookie_glitch > 0 ? magic_cookie_glitch : 0;
		last = ncol - (magic_cookie_glitch >= 0 ?
		    (magic_cookie_glitch != 0 ? magic_cookie_glitch : 1) : 0);
#else
		first = 0;
		last = ncol;
#endif
		if (vvp->v_color == CMODE) {
			/* mode line: single color, no per-column hue */
			for (col = first; col < last; ) {
				w = vvp->v_wid[col];
				if (w == 0) { col++; continue; }
				vslot = VSLOT(vvp, col);
				for (k = 0; k < UTF8_MAX_BYTES && vslot[k]; k++)
					ttputc(vslot[k]);
				ttcol += w;
				col += w;
			}
		} else {
			int curhue = -1;
			for (col = first; col < last; ) {
				if (vvp->v_hue[col] != curhue) {
					ttcolor(vvp->v_hue[col]);
					curhue = vvp->v_hue[col];
				}
				w = vvp->v_wid[col];
				if (w == 0) { col++; continue; }
				vslot = VSLOT(vvp, col);
				for (k = 0; k < UTF8_MAX_BYTES && vslot[k]; k++)
					ttputc(vslot[k]);
				ttcol += w;
				col += w;
			}
		}
		ttcolor(CTEXT);
		return;
	}
	/* Compute left match. */
	first = 0;
	while (first < ncol && SAME(vvp, pvp, first))
		first++;
	if (first >= ncol)		/* All equal.		 */
		return;
	/* Compute right match. */
	nbflag = FALSE;
	last = ncol - 1;
	while (last > first && SAME(vvp, pvp, last)) {
		if (!ISBLANK(vvp, last))
			nbflag = TRUE;
		last--;
	}
	/* `last` is now the last differing column (inclusive). */
	erase_from = last + 1;
	if (nbflag == FALSE && vvp->v_color == CTEXT) {
		int s = last + 1;
		while (s > first && ISBLANK(vvp, s - 1))
			s--;
		if ((last + 1 - s) <= tceeol)
			s = last + 1;	/* erase not cheaper */
		erase_from = s;
	}
	ttmove(row, first);
#ifdef	STANDOUT_GLITCH
	if (vvp->v_color != CTEXT && magic_cookie_glitch > 0) {
		if (first < magic_cookie_glitch)
			first = magic_cookie_glitch;
		if (erase_from > ncol - magic_cookie_glitch)
			erase_from = ncol - magic_cookie_glitch;
	} else if (magic_cookie_glitch < 0)
#endif
		ttcolor(vvp->v_color);
	if (vvp->v_color == CMODE) {
		/* mode line: single color, no per-column hue */
		for (col = first; col < erase_from; ) {
			w = vvp->v_wid[col];
			if (w == 0) { col++; continue; }
			vslot = VSLOT(vvp, col);
			for (k = 0; k < UTF8_MAX_BYTES && vslot[k]; k++)
				ttputc(vslot[k]);
			ttcol += w;
			col += w;
		}
	} else {
		int curhue = -1;
		for (col = first; col < erase_from; ) {
			if (vvp->v_hue[col] != curhue) {
				ttcolor(vvp->v_hue[col]);
				curhue = vvp->v_hue[col];
			}
			w = vvp->v_wid[col];
			if (w == 0) { col++; continue; }
			vslot = VSLOT(vvp, col);
			for (k = 0; k < UTF8_MAX_BYTES && vslot[k]; k++)
				ttputc(vslot[k]);
			ttcol += w;
			col += w;
		}
	}
	if (erase_from <= last) {		/* Do erase.		 */
		/* Use the hue of the last rendered column for the erase. */
		if (vvp->v_color != CMODE && erase_from > 0 && erase_from < ncol)
			ttcolor(vvp->v_hue[erase_from]);
		tteeol();
	}

#undef SAME
#undef ISBLANK
}

/*
 * Redisplay the mode line for the window pointed to by the "wp".
 * This is the only routine that has any idea of how the mode line is
 * formatted. You can change the modeline format by hacking at this
 * routine. Called by "update" any time there is a dirty window.  Note
 * that if STANDOUT_GLITCH is defined, first and last magic_cookie_glitch
 * characters may never be seen.
 */
void
modeline(struct mgwin *wp, int modelinecolor)
{
	int	n, md;
	struct buffer *bp;
	char sl[32];
	int len;

	n = wp->w_toprow + wp->w_ntrows;	/* Location.		 */
	vscreen[n]->v_color = modelinecolor;	/* Mode line color.	 */
	vscreen[n]->v_flag |= (VFCHG | VFHBAD);	/* Recompute, display.	 */
	vt_rightcol = wp->w_leftcol + wp->w_ntcols;
	vtmove(n, wp->w_leftcol);		/* Seek to right line.	 */
	vt_hue = 0;				/* modeline has no syntax hue */
	bp = wp->w_bufp;
	/* Status: -- clean, ** modified, %% readonly, %* readonly+modified */
	vtputc('-', wp);
	if ((bp->b_flag & BFREADONLY) != 0) {
		vtputc('%', wp);
		if ((bp->b_flag & BFCHG) != 0)
			vtputc('*', wp);
		else
			vtputc('%', wp);
	} else if ((bp->b_flag & BFCHG) != 0) {
		vtputc('*', wp);
		vtputc('*', wp);
	} else {
		vtputc('-', wp);
		vtputc('-', wp);
	}
	n = 3;
	/* Buffer name (no fixed padding) */
	n += vtputs("Me:", wp);
	vtputc(' ', wp);
	++n;
	if (bp->b_bname[0] != '\0')
		n += vtputs(&(bp->b_bname[0]), wp);
	vtputc(' ', wp);
	++n;
	/* Mode list */
	vtputc('(', wp);
	++n;
	for (md = 0; ; ) {
		n += vtputs(bp->b_modes[md]->p_name, wp);
		if (++md > bp->b_nmodes)
			break;
		vtputc('-', wp);
		++n;
	}
	if (macrodef == TRUE)
		n += vtputs("-def", wp);
	if (globalwd == TRUE)
		n += vtputs("-gwd", wp);
	vtputc(')', wp);
	++n;
	/* Position: L行:C列 */
	if (linenos && colnos)
		len = snprintf(sl, sizeof(sl), " L%d:C%d", wp->w_dotline,
		    getcolpos(wp));
	else if (linenos)
		len = snprintf(sl, sizeof(sl), " L%d", wp->w_dotline);
	else if (colnos)
		len = snprintf(sl, sizeof(sl), " C%d", getcolpos(wp));
	else
		len = 0;
	if (len > 0 && len < (int)sizeof(sl))
		n += vtputs(sl, wp);

	/* Position percentage, right-aligned. */
	{
		int total = 0;
		struct line *lp;
		for (lp = lforw(bp->b_headp); lp != bp->b_headp; lp = lforw(lp))
			total++;
		int pct;
		if (total == 0)
			pct = 100;
		else if (wp->w_dotline >= total)
			pct = 100;
		else
			pct = (wp->w_dotline * 100) / total;
		char pctbuf[8];
		len = snprintf(pctbuf, sizeof(pctbuf), "%3d%%", pct);
		/* pad to right edge minus percentage width */
		while (n < vt_rightcol - len) {
			vtputc('-', wp);
			++n;
		}
		n += vtputs(pctbuf, wp);
	}

	while (n < vt_rightcol) {		/* Pad out.		 */
		vtputc('-', wp);
		++n;
	}
}

/*
 * Output a string to the mode line, report how many screen columns
 * it consumed.  Handles UTF-8 multi-byte characters properly.
 */
int
vtputs(const char *s, struct mgwin *wp)
{
	int n = 0;
	struct video *vp;
	int c, len, w;

	while (*s != '\0') {
		c = (unsigned char)*s;

		/* ASCII printable or control: use vtputc */
		if (c < 0x80) {
			vtputc(*s++, wp);
			n++;
			continue;
		}

		/* UTF-8 multi-byte sequence */
		if (c >= 0xC0) {
			/* determine sequence length */
			if ((c & 0xE0) == 0xC0) len = 2;
			else if ((c & 0xF0) == 0xE0) len = 3;
			else if ((c & 0xF8) == 0xF0) len = 4;
			else len = 1;

			/* copy bytes into video slot */
			vp = vscreen[vtrow];
			if (vtcol < vt_rightcol) {
				int k;
				memset(VSLOT(vp, vtcol), 0, UTF8_MAX_BYTES);
				for (k = 0; k < len && *s; k++)
					VSLOT(vp, vtcol)[k] = *s++;
				/* East Asian width: CJK = 2, others = 1 */
				w = (len >= 3) ? 2 : 1;
				vp->v_wid[vtcol] = w;
				vp->v_hue[vtcol] = vt_hue;
				n += w;
				vtcol += w;
			} else {
				s += len;  /* skip if past edge */
			}
		} else {
			/* continuation byte without start — skip */
			s++;
		}
	}
	return (n);
}

/*
 * Compute the hash code for the line pointed to by the "vp".
 * Recompute it if necessary. Also set the approximate redisplay
 * cost. The validity of the hash code is marked by a flag bit.
 * The cost understand the advantages of erase to end of line.
 * Tuned for the VAX by Bob McNamara; better than it used to be on
 * just about any machine.
 */
void
hash(struct video *vp)
{
	int	i, n, k, last;
	char   *s;

	if ((vp->v_flag & VFHBAD) != 0) {	/* Hash bad.		 */
		/* Find last non-blank column. */
		last = ncol - 1;
		while (last >= 0) {
			s = VSLOT(vp, last);
			if (vp->v_wid[last] != 1 || s[0] != ' ')
				break;
			last--;
		}
		n = ncol - 1 - last;		/* Erase cheaper?	 */
		if (n > tceeol)
			n = tceeol;
		vp->v_cost = (last + 1) + n;	/* Columns + blanks.	 */
		for (n = 0, i = 0; i <= last; i++) {
			s = VSLOT(vp, i);
			for (k = 0; k < UTF8_MAX_BYTES; k++)
				n = (n << 5) + n + s[k];
			n = (n << 5) + n + vp->v_wid[i];
			n = (n << 5) + n + vp->v_hue[i];
		}
		vp->v_hash = n;			/* Hash code.		 */
		vp->v_flag &= ~VFHBAD;		/* Flag as all done.	 */
	}
}

/*
 * Compute the Insert-Delete
 * cost matrix. The dynamic programming algorithm
 * described by James Gosling is used. This code assumes
 * that the line above the echo line is the last line involved
 * in the scroll region. This is easy to arrange on the VT100
 * because of the scrolling region. The "offs" is the origin 0
 * offset of the first row in the virtual/physical screen that
 * is being updated; the "size" is the length of the chunk of
 * screen being updated. For a full screen update, use offs=0
 * and size=nrow-1.
 *
 * Older versions of this code implemented the score matrix by
 * a two dimensional array of SCORE nodes. This put all kinds of
 * multiply instructions in the code! This version is written to
 * use a linear array and pointers, and contains no multiplication
 * at all. The code has been carefully looked at on the VAX, with
 * only marginal checking on other machines for efficiency. In
 * fact, this has been tuned twice! Bob McNamara tuned it even
 * more for the VAX, which is a big issue for him because of
 * the 66 line X displays.
 *
 * On some machines, replacing the "for (i=1; i<=size; ++i)" with
 * i = 1; do { } while (++i <=size)" will make the code quite a
 * bit better; but it looks ugly.
 */
void
setscores(int offs, int size)
{
	struct score	 *sp;
	struct score	 *sp1;
	struct video	**vp, **pp;
	struct video	**vbase, **pbase;
	int	  tempcost;
	int	  bestcost;
	int	  j, i;

	vbase = &vscreen[offs - 1];	/* By hand CSE's.	 */
	pbase = &pscreen[offs - 1];
	score[0].s_itrace = 0;		/* [0, 0]		 */
	score[0].s_jtrace = 0;
	score[0].s_cost = 0;
	sp = &score[1];			/* Row 0, inserts.	 */
	tempcost = 0;
	vp = &vbase[1];
	for (j = 1; j <= size; ++j) {
		sp->s_itrace = 0;
		sp->s_jtrace = j - 1;
		tempcost += tcinsl;
		tempcost += (*vp)->v_cost;
		sp->s_cost = tempcost;
		++vp;
		++sp;
	}
	sp = &score[nrow];		/* Column 0, deletes.	 */
	tempcost = 0;
	for (i = 1; i <= size; ++i) {
		sp->s_itrace = i - 1;
		sp->s_jtrace = 0;
		tempcost += tcdell;
		sp->s_cost = tempcost;
		sp += nrow;
	}
	sp1 = &score[nrow + 1];		/* [1, 1].		 */
	pp = &pbase[1];
	for (i = 1; i <= size; ++i) {
		sp = sp1;
		vp = &vbase[1];
		for (j = 1; j <= size; ++j) {
			sp->s_itrace = i - 1;
			sp->s_jtrace = j;
			bestcost = (sp - nrow)->s_cost;
			if (j != size)	/* Cd(A[i])=0 @ Dis.	 */
				bestcost += tcdell;
			tempcost = (sp - 1)->s_cost;
			tempcost += (*vp)->v_cost;
			if (i != size)	/* Ci(B[j])=0 @ Dsj.	 */
				tempcost += tcinsl;
			if (tempcost < bestcost) {
				sp->s_itrace = i;
				sp->s_jtrace = j - 1;
				bestcost = tempcost;
			}
			tempcost = (sp - nrow - 1)->s_cost;
			if ((*pp)->v_color != (*vp)->v_color
			    || (*pp)->v_hash != (*vp)->v_hash)
				tempcost += (*vp)->v_cost;
			if (tempcost < bestcost) {
				sp->s_itrace = i - 1;
				sp->s_jtrace = j - 1;
				bestcost = tempcost;
			}
			sp->s_cost = bestcost;
			++sp;		/* Next column.		 */
			++vp;
		}
		++pp;
		sp1 += nrow;		/* Next row.		 */
	}
}

/*
 * Trace back through the dynamic programming cost
 * matrix, and update the screen using an optimal sequence
 * of redraws, insert lines, and delete lines. The "offs" is
 * the origin 0 offset of the chunk of the screen we are about to
 * update. The "i" and "j" are always started in the lower right
 * corner of the matrix, and imply the size of the screen.
 * A full screen traceback is called with offs=0 and i=j=nrow-1.
 * There is some do-it-yourself double subscripting here,
 * which is acceptable because this routine is much less compute
 * intensive then the code that builds the score matrix!
 */
void
traceback(int offs, int size, int i, int j)
{
	int	itrace, jtrace;
	int	k;
	int	ninsl, ndraw, ndell;

	if (i == 0 && j == 0)	/* End of update.	 */
		return;
	itrace = score[(nrow * i) + j].s_itrace;
	jtrace = score[(nrow * i) + j].s_jtrace;
	if (itrace == i) {	/* [i, j-1]		 */
		ninsl = 0;	/* Collect inserts.	 */
		if (i != size)
			ninsl = 1;
		ndraw = 1;
		while (itrace != 0 || jtrace != 0) {
			if (score[(nrow * itrace) + jtrace].s_itrace != itrace)
				break;
			jtrace = score[(nrow * itrace) + jtrace].s_jtrace;
			if (i != size)
				++ninsl;
			++ndraw;
		}
		traceback(offs, size, itrace, jtrace);
		if (ninsl != 0) {
			ttcolor(CTEXT);
			ttinsl(offs + j - ninsl, offs + size - 1, ninsl);
		}
		do {		/* B[j], A[j] blank.	 */
			k = offs + j - ndraw;
			uline(k, vscreen[k], &blanks);
		} while (--ndraw);
		return;
	}
	if (jtrace == j) {	/* [i-1, j]		 */
		ndell = 0;	/* Collect deletes.	 */
		if (j != size)
			ndell = 1;
		while (itrace != 0 || jtrace != 0) {
			if (score[(nrow * itrace) + jtrace].s_jtrace != jtrace)
				break;
			itrace = score[(nrow * itrace) + jtrace].s_itrace;
			if (j != size)
				++ndell;
		}
		if (ndell != 0) {
			ttcolor(CTEXT);
			ttdell(offs + i - ndell, offs + size - 1, ndell);
		}
		traceback(offs, size, itrace, jtrace);
		return;
	}
	traceback(offs, size, itrace, jtrace);
	k = offs + j - 1;
	uline(k, vscreen[k], pscreen[offs + i - 1]);
}
