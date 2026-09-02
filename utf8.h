/* Public domain. */

/*
 * Minimal UTF-8 support for mg: sequence length, decoding, and
 * East Asian display width.  The storage layer keeps raw bytes;
 * these helpers let the display and cursor code treat multi-byte
 * sequences as single characters with a display width of 0, 1, or 2.
 */

#ifndef UTF8_H
#define UTF8_H

#define UTF8_MAX_BYTES 4	/* longest valid UTF-8 sequence */

/*
 * Number of bytes in the UTF-8 sequence whose lead byte is `c`.
 * Returns 1 for ASCII, 0 for invalid/continuation bytes used as
 * a sentinel (callers should treat 0 as "advance one byte").
 */
int	utf8_seqlen(unsigned char c);

/*
 * Decode the UTF-8 sequence at `s` (at most `maxlen` bytes available).
 * On success returns the codepoint and stores the byte count in *len.
 * On failure returns -1 and sets *len to 1 (skip one byte).
 */
int	utf8_decode(const char *s, int maxlen, int *len);

/*
 * Display width of a Unicode codepoint:
 *   0  combining / control
 *   1  normal
 *   2  East Asian wide / fullwidth
 */
int	utf8_cwidth(int cp);

/*
 * Convenience: decode the sequence at `s` and return its display
 * width.  *len receives bytes consumed.  Returns -1 on invalid
 * input (caller should fall back to byte-at-a-time rendering).
 */
int	utf8_width(const char *s, int maxlen, int *len);

#endif /* UTF8_H */
