/*
*  C Implementation: string
*
* Description: General string functions, not directly related to file system operations
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
#include <stdbool.h>
#include <errno.h>

#include "unionfs.h"
#include "opts.h"
#include "debug.h"
#include "general.h"
#include "usyslog.h"

/**
 * Check if the given fname suffixes the hide tag
 */
char *whiteout_tag(const char *fname) {
	DBG("%s\n", fname);

	char *tag = strstr(fname, HIDETAG);

	// check if fname has tag, fname is not only the tag, file name ends with the tag
	// TODO: static strlen(HIDETAG)
	if (tag && tag != fname && strlen(tag) == strlen(HIDETAG)) {
		return tag;
	}

	return NULL;
}

/**
 * copy one or more char arrays into dest and check for maximum size
 *
 * arguments: maximal string length and one or more char* string arrays
 *
 * check if the sum of the strings is larger than PATHLEN_MAX
 *
 * This function requires a NULL as last argument!
 * 
 * path already MUST have been allocated!
 */
int build_path(char *path, size_t max_len, const char *callfunc, int line, ...) {
	va_list ap; // argument pointer
	size_t len = 0;
	size_t arg_len;
	char *str_ptr = path;
	char *arg;

	(void)str_ptr; // please the compile to avoid warning in non-debug mode
	(void)line;
	(void)callfunc; 

	va_start(ap, line);
	*path = '\0';
	/* This is the first step of the loop unrolled due to differences */
	arg = va_arg(ap, char *); // the first path element
	if(arg == NULL || (arg_len = strlen(arg)) == 0){
		va_end(ap);
		USYSLOG(LOG_ERR, "from: %s():%d : No argument given?\n", callfunc, line);
		errno = EIO;
		RETURN(-errno);
	}
	if(len + arg_len + 1 > max_len){ // +1 for final '\0'
		va_end(ap);
		USYSLOG (LOG_WARNING, "%s():%d Path too long \n", callfunc, line);
		errno = ENAMETOOLONG;
		RETURN(-errno);
	}
	memcpy(path, arg, arg_len);
	len += arg_len;
	path += arg_len; // walk to the end of path

	while((arg = va_arg(ap, char *)) != NULL){
		/* Prevent "//" in paths, we have actually 3 possibilities here
		 * 1 - When our partial path ends with '/' and the next append starts
		 * with '/', which would give us "//".
		 * 2 - When there isn't '/' in neither.
		 * 3 - When there is a '/' on the end of our partial string or in the
		 * beginning of the next part, which in that case we don't need to do
		 * anything.
		 */
		if(path[-1] == '/' && arg[0] == '/'){
			path--;
			len--;
		}
		else if(path[-1] != '/' && arg[0] != '/'){
			*path = '/';
			path++;
			len++;
		}
		arg_len = strlen(arg);

		if(len + arg_len + 1 > max_len){ // +1 for final '\0'
			*path = '\0';
			va_end(ap);
			USYSLOG (LOG_WARNING, "%s():%d Path too long \n", callfunc, line);
			errno = ENAMETOOLONG;
			RETURN(-errno);
		}

		memcpy(path, arg, arg_len);
		len += arg_len;
		path += arg_len; // walk to the end of path
	}
	*path = '\0';
	va_end(ap);
	
	DBG("from: %s():%d path: %s\n", callfunc, line, str_ptr);
	RETURN(0);
}

/**
 * dirname() in libc might not be thread-save, at least the man page states
 * "may return pointers to statically allocated memory", so we need our own
 * implementation
 */
char *u_dirname(const char *path) {
	DBG("%s\n", path);

	char *ret = strdup(path);
	if (ret == NULL) {
		USYSLOG(LOG_WARNING, "strdup failed, probably out of memory!\n");
		return ret;
	}

	char *ri = strrchr(ret, '/'); 
	if (ri != NULL) {
		*ri = '\0'; // '/' found, so a full path
	} else {
		strcpy(ret, "."); // '/' not found, so path is only a file
	}

	return ret;
}

/**
 * general elf hash (32-bit) function
 *
 * Algorithm taken from URL: http://www.partow.net/programming/hashfunctions/index.html,
 * but rewritten from scratch due to incompatible license.
 *
 * str needs to NULL terminated
 */
static unsigned int elfhash(const char *str) {
	DBG("%s\n", str);

	unsigned int hash = 0;

	while (*str) {
		hash = (hash << 4) + (*str); // hash * 16 + c

		// 0xF is 1111 in dual system, so highbyte is the highest byte of hash (which is 32bit == 4 Byte)
		unsigned int highbyte = hash & 0xF0000000UL;

		if (highbyte != 0) hash ^= (highbyte >> 24);
		// example (if the condition is met):
		//               hash = 10110000000000000000000010100000
		//           highbyte = 10110000000000000000000000000000
		//   (highbyte >> 24) = 00000000000000000000000010110000
		// after XOR:    hash = 10110000000000000000000000010000

		hash &= ~highbyte;
		//          ~highbyte = 01001111111111111111111111111111
		// after AND:    hash = 00000000000000000000000000010000

		str++;
	}

	return hash;
}

/**
 * Just a hash wrapper function, this way we can easily exchange the default
 * hash algorith.
 */
unsigned int string_hash(void *s) {
	return elfhash(s);
}
