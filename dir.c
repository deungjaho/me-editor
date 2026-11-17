/*	$OpenBSD: dir.c,v 1.33 2023/03/08 04:43:11 guenther Exp $	*/

/* This file is in the public domain. */

/*
 * Name:	MG 2a
 *		Directory management functions
 * Created:	Ron Flax (ron@vsedev.vse.com)
 *		Modified for MG 2a by Mic Kaczmarczik 03-Aug-1987
 */

#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "def.h"

static char	 mgcwd[NFILEN];

/*
 * Initialize anything the directory management routines need.
 */
void
dirinit(void)
{
	mgcwd[0] = '\0';
	if (getcwd(mgcwd, sizeof(mgcwd)) == NULL)
		ewprintf("Can't get current directory!");
	if (mgcwd[0] != '\0' && !(mgcwd[0] == '/' && mgcwd[1] == '\0'))
		(void)strlcat(mgcwd, "/", sizeof(mgcwd));
}

/*
 * Change current working directory.
 */
int
changedir(int f, int n)
{
	char	bufc[NFILEN], *bufp;

	(void)strlcpy(bufc, mgcwd, sizeof(bufc));
	if ((bufp = eread("Change default directory: ", bufc, NFILEN,
	    EFDEF | EFNEW | EFCR | EFFILE)) == NULL)
		return (ABORT);
	else if (bufp[0] == '\0')
		return (FALSE);
	/* Append trailing slash */
	if (chdir(bufc) == -1) {
		dobeep();
		ewprintf("Can't change dir to %s", bufc);
		return (FALSE);
	}
	if ((bufp = getcwd(mgcwd, sizeof(mgcwd))) == NULL) {
		if (bufc[0] == '/')
			(void)strlcpy(mgcwd, bufc, sizeof(mgcwd));
		else
			(void)strlcat(mgcwd, bufc, sizeof(mgcwd));
	}
	if (mgcwd[strlen(mgcwd) - 1] != '/')
		(void)strlcat(mgcwd, "/", sizeof(mgcwd));
	ewprintf("Current directory is now %s", mgcwd);
	return (TRUE);
}

/*
 * Show current directory.
 */
int
showcwdir(int f, int n)
{
	ewprintf("Current directory: %s", mgcwd);
	return (TRUE);
}

int
getcwdir(char *buf, size_t len)
{
	if (strlcpy(buf, mgcwd, len) >= len)
		return (FALSE);

	return (TRUE);
}

/* Create the directory and its parents. */
int
makedir(int f, int n)
{
	return (ask_makedir());
}

int
ask_makedir(void)
{

	char		 bufc[NFILEN];
	char		*path;

	if (getbufcwd(bufc, sizeof(bufc)) != TRUE)
		return (ABORT);
	if ((path = eread("Make directory: ", bufc, NFILEN,
	    EFDEF | EFNEW | EFCR | EFFILE)) == NULL)
		return (ABORT);
	else if (path[0] == '\0')
		return (FALSE);

	return (do_makedir(path));
}

int
do_makedir(char *path)
{
	struct stat	 sb;
	int		 finished, ishere;
	mode_t		 dir_mode, f_mode, oumask;
	char		*slash;

	if ((path = adjustname(path, TRUE)) == NULL)
		return (FALSE);

	/* Remove trailing slashes */
	slash = strrchr(path, '\0');
	while (--slash > path && *slash == '/')
		*slash = '\0';

	slash = path;

	oumask = umask(0);
	f_mode = 0777 & ~oumask;
	dir_mode = f_mode | S_IWUSR | S_IXUSR;

	for (;;) {
		slash += strspn(slash, "/");
		slash += strcspn(slash, "/");

		finished = (*slash == '\0');
		*slash = '\0';

		ishere = !stat(path, &sb);
		if (finished && ishere) {
			dobeep();
			ewprintf("Cannot create directory %s: file exists",
			     path);
			return(FALSE);
		} else if (!finished && ishere && S_ISDIR(sb.st_mode)) {
			*slash = '/';
			continue;
		}

		if (mkdir(path, finished ? f_mode : dir_mode) == 0) {
			if (f_mode > 0777 && chmod(path, f_mode) == -1) {
				umask(oumask);
				return (ABORT);
			}
		} else {
			if (!ishere || !S_ISDIR(sb.st_mode)) {
				if (!ishere) {
					dobeep();
					ewprintf("Creating directory: "
					    "permission denied, %s", path);
				} else
					eerase();

				umask(oumask);
				return (FALSE);
			}
		}

		if (finished)
			break;

		*slash = '/';
	}

	eerase();
	umask(oumask);
	return (TRUE);
}

int
finder(int f, int n)
{
	char	 dpath[NFILEN];
	char	*adj;
	pid_t	 pid;
	int	 status;

	if (curbp->b_fname[0] != '\0') {
		if (fisdir(curbp->b_fname) == TRUE) {
			if (strlcpy(dpath, curbp->b_fname, sizeof(dpath)) >= sizeof(dpath))
				return (FALSE);
		} else {
			char ts[NFILEN], *d;

			if (strlcpy(ts, curbp->b_fname, sizeof(ts)) >= sizeof(ts))
				return (FALSE);
			d = dirname(ts);
			if (d == NULL || strlcpy(dpath, d, sizeof(dpath)) >= sizeof(dpath))
				return (FALSE);
		}
	} else if (curbp->b_cwd[0] != '\0') {
		if (strlcpy(dpath, curbp->b_cwd, sizeof(dpath)) >= sizeof(dpath))
			return (FALSE);
	} else {
		if (getcwdir(dpath, sizeof(dpath)) == FALSE)
			(void)strlcpy(dpath, ".", sizeof(dpath));
	}

	if ((adj = adjustname(dpath, TRUE)) == NULL)
		return (FALSE);
	(void)strlcpy(dpath, adj, sizeof(dpath));

	if (fisdir(dpath) != TRUE) {
		dobeep();
		ewprintf("Not a directory: %s", dpath);
		return (FALSE);
	}

	pid = fork();
	if (pid == -1) {
		dobeep();
		ewprintf("Can't fork: %s", strerror(errno));
		return (FALSE);
	}

	if (pid == 0) {
		pid_t	cpid;
		int	devnull;

		cpid = fork();
		if (cpid == -1)
			_exit(1);
		if (cpid > 0)
			_exit(0);

		devnull = open("/dev/null", O_RDWR);
		if (devnull != -1) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		setsid();

#ifdef __APPLE__
		execlp("open", "open", dpath, (char *)NULL);
#else
		execlp("xdg-open", "xdg-open", dpath, (char *)NULL);
		execlp("gio", "gio", "open", dpath, (char *)NULL);
		execlp("nautilus", "nautilus", dpath, (char *)NULL);
		execlp("dolphin", "dolphin", dpath, (char *)NULL);
		execlp("thunar", "thunar", dpath, (char *)NULL);
#endif
		_exit(1);
	}

	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		dobeep();
		ewprintf("Failed to open file manager");
		return (FALSE);
	}

	ewprintf("Opened %s", dpath);
	return (TRUE);
}
