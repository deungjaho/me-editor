/*	$OpenBSD: kbd.c,v 1.37 2023/03/08 04:43:11 guenther Exp $	*/

/* This file is in the public domain. */

/*
 *	Terminal independent keyboard handling.
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>

#include "def.h"
#include "kbd.h"
#include "key.h"
#include "macro.h"
#include "utf8.h"

#ifdef  MGLOG
#include "log.h"
#endif

#define METABIT 0x80

#define PROMPTL 80
char	 prompt[PROMPTL] = "", *promptp = prompt;

static int mgwrap(PF, int, int);

static int		 use_metakey = TRUE;

#define NPUSH	20

static int		 pushedq[NPUSH];
static int		 pushedn;

struct map_element	*ele;
struct key		 key;
int			 rptcount;

/*
 * Toggle the value of use_metakey
 */
int
do_meta(int f, int n)
{
	if (f & FFARG)
		use_metakey = n > 0;
	else
		use_metakey = !use_metakey;
	ewprintf("Meta keys %sabled", use_metakey ? "en" : "dis");
	return (TRUE);
}

static int	 bs_map = 0;

/*
 * Toggle backspace mapping
 */
int
bsmap(int f, int n)
{
	if (f & FFARG)
		bs_map = n > 0;
	else
		bs_map = !bs_map;
	ewprintf("Backspace mapping %sabled", bs_map ? "en" : "dis");
	return (TRUE);
}

void
ungetkey(int c)
{
	/* Meta-key reassembly disabled for UTF-8 compatibility. */
	if (pushedn < NPUSH)
		pushedq[pushedn++] = c;
}

static int
nextkey(void)
{
	if (pushedn > 0)
		return (pushedq[--pushedn]);
	return (ttgetc());
}

static int
keyavail(void)
{
	return (pushedn > 0 || charswaiting() > 0 || ttwait(20) == FALSE);
}

/*
 * Encode cp as UTF-8 and push it back, first byte popped first.
 */
static void
unget_utf8(int cp)
{
	unsigned char	b[4];
	int		i = 0;

	if (cp < 0x80) {
		ungetkey(cp);
		return;
	}
	if (cp < 0x800)
		b[i++] = 0xc0 | (cp >> 6);
	else if (cp < 0x10000) {
		b[i++] = 0xe0 | (cp >> 12);
		b[i++] = 0x80 | ((cp >> 6) & 0x3f);
	} else {
		b[i++] = 0xf0 | (cp >> 18);
		b[i++] = 0x80 | ((cp >> 12) & 0x3f);
		b[i++] = 0x80 | ((cp >> 6) & 0x3f);
	}
	b[i++] = 0x80 | (cp & 0x3f);
	while (i > 0)
		ungetkey(b[--i]);
}

/*
 * Map a CSI-u codepoint sent with ctrl to its legacy control byte, -1 if
 * there is no legacy representation.
 */
static int
ctrl_code(int cp)
{
	if (cp < 0x20)
		return (cp);
	if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
		return (cp & 0x1f);
	switch (cp) {
	case ' ': case '@': case '2':
		return (0x00);
	case '[':
		return (0x1b);
	case '\\':
		return (0x1c);
	case ']':
		return (0x1d);
	case '^': case '6':
		return (0x1e);
	case '_': case '-': case '/':
		return (0x1f);
	case '?': case '8':
		return (0x7f);
	}
	return (-1);
}

static void
unget_num(int v)
{
	int	t[8], n = 0, i;

	do {
		t[n++] = '0' + v % 10;
		v /= 10;
	} while (v > 0);
	for (i = 0; i < n; i++)
		ungetkey(t[i]);	/* LSB first: pops back out as MSB first */
}

/*
 * The terminal sent ESC [ ... : try to read a kitty CSI-u event
 * (CSI cp;mod u) or an xterm modifyOtherKeys event (CSI 27;mod;cp ~) and
 * translate it into the byte stream the keymaps understand.  The leading
 * ESC was consumed by getkey; '[' is still pending.  Returns the key to
 * dispatch, or -1 after pushing bytes back: unrecognized input is
 * replayed raw, recognized events without a legacy byte form are
 * replayed normalized to CSI u so a single set of bindings covers both
 * protocols.
 */
static int
csi_key(void)
{
	int	 pbuf[16];
	int	 pl = 0, term = 0;
	int	 c, i;
	long	 f[3] = {0, 0, 0};
	int	 nf = 0, cp, m;

	if ((c = nextkey()) != '[') {
		ungetkey(c);
		return (-1);
	}
	while (pl < 16 && keyavail()) {
		c = nextkey();
		if (c == 'u' || c == '~') {
			term = c;
			break;
		}
		pbuf[pl++] = c;
	}
	if (term == 0 || pl == 0)
		goto replay;
	for (i = 0; i < pl && nf < 3; i++) {
		c = pbuf[i];
		if (c >= '0' && c <= '9')
			f[nf] = f[nf] * 10 + (c - '0');
		else if (c == ';')
			nf++;
		else if (c == ':')
			while (i + 1 < pl && pbuf[i + 1] != ';')
				i++;
		else
			goto replay;
	}
	if (term == 'u') {
		if (f[0] <= 0 || f[0] > 0x10ffff)
			goto replay;
		cp = (int)f[0];
		m = nf > 0 ? (int)f[1] - 1 : 0;
	} else {
		if (f[0] != 27 || nf < 2)
			goto replay;
		m = (int)f[1] - 1;
		cp = (int)f[2];
	}
	if (m < 0)
		m = 0;
	if (m & ~7)
		goto reemit;	/* super/hyper/meta: explicit bindings only */
	if (m & 2) {		/* alt */
		if ((m & 4) && (m & 1))
			goto reemit;
		c = (m & 4) ? ctrl_code(cp) : cp;
		if (c < 0)
			goto reemit;
		unget_utf8(c);
		return (CCHR('['));
	}
	if (m & 4) {		/* ctrl */
		if (m & 1)
			goto reemit;	/* shift+ctrl: explicit bindings */
		c = ctrl_code(cp);
		if (c < 0)
			goto reemit;
		return (c);
	}
	if (cp < 128)
		return (cp);		/* plain event, e.g. \e[27u is ESC */
	unget_utf8(cp);
	return (nextkey());
reemit:
	ungetkey('u');
	unget_num(m + 1);
	ungetkey(';');
	unget_num(cp);
	ungetkey('[');
	return (-1);
replay:
	if (term != 0)
		ungetkey(term);
	while (pl > 0)
		ungetkey(pbuf[--pl]);
	ungetkey('[');
	return (-1);
}

int
getkey(int flag)
{
	int	 c, c2, c3, i;

	if (flag && pushedn == 0) {
		if (prompt[0] != '\0' && ttwait(2000)) {
			/* avoid problems with % */
			ewprintf("%s", prompt);
			/* put the cursor back */
			update(CMODE);
			epresf = KCLEAR;
		}
		if (promptp > prompt)
			*(promptp - 1) = ' ';
	}
	c = nextkey();
	for (;;) {
		/*
		 * macOS IMEs commit U+200B (E2 80 8B) when they swallow a
		 * shortcut; drop the sequence.
		 */
		if (c == 0xE2) {
			c2 = nextkey();
			if (c2 != 0x80) {
				ungetkey(c2);
				break;
			}
			c3 = nextkey();
			if (c3 == 0x8B) {
				c = nextkey();
				continue;
			}
			ungetkey(c3);
			ungetkey(c2);
			break;
		}
		/*
		 * ESC followed by a multi-byte char: the IME translated a
		 * meta shortcut into committed text (e.g. Option+Shift+.
		 * sending ESC + U+300B).  No meta binding uses non-ASCII
		 * keys, so swallow ESC and the whole sequence; leaving the
		 * tail bytes behind would insert them as \ooo garbage.
		 */
		if (c == CCHR('[') &&
		    (pushedn > 0 || charswaiting() > 0 || ttwait(20) == FALSE)) {
			c2 = nextkey();
			if (utf8_seqlen((unsigned char)c2) >= 2) {
				for (i = utf8_seqlen((unsigned char)c2) - 1;
				    i > 0; i--)
					(void)nextkey();
				c = nextkey();
				continue;
			}
			if (c2 == '[' && keyavail()) {
				ungetkey(c2);
				c3 = csi_key();
				if (c3 >= 0) {
					c = c3;
					break;
				}
				break;
			}
			ungetkey(c2);
		}
		break;
	}

	if (bs_map) {
		if (c == CCHR('H'))
			c = CCHR('?');
		else if (c == CCHR('?'))
			c = CCHR('H');
	}
	/*
	 * Meta-key translation (ESC + char) is incompatible with UTF-8:
	 * bytes >= 0x80 are multi-byte sequence bytes, not meta keys.
	 * Pass them through unchanged.  Meta keys remain available via
	 * the ESC prefix (press ESC then the key).
	 */
	if (flag && promptp < &prompt[PROMPTL - 5]) {
		promptp = getkeyname(promptp,
		    sizeof(prompt) - (promptp - prompt) - 1, c);
		*promptp++ = '-';
		*promptp = '\0';
	}
	return (c);
}

/*
 * doscan scans a keymap for a keyboard character and returns a pointer
 * to the function associated with that character.  Sets ele to the
 * keymap element the keyboard was found in as a side effect.
 */
PF
doscan(KEYMAP *map, int c, KEYMAP **newmap)
{
	struct map_element	*elec = &map->map_element[0];
	struct map_element	*last = &map->map_element[map->map_num];
	PF		 ret;

	while (elec < last && c > elec->k_num)
		elec++;

	/* used by prefix and binding code */
	ele = elec;
	if (elec >= last || c < elec->k_base)
		ret = map->map_default;
	else
		ret = elec->k_funcp[c - elec->k_base];
	if (ret == NULL && newmap != NULL)
		*newmap = elec->k_prefmap;

	return (ret);
}

int
doin(void)
{
	KEYMAP	*curmap;
	PF	 funct;

	*(promptp = prompt) = '\0';
	curmap = curbp->b_modes[curbp->b_nmodes]->p_map;
	key.k_count = 0;
	while ((funct = doscan(curmap, (key.k_chars[key.k_count++] =
	    getkey(TRUE)), &curmap)) == NULL)
		/* nothing */;

#ifdef  MGLOG
	if (!mglog(funct, curmap))
		ewprintf("Problem with logging");
#endif

	if (macrodef && macrocount < MAXMACRO)
		macro[macrocount++].m_funct = funct;

	return (mgwrap(funct, 0, 1));
}

int
rescan(int f, int n)
{
	int	 c;
	KEYMAP	*curmap;
	int	 i;
	PF	 fp = NULL;
	int	 md = curbp->b_nmodes;

	for (;;) {
		if (ISUPPER(key.k_chars[key.k_count - 1])) {
			c = TOLOWER(key.k_chars[key.k_count - 1]);
			curmap = curbp->b_modes[md]->p_map;
			for (i = 0; i < key.k_count - 1; i++) {
				if ((fp = doscan(curmap, (key.k_chars[i]),
				    &curmap)) != NULL)
					break;
			}
			if (fp == NULL) {
				if ((fp = doscan(curmap, c, NULL)) == NULL)
					while ((fp = doscan(curmap,
					    key.k_chars[key.k_count++] =
					    getkey(TRUE), &curmap)) == NULL)
						/* nothing */;
				if (fp != rescan) {
					if (macrodef && macrocount <= MAXMACRO)
						macro[macrocount - 1].m_funct
						    = fp;
					return (mgwrap(fp, f, n));
				}
			}
		}
		/* try previous mode */
		if (--md < 0)
			return (ABORT);
		curmap = curbp->b_modes[md]->p_map;
		for (i = 0; i < key.k_count; i++) {
			if ((fp = doscan(curmap, (key.k_chars[i]), &curmap)) != NULL)
				break;
		}
		if (fp == NULL) {
			while ((fp = doscan(curmap, key.k_chars[i++] =
			    getkey(TRUE), &curmap)) == NULL)
				/* nothing */;
			key.k_count = i;
		}
		if (fp != rescan && i >= key.k_count - 1) {
			if (macrodef && macrocount <= MAXMACRO)
				macro[macrocount - 1].m_funct = fp;
			return (mgwrap(fp, f, n));
		}
	}
}

int
universal_argument(int f, int n)
{
	KEYMAP	*curmap;
	PF	 funct;
	int	 c, nn = 4;

	if (f & FFUNIV)
		nn *= n;
	for (;;) {
		key.k_chars[0] = c = getkey(TRUE);
		key.k_count = 1;
		if (c == '-')
			return (negative_argument(f, nn));
		if (c >= '0' && c <= '9')
			return (digit_argument(f, nn));
		curmap = curbp->b_modes[curbp->b_nmodes]->p_map;
		while ((funct = doscan(curmap, c, &curmap)) == NULL) {
			key.k_chars[key.k_count++] = c = getkey(TRUE);
		}
		if (funct != universal_argument) {
			if (macrodef && macrocount < MAXMACRO - 1) {
				if (f & FFARG)
					macrocount--;
				macro[macrocount++].m_count = nn;
				macro[macrocount++].m_funct = funct;
			}
			return (mgwrap(funct, FFUNIV, nn));
		}
		nn <<= 2;
	}
}

int
digit_argument(int f, int n)
{
	KEYMAP	*curmap;
	PF	 funct;
	int	 nn, c;

	nn = key.k_chars[key.k_count - 1] - '0';
	for (;;) {
		c = getkey(TRUE);
		if (c < '0' || c > '9')
			break;
		nn *= 10;
		nn += c - '0';
	}
	key.k_chars[0] = c;
	key.k_count = 1;
	curmap = curbp->b_modes[curbp->b_nmodes]->p_map;
	while ((funct = doscan(curmap, c, &curmap)) == NULL) {
		key.k_chars[key.k_count++] = c = getkey(TRUE);
	}
	if (macrodef && macrocount < MAXMACRO - 1) {
		if (f & FFARG)
			macrocount--;
		else
			macro[macrocount - 1].m_funct = universal_argument;
		macro[macrocount++].m_count = nn;
		macro[macrocount++].m_funct = funct;
	}
	return (mgwrap(funct, FFOTHARG, nn));
}

int
negative_argument(int f, int n)
{
	KEYMAP	*curmap;
	PF	 funct;
	int	 c;
	int	 nn = 0;

	for (;;) {
		c = getkey(TRUE);
		if (c < '0' || c > '9')
			break;
		nn *= 10;
		nn += c - '0';
	}
	if (nn)
		nn = -nn;
	else
		nn = -n;
	key.k_chars[0] = c;
	key.k_count = 1;
	curmap = curbp->b_modes[curbp->b_nmodes]->p_map;
	while ((funct = doscan(curmap, c, &curmap)) == NULL) {
		key.k_chars[key.k_count++] = c = getkey(TRUE);
	}
	if (macrodef && macrocount < MAXMACRO - 1) {
		if (f & FFARG)
			macrocount--;
		else
			macro[macrocount - 1].m_funct = universal_argument;
		macro[macrocount++].m_count = nn;
		macro[macrocount++].m_funct = funct;
	}
	return (mgwrap(funct, FFNEGARG, nn));
}

/*
 * Insert a character.	While defining a macro, create a "LINE" containing
 * all inserted characters.
 */
int
selfinsert(int f, int n)
{
	struct line	*lp;
	int	 c;
	int	 count;
	char	 ubuf[UTF8_MAX_BYTES];
	int	 ulen = 0, i, j;

	if (n < 0)
		return (FALSE);
	if (n == 0)
		return (TRUE);
	c = key.k_chars[key.k_count - 1];

	/* Assemble full UTF-8 sequence if lead byte */
	if ((unsigned char)c >= 0x80) {
		int seqlen = utf8_seqlen((unsigned char)c);
		/* orphan continuation byte: lead was consumed elsewhere */
		if (seqlen == 0)
			return (TRUE);
		if (seqlen > 1) {
			ubuf[0] = c;
			for (i = 1; i < seqlen; i++)
				ubuf[i] = getkey(TRUE);
			ulen = seqlen;
		}
	}

	if (macrodef && macrocount < MAXMACRO) {
		int total = ulen > 0 ? ulen * n : n;
		if (f & FFARG)
			macrocount -= 2;

		/* last command was insert -- tack on the end */
		if (lastflag & CFINS) {
			macrocount--;
			if (maclcur->l_size < maclcur->l_used + total) {
				if (lrealloc(maclcur, maclcur->l_used + total)
				    == FALSE)
					return (FALSE);
			}
			maclcur->l_used += total;
			for (count = maclcur->l_used - total;
			    count < maclcur->l_used; ) {
				if (ulen > 0)
					for (j = 0; j < ulen; j++)
						maclcur->l_text[count++] =
						    ubuf[j];
				else
					maclcur->l_text[count++] = c;
			}
		} else {
			macro[macrocount - 1].m_funct = insert;
			if ((lp = lalloc(total)) == NULL)
				return (FALSE);
			lp->l_bp = maclcur;
			lp->l_fp = maclcur->l_fp;
			maclcur->l_fp = lp;
			maclcur = lp;
			for (count = 0; count < total; ) {
				if (ulen > 0)
					for (j = 0; j < ulen; j++)
						lp->l_text[count++] = ubuf[j];
				else
					lp->l_text[count++] = c;
			}
		}
		thisflag |= CFINS;
	}
	if (c == *curbp->b_nlchr) {
		do {
			count = lnewline();
		} while (--n && count == TRUE);
		return (count);
	}

	/* overwrite mode (byte-at-a-time; UTF-8 falls through to insert) */
	if (ulen == 0 && (curbp->b_flag & BFOVERWRITE)) {
		lchange(WFEDIT);
		while (curwp->w_doto < llength(curwp->w_dotp) && n--)
			lputc(curwp->w_dotp, curwp->w_doto++, c);
		if (n <= 0)
			return (TRUE);
	}
	if (ulen > 0) {
		for (i = 0; i < n; i++)
			if (linsert_buf(ubuf, ulen) != TRUE)
				return (FALSE);
		return (TRUE);
	}
	return (linsert(n, c));
}

/*
 * selfinsert() can't be called directly from a startup file or by
 * 'eval-current-buffer' since it is by design, meant to be called interactively
 * as characters are typed in a buffer. ask_selfinsert() allows selfinsert() to
 * be used by excline(). Having ask_selfinsert() helps with regression testing.
 * No manual page entry since use case is a bit obscure. See 'insert' command.
 */
int
ask_selfinsert(int f, int n)
{
	char	*c, cbuf[2];

	if ((c = eread("Insert a character: ", cbuf, sizeof(cbuf),
	    EFNEW)) == NULL || (c[0] == '\0'))
		return (ABORT);

	key.k_chars[0] = *c;
	key.k_chars[1] = '\0';
	key.k_count = 1;

	return (selfinsert(FFRAND, 1));
}

/*
 * This could be implemented as a keymap with everything defined as self-insert.
 */
int
quote(int f, int n)
{
	int	 c;

	key.k_count = 1;
	if ((key.k_chars[0] = getkey(TRUE)) >= '0' && key.k_chars[0] <= '7') {
		key.k_chars[0] -= '0';
		if ((c = getkey(TRUE)) >= '0' && c <= '7') {
			key.k_chars[0] <<= 3;
			key.k_chars[0] += c - '0';
			if ((c = getkey(TRUE)) >= '0' && c <= '7') {
				key.k_chars[0] <<= 3;
				key.k_chars[0] += c - '0';
			} else
				ungetkey(c);
		} else
			ungetkey(c);
	}
	return (selfinsert(f, n));
}

enum paste_end_state {
	PASTE_TEXT = 0,
	PASTE_ESC,
	PASTE_CSI,
	PASTE_2,
	PASTE_20,
	PASTE_201
};

static void
paste_flush(char *buf, size_t *len, int *ok)
{
	if (*len > 0) {
		if (*ok && linsert_buf(buf, (int)*len) != TRUE)
			*ok = FALSE;
		*len = 0;
	}
}

static void
paste_emit_byte(unsigned char b, char *buf, size_t bufcap, size_t *len,
    int *prev_cr, int *ok)
{
	if (b == '\r') {
		paste_flush(buf, len, ok);
		if (*ok && lnewline() != TRUE)
			*ok = FALSE;
		*prev_cr = 1;
		return;
	}
	if (b == '\n') {
		paste_flush(buf, len, ok);
		if (!*prev_cr && *ok && lnewline() != TRUE)
			*ok = FALSE;
		*prev_cr = 0;
		return;
	}
	*prev_cr = 0;
	if (!*ok)
		return;
	/* Flush on UTF-8 lead/ASCII boundaries near capacity to avoid split codepoints. */
	if (*len >= bufcap || (*len >= bufcap - 4 && (b & 0xC0) != 0x80))
		paste_flush(buf, len, ok);
	if (*ok)
		buf[(*len)++] = (char)b;
}

/*
 * Consume a terminal bracketed paste (\e[200~ ... \e[201~) and insert its
 * contents literally without triggering auto-indent, autopair, or fill.
 */
int
bracketed_paste(int f, int n)
{
	static const char	 paste_end[] = "\033[201~";
	enum paste_end_state	 state = PASTE_TEXT;
	char			 buf[256];
	size_t			 len = 0;
	int			 prev_cr = 0;
	int			 ok = TRUE;
	int			 i, raw;
	unsigned char		 c;

	undo_boundary_enable(FFRAND, 0);
	for (;;) {
		raw = nextkey();
		c = (unsigned char)raw;
		if (c == (unsigned char)paste_end[state]) {
			if (state == PASTE_201)
				break;
			state = (enum paste_end_state)(state + 1);
			continue;
		}
		if (state != PASTE_TEXT) {
			for (i = 0; i < (int)state; i++)
				paste_emit_byte((unsigned char)paste_end[i],
				    buf, sizeof(buf), &len, &prev_cr, &ok);
			state = PASTE_TEXT;
			if (c == (unsigned char)paste_end[0]) {
				state = PASTE_ESC;
				continue;
			}
		}
		paste_emit_byte(c, buf, sizeof(buf), &len, &prev_cr, &ok);
	}
	paste_flush(buf, &len, &ok);
	undo_boundary_enable(FFRAND, 1);
	return (ok);
}

/*
 * Wrapper function to count invocation repeats.
 * We ignore any function whose sole purpose is to get us
 * to the intended function.
 */
static int
mgwrap(PF funct, int f, int n)
{
	static	 PF ofp;

	if (funct != rescan &&
	    funct != negative_argument &&
	    funct != digit_argument &&
	    funct != universal_argument) {
		if (funct == ofp)
			rptcount++;
		else
			rptcount = 0;
		ofp = funct;
	}

	return ((*funct)(f, n));
}
