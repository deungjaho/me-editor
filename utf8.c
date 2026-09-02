/* Public domain. */

/*
 * UTF-8 helpers for mg.
 *
 * The East Asian Width table is a compact sorted array of {first, last}
 * codepoint ranges that are "wide" (display width 2).  Everything else
 * non-cominating is width 1.  Combining characters (width 0) are listed
 * in a second table.  Binary search keeps lookups to O(log n) with very
 * small constant factor — the wide table has ~20 entries.
 */

#include <stdlib.h>

#include "utf8.h"

/* ---- sequence length ---- */

int
utf8_seqlen(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 0;	/* continuation or invalid */
}

/* ---- decoding ---- */

int
utf8_decode(const char *s, int maxlen, int *len)
{
	const unsigned char *p = (const unsigned char *)s;
	int cp, n, i;

	if (maxlen <= 0) {
		*len = 1;
		return -1;
	}

	n = utf8_seqlen(p[0]);
	if (n == 0 || n > maxlen) {
		*len = 1;
		return -1;
	}

	if (n == 1) {
		*len = 1;
		return p[0];
	}

	cp = p[0] & (0x7F >> n);
	for (i = 1; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*len = 1;
			return -1;
		}
		cp = (cp << 6) | (p[i] & 0x3F);
	}

	/* reject overlong encodings and surrogates */
	if (n == 2 && cp < 0x80) {
		*len = 1;
		return -1;
	}
	if (n == 3 && cp < 0x800) {
		*len = 1;
		return -1;
	}
	if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) {
		*len = 1;
		return -1;
	}
	if (cp >= 0xD800 && cp <= 0xDFFF) {
		*len = 1;
		return -1;
	}

	*len = n;
	return cp;
}

/* ---- East Asian Width table ---- */

struct range {
	int first, last;
};

/* Codepoints with display width 2 (East Asian Wide / Fullwidth). */
static const struct range wide_tab[] = {
	{ 0x1100, 0x115F },	/* Hangul Jamo */
	{ 0x2329, 0x232A },	/* Angle brackets */
	{ 0x2E80, 0x303E },	/* CJK radicals, Kangxi */
	{ 0x3041, 0x33FF },	/* Hiragana, Katakana, Bopomofo, Kanbun, CJK symbols */
	{ 0x3400, 0x4DBF },	/* CJK Unified Ideographs Extension A */
	{ 0x4E00, 0x9FFF },	/* CJK Unified Ideographs */
	{ 0xA000, 0xA4CF },	/* Yi */
	{ 0xAC00, 0xD7A3 },	/* Hangul Syllables */
	{ 0xF900, 0xFAFF },	/* CJK Compatibility Ideographs */
	{ 0xFE10, 0xFE19 },	/* Vertical forms */
	{ 0xFE30, 0xFE6F },	/* CJK Compatibility Forms */
	{ 0xFF00, 0xFF60 },	/* Fullwidth ASCII */
	{ 0xFFE0, 0xFFE6 },	/* Fullwidth signs */
	{ 0x1F300, 0x1F64F },	/* Emoji – pictographs */
	{ 0x1F900, 0x1F9FF },	/* Supplemental symbols */
	{ 0x20000, 0x2FFFD },	/* CJK Extension B-F */
	{ 0x30000, 0x3FFFD },	/* CJK Extension G+ */
};

/* Codepoints with display width 0 (combining marks). */
static const struct range comb_tab[] = {
	{ 0x0300, 0x036F },	/* Combining diacritical marks */
	{ 0x0483, 0x0489 },
	{ 0x0591, 0x05BD },
	{ 0x05BF, 0x05BF },
	{ 0x05C1, 0x05C2 },
	{ 0x05C4, 0x05C5 },
	{ 0x05C7, 0x05C7 },
	{ 0x0600, 0x0605 },
	{ 0x0610, 0x061A },
	{ 0x061C, 0x061C },
	{ 0x064B, 0x065F },
	{ 0x0670, 0x0670 },
	{ 0x06D6, 0x06DD },
	{ 0x06DF, 0x06E4 },
	{ 0x06E7, 0x06E8 },
	{ 0x06EA, 0x06ED },
	{ 0x070F, 0x070F },
	{ 0x0711, 0x0711 },
	{ 0x0730, 0x074A },
	{ 0x200B, 0x200F },	/* Zero-width spaces */
	{ 0x202A, 0x202E },	/* Bidi controls */
	{ 0x2060, 0x2064 },
	{ 0x2066, 0x206F },
	{ 0xFE00, 0xFE0F },	/* Variation selectors */
	{ 0xFEFF, 0xFEFF },	/* BOM */
	{ 0xFFF9, 0xFFFB },
	{ 0x1D167, 0x1D169 },
	{ 0x1D173, 0x1D182 },
	{ 0x1D185, 0x1D18B },
	{ 0x1D1AA, 0x1D1AD },
	{ 0xE0001, 0xE0001 },
	{ 0xE0020, 0xE007F },
};

static int
in_range(int cp, const struct range *tab, int n)
{
	int lo = 0, hi = n - 1, mid;

	while (lo <= hi) {
		mid = lo + (hi - lo) / 2;
		if (cp < tab[mid].first)
			hi = mid - 1;
		else if (cp > tab[mid].last)
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

int
utf8_cwidth(int cp)
{
	if (cp < 0x20 || cp == 0x7F)
		return 0;	/* control */
	if (cp < 0x7F)
		return 1;	/* ASCII printable */
	if (in_range(cp, comb_tab, (int)(sizeof(comb_tab) / sizeof(comb_tab[0]))))
		return 0;
	if (in_range(cp, wide_tab, (int)(sizeof(wide_tab) / sizeof(wide_tab[0]))))
		return 2;
	return 1;
}

/* ---- combined decode + width ---- */

int
utf8_width(const char *s, int maxlen, int *len)
{
	int cp;

	cp = utf8_decode(s, maxlen, len);
	if (cp < 0)
		return -1;
	return utf8_cwidth(cp);
}
