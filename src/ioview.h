/*
 * Copyright (C) 2012-2017 Robin Haberkorn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __IOVIEW_H
#define __IOVIEW_H

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"

namespace SciTECO {

/*
 * Auxiliary functions
 */

gchar *expand_path(const gchar *path);

/**
 * Get absolute/full version of a possibly relative path.
 * The path is tried to be canonicalized so it does
 * not contain relative components.
 * Works with existing and non-existing paths (in the latter case,
 * heuristics may be applied).
 * Depending on platform and existence of the path,
 * canonicalization might fail, but the path returned is
 * always absolute.
 *
 * @param path Possibly relative path name.
 * @return Newly-allocated absolute path name.
 */
gchar *get_absolute_path(const gchar *path);

/**
 * Normalize path or file name.
 *
 * This changes the directory separators
 * to forward slash (on platforms that support
 * different directory separator styles).
 *
 * @param path The path to normalize.
 *             It is changed in place.
 * @return Returns `path`. The return value
 *         may be ignored.
 */
static inline gchar *
normalize_path(gchar *path)
{
#if G_DIR_SEPARATOR != '/'
	return g_strdelimit(path, G_DIR_SEPARATOR_S, '/');
#else
	return path;
#endif
}

bool file_is_visible(const gchar *path);

/**
 * This gets the length of a file name's directory
 * component including any trailing directory separator.
 * It returns 0 if the file name does not have a directory
 * separator.
 * This is useful when constructing file names in the same
 * directory as an existing one, keeping the exact same
 * directory component (globbing, tab completion...).
 * Also if it returns non-0, this can be used to look up
 * the last used directory separator in the file name.
 */
static inline gsize
file_get_dirname_len(const gchar *path)
{
	gsize len = 0;

	for (const gchar *p = path; *p; p++)
		if (G_IS_DIR_SEPARATOR(*p))
			len = p - path + 1;

	return len;
}

class IOView : public ViewCurrent {
	class UndoTokenRemoveFile : public UndoToken {
		gchar *filename;

	public:
		UndoTokenRemoveFile(const gchar *_filename)
				   : filename(g_strdup(_filename)) {}
		~UndoTokenRemoveFile()
		{
			g_free(filename);
		}

		void
		run(void)
		{
			g_unlink(filename);
		}
	};

public:
	void load(GIOChannel *channel);
	void load(const gchar *filename);

	void save(GIOChannel *channel);
	void save(const gchar *filename);
};

} /* namespace SciTECO */

#endif
