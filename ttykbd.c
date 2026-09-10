/*	$OpenBSD: ttykbd.c,v 1.22 2023/03/30 19:00:02 op Exp $	*/

/* This file is in the public domain. */

/*
 * Name:	MG 2a
 *		Terminfo keyboard driver using key files
 * Created:	22-Nov-1987 Mic Kaczmarczik (mic@emx.cc.utexas.edu)
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <term.h>

#include "def.h"
#include "kbd.h"

/*
 * Get keyboard character.  Very simple if you use keymaps and keys files.
 */

char	*keystrings[] = {NULL};

/*
 * Turn on function keys using keypad_xmit, then load a keys file, if
 * available.  The keys file is located in the same manner as the startup
 * file is, depending on what startupfile() does on your system.
 */
void
ttykeymapinit(void)
{
	char	*cp, file[NFILEN];
	FILE	*ffp;

	/* Bind keypad function keys. */
	if (key_left)
		dobindkey(fundamental_map, "backward-char", key_left);
	if (key_right)
		dobindkey(fundamental_map, "forward-char", key_right);
	if (key_up)
		dobindkey(fundamental_map, "previous-line", key_up);
	if (key_down)
		dobindkey(fundamental_map, "next-line", key_down);
	if (key_beg)
		dobindkey(fundamental_map, "beginning-of-line", key_beg);
	else if (key_home)
		dobindkey(fundamental_map, "beginning-of-line", key_home);
	if (key_end)
		dobindkey(fundamental_map, "end-of-line", key_end);
	if (key_npage)
		dobindkey(fundamental_map, "scroll-up", key_npage);
	if (key_ppage)
		dobindkey(fundamental_map, "scroll-down", key_ppage);
	if (key_ic)
		dobindkey(fundamental_map, "overwrite-mode", key_ic);
	if (key_dc)
		dobindkey(fundamental_map, "delete-char", key_dc);
	if (key_f1)
		dobindkey(fundamental_map, "help-help", key_f1);

	/* Bind auto-pair delimiters on fundamental map */
	dobindkey(fundamental_map, "autopair-insert", "(");
	dobindkey(fundamental_map, "autopair-insert", "[");
	dobindkey(fundamental_map, "autopair-insert", "{");
	dobindkey(fundamental_map, "autopair-insert", "\"");
	dobindkey(fundamental_map, "autopair-insert", "'");
	dobindkey(fundamental_map, "autopair-insert", ")");
	dobindkey(fundamental_map, "autopair-insert", "]");
	dobindkey(fundamental_map, "autopair-insert", "}");
	dobindkey(fundamental_map, "smart-backdel", "\\^H");
	dobindkey(fundamental_map, "smart-backdel", "\\^?");

	/* Cmd-/ sends ESC / or CSI sequences depending on terminal; bind M-/ and Alt-; */
	dobindkey(fundamental_map, "comment-line", "\\e;");
	dobindkey(fundamental_map, "comment-line", "\\e/");
	dobindkey(fundamental_map, "comment-line", "\\e[106;9u"); /* Kitty / CSI u Cmd-/ */
	dobindkey(fundamental_map, "comment-line", "\\e[47;9u");  /* CSI u Cmd-/ */
	dobindkey(fundamental_map, "comment-line", "\\e[59;3u");  /* Alt-; in CSI u */

	/* Ctrl-Shift-/ sends CSI u with shift modifier (e.g. \e[47;6u or \e[31;2u or \e[47;2u); bind to redo */
	dobindkey(fundamental_map, "redo", "\\e[47;6u");
	dobindkey(fundamental_map, "redo", "\\e[47;2u");
	dobindkey(fundamental_map, "redo", "\\e[31;6u");
	dobindkey(fundamental_map, "redo", "\\e[63;6u"); /* ? with Ctrl */
	dobindkey(fundamental_map, "redo", "\\e[63;2u"); /* ? with Shift */
	dobindkey(fundamental_map, "redo", "\\e_");     /* M-_ (standard Emacs redo) */

	/* Bind NBSP (Option-Space on macOS Cocoa terminal) to set-mark-command */
	dobindkey(fundamental_map, "set-mark-command", "\xc2\xa0");
	dobindkey(fundamental_map, "set-mark-command", "\\e ");

	if ((cp = getenv("TERM")) != NULL &&
	    (ffp = startupfile(cp, NULL, file, sizeof(file))) != NULL) {
		if (load(ffp, file) != TRUE)
			ewprintf("Error reading key initialization file");
		(void)ffclose(ffp, NULL);
	}
	if (keypad_xmit)
		/* turn on keypad */
		putpad(keypad_xmit, 1);
}

/*
 * Clean up the keyboard -- called by tttidy()
 */
void
ttykeymaptidy(void)
{
	if (keypad_local)
		/* turn off keypad */
		putpad(keypad_local, 1);
}
