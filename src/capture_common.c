/*
 * capture_common.c - Mostly code to handle excluding paths from capture.
 */

/*
 * Copyright (C) 2013, 2014 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/capture.h"
#include "wimlib/dentry.h"
#include "wimlib/error.h"
#include "wimlib/lookup_table.h"
#include "wimlib/paths.h"
#include "wimlib/progress.h"
#include "wimlib/textfile.h"
#include "wimlib/wildcard.h"

#include <string.h>

/*
 * Tally a file (or directory) that has been scanned for a capture operation,
 * and possibly call the progress function provided by the library user.
 *
 * @params
 *	Flags, optional progress function, and progress data for the capture
 *	operation.
 * @status
 *	Status of the scanned file.
 * @inode
 *	If @status is WIMLIB_SCAN_DENTRY_OK, this is a pointer to the WIM inode
 *	that has been created for the scanned file.  The first time the file is
 *	seen, inode->i_nlink will be 1.  On subsequent visits of the same inode
 *	via additional hard links, inode->i_nlink will be greater than 1.
 */
int
do_capture_progress(struct add_image_params *params, int status,
		    const struct wim_inode *inode)
{
	switch (status) {
	case WIMLIB_SCAN_DENTRY_OK:
		if (!(params->add_flags & WIMLIB_ADD_FLAG_VERBOSE))
			return 0;
	case WIMLIB_SCAN_DENTRY_UNSUPPORTED:
	case WIMLIB_SCAN_DENTRY_EXCLUDED:
	case WIMLIB_SCAN_DENTRY_FIXED_SYMLINK:
	case WIMLIB_SCAN_DENTRY_NOT_FIXED_SYMLINK:
		if (!(params->add_flags & WIMLIB_ADD_FLAG_EXCLUDE_VERBOSE))
			return 0;
	}
	params->progress.scan.status = status;
	if (status == WIMLIB_SCAN_DENTRY_OK && inode->i_nlink == 1) {

		/* Successful scan, and visiting inode for the first time  */

		/* Tally size of all data streams.  */
		const struct wim_lookup_table_entry *lte;
		for (unsigned i = 0; i <= inode->i_num_ads; i++) {
			lte = inode_stream_lte_resolved(inode, i);
			if (lte)
				params->progress.scan.num_bytes_scanned += lte->size;
		}

		/* Tally the file itself.  */
		if (inode->i_attributes & FILE_ATTRIBUTE_DIRECTORY)
			params->progress.scan.num_dirs_scanned++;
		else
			params->progress.scan.num_nondirs_scanned++;
	}

	/* Call the user-provided progress function.  */
	return call_progress(params->progfunc, WIMLIB_PROGRESS_MSG_SCAN_DENTRY,
			     &params->progress, params->progctx);
}

/*
 * Given a null-terminated pathname pattern @pat that has been read from line
 * @line_no of the file @path, validate and canonicalize the pattern.
 *
 * On success, returns 0.
 * On failure, returns WIMLIB_ERR_INVALID_CAPTURE_CONFIG.
 * In either case, @pat may have been modified in-place (and possibly
 * shortened).
 */
int
mangle_pat(tchar *pat, const tchar *path, unsigned long line_no)
{
	if (!is_any_path_separator(pat[0]) &&
	    pat[0] != T('\0') && pat[1] == T(':'))
	{
		/* Pattern begins with drive letter.  */

		if (!is_any_path_separator(pat[2])) {
			/* Something like c:file, which is actually a path
			 * relative to the current working directory on the c:
			 * drive.  We require paths with drive letters to be
			 * absolute.  */
			ERROR("%"TS":%lu: Invalid pattern \"%"TS"\":\n"
			      "        Patterns including drive letters must be absolute!\n"
			      "        Maybe try \"%"TC":%"TC"%"TS"\"?\n",
			      path, line_no, pat,
			      pat[0], OS_PREFERRED_PATH_SEPARATOR, &pat[2]);
			return WIMLIB_ERR_INVALID_CAPTURE_CONFIG;
		}

		WARNING("%"TS":%lu: Pattern \"%"TS"\" starts with a drive "
			"letter, which is being removed.",
			path, line_no, pat);

		/* Strip the drive letter.  */
		tmemmove(pat, pat + 2, tstrlen(pat + 2) + 1);
	}

	/* Collapse consecutive path separators, and translate both / and \ into
	 * / (UNIX) or \ (Windows).
	 *
	 * Note: we expect that this function produces patterns that can be used
	 * for both filesystem paths and WIM paths, so the desired path
	 * separators must be the same.  */
	BUILD_BUG_ON(OS_PREFERRED_PATH_SEPARATOR != WIM_PATH_SEPARATOR);
	do_canonicalize_path(pat, pat);

	/* Relative patterns can only match file names, so they must be
	 * single-component only.  */
	if (pat[0] != OS_PREFERRED_PATH_SEPARATOR &&
	    tstrchr(pat, OS_PREFERRED_PATH_SEPARATOR))
	{
		ERROR("%"TS":%lu: Invalid pattern \"%"TS"\":\n"
		      "        Relative patterns can only include one path component!\n"
		      "        Maybe try \"%"TC"%"TS"\"?",
		      path, line_no, pat, OS_PREFERRED_PATH_SEPARATOR, pat);
		return WIMLIB_ERR_INVALID_CAPTURE_CONFIG;
	}

	return 0;
}

/*
 * Read, parse, and validate a capture configuration file from either an on-disk
 * file or an in-memory buffer.
 *
 * To read from a file, specify @config_file, and use NULL for @buf.
 * To read from a buffer, specify @buf and @bufsize.
 *
 * @config must be initialized to all 0's.
 *
 * On success, 0 will be returned, and the resulting capture configuration will
 * be stored in @config.
 *
 * On failure, a positive error code will be returned, and the contents of
 * @config will be invalidated.
 */
int
read_capture_config(const tchar *config_file, const void *buf,
		    size_t bufsize, struct capture_config *config)
{
	int ret;

	/* [PrepopulateList] is used for apply, not capture.  But since we do
	 * understand it, recognize it, thereby avoiding the unrecognized
	 * section warning, but discard the resulting strings.  */
	STRING_SET(prepopulate_pats);

	struct text_file_section sections[] = {
		{T("ExclusionList"),
			&config->exclusion_pats},
		{T("ExclusionException"),
			&config->exclusion_exception_pats},
		{T("PrepopulateList"),
			&prepopulate_pats},
	};
	void *mem;

	ret = do_load_text_file(config_file, buf, bufsize, &mem,
				sections, ARRAY_LEN(sections),
				LOAD_TEXT_FILE_REMOVE_QUOTES, mangle_pat);
	if (ret)
		return ret;

	FREE(prepopulate_pats.strings);

	config->buf = mem;
	return 0;
}

void
destroy_capture_config(struct capture_config *config)
{
	FREE(config->exclusion_pats.strings);
	FREE(config->exclusion_exception_pats.strings);
	FREE(config->buf);
}

/*
 * Determine whether a path matches any wildcard pattern in a list.
 *
 * Special rules apply about what form @path must be in; see match_path().
 */
bool
match_pattern_list(const tchar *path, size_t path_nchars,
		   const struct string_set *list)
{
	for (size_t i = 0; i < list->num_strings; i++)
		if (match_path(path, path_nchars, list->strings[i],
			       OS_PREFERRED_PATH_SEPARATOR, true))
			return true;
	return false;
}

/*
 * Determine whether the filesystem @path should be excluded from capture, based
 * on the current capture configuration file.
 *
 * The @path must be given relative to the root of the capture, but with a
 * leading path separator.  For example, if the file "in/file" is being tested
 * and the library user ran wimlib_add_image(wim, "in", ...), then the directory
 * "in" is the root of the capture and the path should be specified as "/file".
 *
 * Also, all path separators in @path must be OS_PREFERRED_PATH_SEPARATOR, there
 * cannot be trailing slashes, and there cannot be consecutive path separators.
 *
 * As a special case, the empty string will be interpreted as a single path
 * separator (which means the root of capture itself).
 */
bool
should_exclude_path(const tchar *path, size_t path_nchars,
		    const struct capture_config *config)
{
	tchar dummy[2];

	if (!config)
		return false;

	if (!*path) {
		dummy[0] = OS_PREFERRED_PATH_SEPARATOR;
		dummy[1] = T('\0');
		path = dummy;
		path_nchars = 1;
	}

	return match_pattern_list(path, path_nchars, &config->exclusion_pats) &&
	      !match_pattern_list(path, path_nchars, &config->exclusion_exception_pats);

}
