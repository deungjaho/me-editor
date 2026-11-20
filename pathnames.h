/*	$OpenBSD: pathnames.h,v 1.1 2012/06/18 07:14:55 jasper Exp $	*/

/* This file is in the public domain. */

/*
 *	standard path names
 */

#define	_PATH_ME_DIR		"~/.me.d"
#define	_PATH_ME_STARTUP	"%s/.me"
#define	_PATH_ME_TERM		"%s/.me-%s"
#define	_PATH_ME_SYNTAX		"%s/.me-syntax/%s.syntax"

/*
 * System-wide syntax dir as fallback when the user has no ~/.me-syntax
 * entry.  ME_SYNTAX_PREFIX is overridden by the Makefile (-D) so it
 * tracks the install PREFIX.
 */
#ifndef ME_SYNTAX_PREFIX
#define	ME_SYNTAX_PREFIX	"/usr/local"
#endif
#define	_PATH_ME_SYNTAX_SYS	ME_SYNTAX_PREFIX "/share/me/syntax/%s.syntax"
