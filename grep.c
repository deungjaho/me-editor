/*	$OpenBSD: grep.c,v 1.50 2023/03/08 04:43:11 guenther Exp $	*/

/* This file is in the public domain */

#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "def.h"
#include "kbd.h"
#include "funmap.h"
#include "syntax.h"
#include "utf8.h"

int	 globalwd = FALSE;
struct smatch;
static int	 compile_goto_error(int, int);
static int	 find_project_root(char *, size_t);
static int	 shellquote(char *, size_t, const char *);
static int	 collect_matches(const char *, const char *,
    struct smatch **);
static int	 match_menu(struct smatch *, int, const char *,
    const char *);
static void	 preview_show(const char *, struct smatch *,
    const char *);
int		 global_search(int, int);
int		 next_error(int, int);
static int	 grep(int, int);
static int	 gid(int, int);
static struct buffer	*compile_mode(const char *, const char *);
void grep_init(void);

static char compile_last_command[NFILEN] = "make ";

/*
 * Hints for next-error
 *
 * XXX - need some kind of callback to find out when those get killed.
 */
struct mgwin	*compile_win;
struct buffer	*compile_buffer;

/*
 * State the candidate list shares with the display code: menu_wp/
 * menu_stat mark the mode line carrying the "sel/total" count,
 * menu_hitline the previewed match line to highlight.
 */
struct mgwin		*menu_wp;
struct line		*menu_hitline;
char			 menu_stat[16];

static PF compile_pf[] = {
	compile_goto_error
};

static struct KEYMAPE (1) compilemap = {
	1,
	1,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), compile_pf, NULL }
	}
};

void
grep_init(void)
{
	funmap_add(compile_goto_error, "compile-goto-error", 0);
	funmap_add(next_error, "next-error", 0);
	funmap_add(grep, "grep", 1);
	funmap_add(global_search, "global-search", 1);
	funmap_add(compile, "compile", 0);
	funmap_add(gid, "gid", 1);
	maps_add((KEYMAP *)&compilemap, "compile");
}

static int
grep(int f, int n)
{
	char	 cprompt[NFILEN], *bufp;
	struct buffer	*bp;
	struct mgwin	*wp;

	(void)strlcpy(cprompt, "grep -n ", sizeof(cprompt));
	if ((bufp = eread("Run grep: ", cprompt, NFILEN,
	    EFDEF | EFNEW | EFCR)) == NULL)
		return (ABORT);
	else if (bufp[0] == '\0')
		return (FALSE);
	if (strlcat(cprompt, " /dev/null", sizeof(cprompt)) >= sizeof(cprompt))
		return (FALSE);

	if ((bp = compile_mode("*grep*", cprompt)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	compile_win = curwp = wp;
	return (TRUE);
}

int
compile(int f, int n)
{
	char	 cprompt[NFILEN], *bufp;
	struct buffer	*bp;
	struct mgwin	*wp;

	(void)strlcpy(cprompt, compile_last_command, sizeof(cprompt));
	if ((bufp = eread("Compile command: ", cprompt, NFILEN,
	    EFDEF | EFNEW | EFCR)) == NULL)
		return (ABORT);
	else if (bufp[0] == '\0')
		return (FALSE);
	if (savebuffers(f, n) == ABORT)
		return (ABORT);
	(void)strlcpy(compile_last_command, bufp, sizeof(compile_last_command));

	if ((bp = compile_mode("*compile*", cprompt)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	compile_win = curwp = wp;
	gotoline(FFARG, 0);
	return (TRUE);
}

/* id-utils foo. */
static int
gid(int f, int n)
{
	char	 command[NFILEN];
	char	 cprompt[NFILEN], *bufp;
	int	c;
	struct buffer	*bp;
	struct mgwin	*wp;
	int	 i, j, len;

	/* catch ([^\s(){}]+)[\s(){}]* */

	i = curwp->w_doto;
	/* Skip backwards over delimiters we are currently on */
	while (i > 0) {
		c = lgetc(curwp->w_dotp, i);
		if (isalnum(c) || c == '_')
			break;

		i--;
	}

	/* Skip the symbol itself */
	for (; i > 0; i--) {
		c = lgetc(curwp->w_dotp, i - 1);
		if (!isalnum(c) && c != '_')
			break;
	}
	/* Fill the symbol in cprompt[] */
	for (j = 0; j < sizeof(cprompt) - 1 && i < llength(curwp->w_dotp);
	    j++, i++) {
		c = lgetc(curwp->w_dotp, i);
		if (!isalnum(c) && c != '_')
			break;
		cprompt[j] = c;
	}
	cprompt[j] = '\0';

	if ((bufp = eread("Run gid (with args): ", cprompt, NFILEN,
	    (j ? EFDEF : 0) | EFNEW | EFCR)) == NULL)
		return (ABORT);
	else if (bufp[0] == '\0')
		return (FALSE);
	len = snprintf(command, sizeof(command), "gid %s", cprompt);
	if (len < 0 || len >= sizeof(command))
		return (FALSE);

	if ((bp = compile_mode("*gid*", command)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	compile_win = curwp = wp;
	return (TRUE);
}

/*
 * Prompt for a pattern and search for it in all files under the project
 * root: the nearest ancestor directory containing a .git entry, or the
 * buffer's working directory when there is none.  Matches are presented
 * as a selectable list above the echo area, consult-ripgrep style.
 */
int
global_search(int f, int n)
{
	char	 patbuf[NPAT], *bufp;
	char	 dir[NFILEN], qpat[NPAT * 4 + 8];
	char	 cmd[NFILEN];
	const char *gflags = "-rIn";
	struct smatch	*ms;
	int	 i, len, nm;

	patbuf[0] = '\0';
	if ((bufp = eread("Search in project: ", patbuf, sizeof(patbuf),
	    EFNEW | EFCR)) == NULL)
		return (ABORT);
	else if (bufp[0] == '\0')
		return (FALSE);

	if (find_project_root(dir, sizeof(dir)) != TRUE)
		return (FALSE);

	/* grep has no smart-case; keep the editor's search semantics */
	for (i = 0; bufp[i] != '\0'; i++)
		if (ISUPPER((unsigned char)bufp[i])) {
			gflags = "-rn";
			break;
		}
	if (shellquote(qpat, sizeof(qpat), bufp) != TRUE)
		return (FALSE);
	len = snprintf(cmd, sizeof(cmd),
	    "if command -v rg >/dev/null 2>&1; then"
	    " rg -S -n -- %s .;"
	    " else grep %s --exclude-dir=.git --exclude-dir=.svn"
	    " --exclude-dir=CVS -- %s .; fi",
	    qpat, gflags, qpat);
	if (len < 0 || len >= sizeof(cmd))
		return (FALSE);

	nm = collect_matches(dir, cmd, &ms);
	if (nm <= 0) {
		ewprintf("No matches for '%s'", bufp);
		return (TRUE);
	}
	return (match_menu(ms, nm, dir, bufp));
}

/*
 * One project-search hit: a file (relative to the search root, without a
 * leading "./"), a 1-based line number and the matched line's text.
 */
struct smatch {
	char	 file[NFILEN];
	int	 lineno;
	char	*text;
};

#define SLIST_MAX	10	/* candidate rows drawn above the echo line */
#define SMATCH_MAX	2048	/* results kept at most			*/
#define SHIT_MAX	32	/* pattern hits highlighted per line	*/

/* File, then line order: keeps every file's matches contiguous. */
static int
smatch_cmp(const void *a, const void *b)
{
	const struct smatch	*x = a, *y = b;
	int			 c = strcmp(x->file, y->file);

	return (c != 0 ? c : x->lineno - y->lineno);
}

/* Run cmd in dir; parse its "file:line:text" output into *out. */
static int
collect_matches(const char *dir, const char *cmd, struct smatch **out)
{
	struct smatch	*m = NULL;
	FILE	*fp;
	char	*buf = NULL, *p1, *p2;
	size_t	 sz = 0;
	ssize_t	 len;
	int	 nm = 0, cap = 0, ret = -1;
	char	 cwd[NFILEN], qcmd[NFILEN];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (-1);
	if (snprintf(qcmd, sizeof(qcmd), "%s 2>/dev/null", cmd) >=
	    (int)sizeof(qcmd))
		return (-1);
	if (chdir(dir) == -1 || (fp = popen(qcmd, "r")) == NULL)
		return (-1);
	while ((len = getline(&buf, &sz, fp)) != -1) {
		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		if ((p1 = strchr(buf, ':')) == NULL)
			continue;
		*p1++ = '\0';
		if ((p2 = strchr(p1, ':')) == NULL)
			continue;
		*p2++ = '\0';
		if (nm < SMATCH_MAX && nm == cap) {
			struct smatch *m2;

			cap = cap ? cap * 2 : 64;
			if ((m2 = realloc(m, cap * sizeof(*m))) == NULL)
				goto out;
			m = m2;
		}
		if (nm == SMATCH_MAX)
			continue;
		(void)strlcpy(m[nm].file,
		    buf[0] == '.' && buf[1] == '/' ? buf + 2 : buf,
		    sizeof(m[nm].file));
		m[nm].lineno = atoi(p1);
		m[nm].text = strdup(p2);
		if (m[nm].text == NULL)
			goto out;
		nm++;
	}
	if (nm > 1)
		qsort(m, nm, sizeof(*m), smatch_cmp);
	ret = nm;
out:
	free(buf);
	(void)pclose(fp);
	(void)chdir(cwd);
	if (ret < 0 && m != NULL) {
		while (nm > 0)
			free(m[--nm].text);
		free(m);
		return (-1);
	}
	*out = m;
	return (ret);
}

/* Byte ranges of pattern occurrences in s, smart-case like the search. */
static int
pat_ranges(const char *s, const char *pat, int (*ranges)[2], int maxr)
{
	int	 plen = strlen(pat), tlen = strlen(s);
	int	 i, j, nr = 0, xcase = 0;

	if (plen == 0)
		return (0);
	for (i = 0; i < plen; i++)
		if (ISUPPER((unsigned char)pat[i])) {
			xcase = 1;
			break;
		}
	for (i = 0; i + plen <= tlen && nr < maxr; i++) {
		for (j = 0; j < plen; j++) {
			int bc = CHARMASK(s[i + j]);
			int pc = CHARMASK(pat[j]);
			if (xcase ? bc != pc : bc != pc &&
			    (!ISUPPER(bc) || TOLOWER(bc) != pc))
				break;
		}
		if (j == plen) {
			ranges[nr][0] = i;
			ranges[nr][1] = i + plen;
			nr++;
			i += plen - 1;
		}
	}
	return (nr);
}

/* Print at most n bytes, clipping on UTF-8 char and column bounds. */
static void
menu_puts(const char *s, int n, int *cols)
{
	int	 w, len;

	while (n > 0) {
		if ((w = utf8_width(s, n, &len)) <= 0 || len <= 0)
			w = len = 1;
		if (*cols + w >= ncol - 1)
			break;
		while (len-- > 0) {
			ttputc((unsigned char)*s++);
			n--;
		}
		*cols += w;
	}
}

/* One candidate row: "> N: text" when selected, hits in rose. */
static void
menu_draw_match(struct smatch *ms, const char *pat, int sel)
{
	int	 ranges[SHIT_MAX][2];
	int	 k, nr, pos, cols = 0;
	char	 numbuf[24];
	const char *t;

	nr = pat_ranges(ms->text, pat, ranges, SHIT_MAX);
	if (sel)
		ttcolor(SCOLOR_CONTROL);
	else
		ttcolor(SCOLOR_DEFAULT);
	ttputc(sel ? '>' : ' ');
	cols++;
	snprintf(numbuf, sizeof(numbuf), " %d", ms->lineno);
	ttcolor(SCOLOR_TYPE);
	for (t = numbuf; *t != '\0' && cols < ncol - 1; t++, cols++)
		ttputc(*t);
	ttcolor(SCOLOR_DEFAULT);
	menu_puts(": ", 2, &cols);
	pos = 0;
	for (k = 0; k < nr; k++) {
		ttcolor(SCOLOR_DEFAULT);
		menu_puts(ms->text + pos, ranges[k][0] - pos, &cols);
		ttcolor(SCOLOR_CONTROL);
		menu_puts(ms->text + ranges[k][0],
		    ranges[k][1] - ranges[k][0], &cols);
		pos = ranges[k][1];
	}
	ttcolor(SCOLOR_DEFAULT);
	menu_puts(ms->text + pos, strlen(ms->text + pos), &cols);
	tteeol();
}

/* Park the terminal cursor on the previewed hit line at the match. */
static void
menu_cursor(void)
{
	struct line	*lp;
	int	 r = curwp->w_toprow, c = curwp->w_leftcol;
	int	 i, w, len, n = 0, tw = curwp->w_bufp->b_tabw;

	/* bounded: a stale w_linep can never join the dot's ring */
	for (lp = curwp->w_linep; lp != curwp->w_dotp &&
	    lp != curwp->w_bufp->b_headp && n < curwp->w_ntrows;
	    lp = lforw(lp), n++)
		r++;
	for (i = 0; i < curwp->w_doto && i < llength(curwp->w_dotp); ) {
		if (curwp->w_dotp->l_text[i] == '\t') {
			w = tw - (c - curwp->w_leftcol) % tw;
			len = 1;
		} else if ((w = utf8_width(&curwp->w_dotp->l_text[i],
		    llength(curwp->w_dotp) - i, &len)) <= 0 || len <= 0)
			w = len = 1;
		c += w;
		i += len;
	}
	c -= curline_lbound;
	if (c < curwp->w_leftcol)
		c = curwp->w_leftcol;
	if (c >= curwp->w_leftcol + curwp->w_ntcols)
		c = curwp->w_leftcol + curwp->w_ntcols - 1;
	ttmove(r, c < ncol ? c : ncol - 1);
}

/* Redraw the visible slice of the candidate list. */
static void
menu_draw(struct smatch *ms, int nm, int *mrow, int sel, int *offp, int v,
    const char *pat)
{
	int	 top = nrow - 1 - v, off = *offp;
	int	 i, r = 0, drawn = 0, cols;

	if (mrow[sel] < off)
		off = mrow[sel];
	if (mrow[sel] >= off + v)
		off = mrow[sel] - v + 1;
	*offp = off;
	for (i = 0; i < nm && drawn < v; i++) {
		if (i == 0 || strcmp(ms[i].file, ms[i - 1].file) != 0) {
			if (r >= off) {
				ttmove(top + drawn, 0);
				ttcolor(SCOLOR_COMMENT);
				cols = 0;
				menu_puts(ms[i].file, strlen(ms[i].file),
				    &cols);
				tteeol();
				drawn++;
			}
			r++;
		}
		if (r >= off) {
			ttmove(top + drawn, 0);
			menu_draw_match(&ms[i], pat, i == sel);
			drawn++;
		}
		r++;
	}
	while (drawn < v) {
		ttmove(top + drawn, 0);
		tteeol();
		drawn++;
	}
	menu_cursor();
	ttflush();
}

/* The window whose mode line sits lowest on the screen. */
static struct mgwin *
bottom_window(void)
{
	struct mgwin *wp, *best = NULL;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (best == NULL || wp->w_toprow + wp->w_ntrows >=
		    best->w_toprow + best->w_ntrows)
			best = wp;
	return (best);
}

/*
 * Candidate-picker loop: the bottom window gives the list v rows, so its
 * mode line is pushed up and the file preview lives right above it.
 * C-n/C-p/arrows move, RET visits the match, C-g/q/ESC cancels.
 */
static int
match_menu(struct smatch *ms, int nm, const char *dir, const char *pat)
{
	struct mgwin	*bwp;
	struct buffer	*bp, *pold;
	int	 sel = 0, off = 0, i, c, r = 0, v = SLIST_MAX;
	int	 accept = FALSE, lastsel = -1;
	int	*mrow;

	if (v > nrow - 2)
		v = nrow - 2;
	mrow = malloc(nm * sizeof(*mrow));
	if (mrow == NULL) {
		for (i = 0; i < nm; i++)
			free(ms[i].text);
		free(ms);
		return (FALSE);
	}
	for (i = 0; i < nm; i++) {
		if (i == 0 || strcmp(ms[i].file, ms[i - 1].file) != 0)
			r++;
		mrow[i] = r++;
	}
	if (v > r)
		v = r;
	pold = curwp->w_bufp;
	preview_show(dir, &ms[0], pat);
	bwp = bottom_window();
	if (v > bwp->w_ntrows - 1)
		v = bwp->w_ntrows - 1;
	if (v < 1)
		v = 1;
	bwp->w_ntrows -= v;
	bwp->w_rflag |= WFFULL;
	menu_wp = bwp;
	(void)update(CMODE);
	while (!accept) {
		if (sel != lastsel) {
			preview_show(dir, &ms[sel], pat);
			(void)snprintf(menu_stat, sizeof(menu_stat),
			    "%d/%d", sel + 1, nm);
			bwp->w_rflag |= WFFULL;
			(void)update(CMODE);
			eerase();
			menu_draw(ms, nm, mrow, sel, &off, v, pat);
			lastsel = sel;
		}
		c = getkey(FALSE);
		switch (c) {
		case CCHR('N'):
		case 'j':
			if (sel + 1 < nm)
				sel++;
			break;
		case CCHR('P'):
		case 'k':
			if (sel > 0)
				sel--;
			break;
		case CCHR('M'):
		case CCHR('J'):
			accept = TRUE;
			break;
		case CCHR('A'):		/* preview cursor: line start */
			(void)gotobol(FFRAND, 1);
			(void)update(CMODE);
			break;
		case CCHR('E'):		/* preview cursor: line end */
			(void)gotoeol(FFRAND, 1);
			(void)update(CMODE);
			break;
		case CCHR('F'):		/* preview cursor: forward char */
			(void)forwchar(FFRAND, 1);
			(void)update(CMODE);
			break;
		case CCHR('B'):		/* preview cursor: backward char */
			(void)backchar(FFRAND, 1);
			(void)update(CMODE);
			break;
		case CCHR('G'):
		case 'q':
			goto done;
		case 0x1b:		/* arrow keys or lone ESC */
			if (!charswaiting())
				goto done;
			c = getkey(FALSE);
			if (c != '[' && c != 'O')
				goto done;
			c = getkey(FALSE);
			if (c == 'B' && sel + 1 < nm)
				sel++;
			else if (c == 'A' && sel > 0)
				sel--;
			break;
		default:
			break;
		}
	}
done:
	/* bottom window always reaches nrow-2; absolute restore is drift-proof */
	bwp->w_ntrows = nrow - 2 - bwp->w_toprow;
	bwp->w_rflag |= WFFULL;
	menu_wp = NULL;
	menu_stat[0] = '\0';
	menu_hitline = NULL;
	sgarbf = TRUE;
	if (!accept) {
		for (bp = bheadp; bp != NULL; bp = bp->b_bufp)
			if (bp == pold)
				break;
		if (bp != NULL) {
			(void)showbuffer(bp, curwp, WFFULL);
			curbp = bp;
		}
		ewprintf("Quit");
	} else {
		/* the window already previews the match; keep it */
		curbp = curwp->w_bufp;
	}
	for (i = 0; i < nm; i++)
		free(ms[i].text);
	free(ms);
	free(mrow);
	return (TRUE);
}

/*
 * Ascend from the buffer's working directory to the nearest ancestor
 * containing a .git entry; fall back to the buffer's directory itself.
 * On entry and on return, dir holds an absolute path ending in '/'.
 */
static int
find_project_root(char *dir, size_t dlen)
{
	char	 cwd[NFILEN], probe[NFILEN], resolved[PATH_MAX];
	char	*p;
	struct stat sb;

	if (getbufcwd(dir, dlen) != TRUE || dir[0] == '\0')
		return (FALSE);
	/* canonicalize so inherited './' segments don't leak into results */
	if (realpath(dir, resolved) != NULL) {
		if (strlcpy(dir, resolved, dlen) >= dlen)
			return (FALSE);
		if (dir[strlen(dir) - 1] != '/' &&
		    strlcat(dir, "/", dlen) >= dlen)
			return (FALSE);
	}
	(void)strlcpy(cwd, dir, sizeof(cwd));
	for (;;) {
		if (strlcpy(probe, dir, sizeof(probe)) >= sizeof(probe) ||
		    strlcat(probe, ".git", sizeof(probe)) >= sizeof(probe))
			return (FALSE);
		if (stat(probe, &sb) == 0)
			return (TRUE);
		if (strcmp(dir, "/") == 0) {
			(void)strlcpy(dir, cwd, dlen);
			return (TRUE);
		}
		/* strip the last component; dir ends with '/' */
		p = dir + strlen(dir) - 2;
		while (p > dir && *p != '/')
			p--;
		p[1] = '\0';
	}
}

/* Quote src for the shell: wrap in single quotes, ' becomes '\''. */
static int
shellquote(char *dst, size_t dlen, const char *src)
{
	size_t	 n = 0;

	if (dlen < 3)
		return (FALSE);
	dst[n++] = '\'';
	for (; *src != '\0'; src++) {
		if (n + (*src == '\'' ? 5 : 2) >= dlen)
			return (FALSE);
		if (*src == '\'') {
			dst[n++] = '\'';
			dst[n++] = '\\';
		}
		dst[n++] = *src;
		if (*src == '\'')
			dst[n++] = '\'';
	}
	dst[n++] = '\'';
	dst[n] = '\0';
	return (TRUE);
}

struct buffer *
compile_mode(const char *name, const char *command)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	FILE	*fpipe;
	char	*buf;
	size_t	 sz;
	ssize_t	 len;
	int	 ret, n, status;
	char	 cwd[NFILEN], qcmd[NFILEN];
	char	 timestr[NTIME];
	time_t	 t;

	buf = NULL;
	sz = 0;

	n = snprintf(qcmd, sizeof(qcmd), "%s 2>&1", command);
	if (n < 0 || n >= sizeof(qcmd))
		return (NULL);

	bp = bfind(name, TRUE);
	if (bclear(bp) != TRUE)
		return (NULL);

	if (getbufcwd(bp->b_cwd, sizeof(bp->b_cwd)) != TRUE)
		return (NULL);
	addlinef(bp, "cd %s", bp->b_cwd);
	addline(bp, qcmd);
	addline(bp, "");

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		panic("Can't get current directory!");
	if (chdir(bp->b_cwd) == -1) {
		dobeep();
		ewprintf("Can't change dir to %s", bp->b_cwd);
		return (NULL);
	}
	if ((fpipe = popen(qcmd, "r")) == NULL) {
		dobeep();
		ewprintf("Problem opening pipe");
		return (NULL);
	}
	while ((len = getline(&buf, &sz, fpipe)) != -1) {
		if (buf[len - 1] == *bp->b_nlchr)
			buf[len - 1] = '\0';
		addline(bp, buf);
	}
	free(buf);
	if (ferror(fpipe))
		ewprintf("Problem reading pipe");
	ret = pclose(fpipe);
	t = time(NULL);
	strftime(timestr, sizeof(timestr), "%a %b %e %T %Y", localtime(&t));
	addline(bp, "");
	if (WIFEXITED(ret)) {
		status = WEXITSTATUS(ret);
		if (status == 0)
			addlinef(bp, "Command finished at %s", timestr);
		else
			addlinef(bp, "Command exited abnormally with code %d "
			    "at %s", status, timestr);
	} else
		addlinef(bp, "Subshell killed by signal %d at %s",
		    WTERMSIG(ret), timestr);

	bp->b_dotp = bfirstlp(bp);
	bp->b_modes[0] = name_mode("fundamental");
	bp->b_modes[1] = name_mode("compile");
	bp->b_nmodes = 1;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp) {
			wp->w_dotp = wp->w_linep = bfirstlp(bp);
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_dotline = 1;
			wp->w_rflag |= WFFULL;
		}

	compile_buffer = bp;

	if (chdir(cwd) == -1) {
		dobeep();
		ewprintf("Can't change dir back to %s", cwd);
		return (NULL);
	}
	return (bp);
}

static int
compile_goto_error(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char	*fname, *line, *lp, *ln;
	int	 lineno;
	char	*adjf, path[NFILEN];
	const char *errstr;
	struct line	*last;

	compile_win = curwp;
	compile_buffer = curbp;
	last = blastlp(compile_buffer);

 retry:
	/* last line is compilation result */
	if (curwp->w_dotp == last)
		return (FALSE);

	if ((line = linetostr(curwp->w_dotp)) == NULL)
		return (FALSE);
	lp = line;
	if ((fname = strsep(&lp, ":")) == NULL || *fname == '\0')
		goto fail;
	if ((ln = strsep(&lp, ":")) == NULL || *ln == '\0')
		goto fail;
	lineno = (int)strtonum(ln, INT_MIN, INT_MAX, &errstr);
	if (errstr)
		goto fail;

	if (fname && fname[0] != '/') {
		if (getbufcwd(path, sizeof(path)) == FALSE)
			goto fail;
		if (strlcat(path, fname, sizeof(path)) >= sizeof(path))
			goto fail;
		adjf = path;
	} else {
		adjf = adjustname(fname, TRUE);
	}
	free(line);

	if (adjf == NULL)
		return (FALSE);

	if ((bp = findbuffer(adjf)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	curwp = wp;
	if (bp->b_fname[0] == '\0')
		readin(adjf);
	gotoline(FFARG, lineno);
	return (TRUE);
fail:
	free(line);
	if (curwp->w_dotp != blastlp(curbp)) {
		curwp->w_dotp = lforw(curwp->w_dotp);
		curwp->w_rflag |= WFMOVE;
		goto retry;
	}
	dobeep();
	ewprintf("No more hits");
	return (FALSE);
}

int
next_error(int f, int n)
{
	if (compile_win == NULL || compile_buffer == NULL) {
		dobeep();
		ewprintf("No compilation active");
		return (FALSE);
	}
	curwp = compile_win;
	curbp = compile_buffer;
	if (curwp->w_dotp == blastlp(curbp)) {
		dobeep();
		ewprintf("No more hits");
		return (FALSE);
	}
	curwp->w_dotp = lforw(curwp->w_dotp);
	curwp->w_rflag |= WFMOVE;

	return (compile_goto_error(f, n));
}

/*
 * Show the file at match m in the invoking window itself, dot on the
 * first hit.  The caller restores the replaced buffer on cancel;
 * previewed file buffers are kept.
 */
static void
preview_show(const char *dir, struct smatch *m, const char *pat)
{
	struct buffer	*bp, *obp;
	struct line	*lp;
	char		 adjf[NFILEN], path[NFILEN];
	int		 i, ranges[SHIT_MAX][2];

	if (m->file[0] == '/') {
		if (strlcpy(adjf, m->file, sizeof(adjf)) >= sizeof(adjf))
			return;
	} else {
		if (snprintf(path, sizeof(path), "%s%s", dir, m->file) >=
		    (int)sizeof(path))
			return;
		if (realpath(path, adjf) == NULL &&
		    strlcpy(adjf, path, sizeof(adjf)) >= sizeof(adjf))
			return;
	}
	if ((bp = findbuffer(adjf)) == NULL)
		return;
	if (showbuffer(bp, curwp, WFFULL) != TRUE)
		return;
	/* readin splices into curwp->w_dotp's list; aim it at bp */
	if (bp->b_fname[0] == '\0') {
		obp = curbp;
		curbp = bp;
		(void)readin(adjf);
		curbp = obp;
	}
	curbp = bp;		/* preview is current while the menu runs */
	for (i = 1, lp = bfirstlp(bp); lp != bp->b_headp &&
	    i < m->lineno; i++)
		lp = lforw(lp);
	curwp->w_dotp = lp;
	curwp->w_doto = 0;
	if (pat_ranges(m->text, pat, ranges, SHIT_MAX) > 0)
		curwp->w_doto = ranges[0][0];
	curwp->w_dotline = i;
	curwp->w_linep = lp;
	curwp->w_markp = NULL;
	curwp->w_wrapline = NULL;
	curwp->w_frame = 0;		/* center the match */
	curwp->w_rflag |= WFFRAME | WFFULL;
	menu_hitline = lp;
}

/*
 * Since we don't have variables (we probably should) these are command
 * processors for changing the values of mode flags.
 */
int
globalwdtoggle(int f, int n)
{
	if (f & FFARG)
		globalwd = n > 0;
	else
		globalwd = !globalwd;

	sgarbf = TRUE;

	return (TRUE);
}
