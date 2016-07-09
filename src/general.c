/*
*  C Implementation: general
*
* Description: General functions, not directly related to file system operations
*
* original implementation by Radek Podgorny
*
* License: BSD-style license
* Copyright: Radek Podgorny <radek@podgorny.cz>,
*            Bernd Schubert <bernd-schubert@gmx.de>
*
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>

#include "unionfs.h"
#include "opts.h"
#include "string.h"
#include "cow.h"
#include "findbranch.h"
#include "general.h"
#include "debug.h"
#include "usyslog.h"

/**
 * Check if a file or directory is hidden.
 * @param path Path to be tested.
 * @param length Consider up to 'length' bytes in 'path'.
 *  	Zero means the whole string.
 * @retval 1 If the file is hidden.
 * @retval 0 if it's not.
 */
static int filedir_hidden(const char *path, size_t length) {
	// cow mode disabled, no need for hidden files
	if (!uopt.cow_enabled) RETURN(false);
	
	char p[PATHLEN_MAX];
	static const size_t tag_length = strlen(HIDETAG);

	if (length == 0) length = strlen(path);
	if (length + tag_length >= PATHLEN_MAX) RETURN(-ENAMETOOLONG);
	memcpy(p, path, length);
	memcpy((p + length), HIDETAG, tag_length);
	p[length + tag_length] = '\0';
	DBG("%s\n", p);

	struct stat stbuf;
	int res = lstat(p, &stbuf);
	if (res == 0) RETURN(1);

	RETURN(0);
}

/**
 * check if any dir or file within path is hidden
 */
int path_hidden(const char *path, int branch) {
	DBG("%s\n", path);

	if (!uopt.cow_enabled) RETURN(false);

	char whiteoutpath[PATHLEN_MAX];
	if (BUILD_PATH(whiteoutpath, uopt.branches[branch].path, METADIR, path)) RETURN(false);

	// -1 as we MUST not end on the next path element 
	char *walk = whiteoutpath + uopt.branches[branch].path_len + strlen(METADIR) - 1;

	// first slashes, e.g. we have path = /dir1/dir2/, will set walk = dir1/dir2/
	while (*walk == '/') walk++;

	do {
		// walk over the directory name, walk will now be /dir2
		while (*walk != '\0' && *walk != '/') walk++;

		// walk - path = strlen(/dir1)
		int res = filedir_hidden(whiteoutpath, (walk - whiteoutpath));
		if (res != 0) RETURN(res); // path is hidden or error

		// as above the do loop, walk over the next slashes, walk = dir2/
		while (*walk == '/') walk++;
	} while (*walk != '\0');

	RETURN(0);
}

/**
 * Remove a hide-file in all branches up to maxbranch
 * If maxbranch == -1, try to delete it in all branches.
 */
int remove_hidden(const char *path, int maxbranch) {
	DBG("%s\n", path);

	if (!uopt.cow_enabled) RETURN(0);

	if (maxbranch == -1) maxbranch = uopt.nbranches;

	int i;
	for (i = 0; i <= maxbranch; i++) {
		char p[PATHLEN_MAX];
		if (BUILD_PATH(p, uopt.branches[i].path, METADIR, path)) RETURN(-ENAMETOOLONG);
		if (strlen(p) + strlen(HIDETAG) > PATHLEN_MAX) RETURN(-ENAMETOOLONG);
		strcat(p, HIDETAG); // TODO check length

		switch (path_is_dir(p)) {
			case IS_FILE: unlink(p); break;
			case IS_DIR: rmdir(p); break;
			case NOT_EXISTING: continue;
		}
	}

	RETURN(0);
}

/**
 * check if path is a directory
 *
 * return proper types given by filetype_t
 */
filetype_t path_is_dir(const char *path) {
	DBG("%s\n", path);

	struct stat buf;
	
	if (lstat(path, &buf) == -1) RETURN(NOT_EXISTING);
	
	if (S_ISDIR(buf.st_mode)) RETURN(IS_DIR);
	
	RETURN(IS_FILE);
}

/**
 * Create a file or directory that hides path below branch_rw
 */
static int do_create_whiteout(const char *path, int branch_rw, enum whiteout mode) {
	DBG("%s\n", path);

	char metapath[PATHLEN_MAX];

	if (BUILD_PATH(metapath, METADIR, path))  RETURN(-1);

	// p MUST be without path to branch prefix here! 2 x branch_rw is correct here!
	// this creates e.g. branch/.unionfs/some_directory
	path_create_cutlast(metapath, branch_rw, branch_rw);

	char p[PATHLEN_MAX];
	if (BUILD_PATH(p, uopt.branches[branch_rw].path, metapath)) RETURN(-1);
	strcat(p, HIDETAG); // TODO check length

	int res;
	if (mode == WHITEOUT_FILE) {
		res = open(p, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
		if (res == -1) RETURN(-1);
		res = close(res);
	} else {
		res = mkdir(p, S_IRWXU);
		if (res)
			USYSLOG(LOG_ERR, "Creating %s failed: %s\n", p, strerror(errno));
	}

	RETURN(res);
}

/**
 * Create a file that hides path below branch_rw
 */
int hide_file(const char *path, int branch_rw) {
	DBG("%s\n", path);
	int res = do_create_whiteout(path, branch_rw, WHITEOUT_FILE);
	RETURN(res);
}

/**
 * Create a directory that hides path below branch_rw
 */
int hide_dir(const char *path, int branch_rw) {
	DBG("%s\n", path);
	int res = do_create_whiteout(path, branch_rw, WHITEOUT_DIR);
	RETURN(res);
}

/**
 * This is called *after* unlink() or rmdir(), create a whiteout file
 * if the same file/dir does exist in a lower branch
 */
int maybe_whiteout(const char *path, int branch_rw, enum whiteout mode) {
	DBG("%s\n", path);

	// we are not interested in the branch itself, only if it exists at all
	if (find_rorw_branch(path) != -1) {
		int res = do_create_whiteout(path, branch_rw, mode);
		RETURN(res);
	}

	RETURN(0);
}

/**
 * Set file owner of after an operation, which created a file.
 */
int set_owner(const char *path) {
	struct fuse_context *ctx = fuse_get_context();
	if (ctx->uid != 0 && ctx->gid != 0) {
		int res = lchown(path, ctx->uid, ctx->gid);
		if (res) {
			USYSLOG(LOG_WARNING,
			       ":%s: Setting the correct file owner failed: %s !\n", 
			       __func__, strerror(errno));
			RETURN(-errno);
		}
	}
	RETURN(0);
}
