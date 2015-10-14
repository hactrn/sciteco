/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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

#ifndef __INTERFACE_CURSES_H
#define __INTERFACE_CURSES_H

#include <stdarg.h>

#include <glib.h>

#include <curses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "interface.h"

namespace SciTECO {

typedef class ViewCurses : public View<ViewCurses> {
	Scintilla *sci;

public:
	ViewCurses() : sci(NULL) {}

	/* implementation of View::initialize() */
	void initialize_impl(void);

	inline ~ViewCurses()
	{
		/*
		 * NOTE: This deletes/frees the view's
		 * curses WINDOW, despite of what old versions
		 * of the Scinterm documentation claim.
		 */
		if (sci)
			scintilla_delete(sci);
	}

	inline void
	noutrefresh(void)
	{
		scintilla_noutrefresh(sci);
	}

	inline void
	refresh(void)
	{
		scintilla_refresh(sci);
	}

	inline WINDOW *
	get_window(void)
	{
		return scintilla_get_window(sci);
	}

	/* implementation of View::ssm() */
	inline sptr_t
	ssm_impl(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(sci, iMessage, wParam, lParam);
	}
} ViewCurrent;

typedef class InterfaceCurses : public Interface<InterfaceCurses, ViewCurses> {
	/**
	 * Mapping of the first 16 curses color codes (that may or may not
	 * correspond with the standard terminal color codes) to
	 * Scintilla-compatible RGB values (red is LSB) to initialize after
	 * Curses startup.
	 * Negative values mean no color redefinition (keep the original
	 * palette entry).
	 */
	gint32 color_table[16];

	/**
	 * Mapping of the first 16 curses color codes to their
	 * original values for restoring them on shutdown.
	 * Unfortunately, this may not be supported on all
	 * curses ports, so this array may be unused.
	 */
	struct {
		short r, g, b;
	} orig_color_table[16];

	int stdout_orig, stderr_orig;
	SCREEN *screen;
	FILE *screen_tty;

	WINDOW *info_window;
	enum {
		INFO_TYPE_BUFFER = 0,
		INFO_TYPE_QREGISTER
	} info_type;
	gchar *info_current;

	WINDOW *msg_window;

	WINDOW *cmdline_window, *cmdline_pad;
	gsize cmdline_len, cmdline_rubout_len;

	class Popup {
		WINDOW *window;		/**! window showing part of pad */
		WINDOW *pad;		/**! full-height entry list */

		struct Entry {
			PopupEntryType type;
			bool highlight;
			gchar name[];
		};

		GSList *list;		/**! list of popup entries */
		gint longest;		/**! size of longest entry */
		gint length;		/**! total number of popup entries */

		gint pad_first_line;	/**! first line in pad to show */

	public:
		Popup() : window(NULL), pad(NULL),
		          list(NULL), longest(0), length(0),
		          pad_first_line(0) {}

		void add(PopupEntryType type,
			 const gchar *name, bool highlight = false);

		void show(attr_t attr);
		inline bool
		is_shown(void)
		{
			return window != NULL;
		}

		void clear(void);

		inline void
		noutrefresh(void)
		{
			if (window)
				wnoutrefresh(window);
		}

		~Popup();

	private:
		void init_pad(attr_t attr);
	} popup;

public:
	InterfaceCurses();
	~InterfaceCurses();

	/* implementation of Interface::main() */
	void main_impl(int &argc, char **&argv);

	/* override of Interface::init_color() */
	void init_color(guint color, guint32 rgb);

	/* implementation of Interface::vmsg() */
	void vmsg_impl(MessageType type, const gchar *fmt, va_list ap);
	/* override of Interface::msg_clear() */
	void msg_clear(void);

	/* implementation of Interface::show_view() */
	void show_view_impl(ViewCurses *view);

	/* implementation of Interface::info_update() */
	void info_update_impl(const QRegister *reg);
	void info_update_impl(const Buffer *buffer);

	/* implementation of Interface::cmdline_update() */
	void cmdline_update_impl(const Cmdline *cmdline);

	/* implementation of Interface::popup_add() */
	inline void
	popup_add_impl(PopupEntryType type,
	               const gchar *name, bool highlight = false)
	{
		if (cmdline_window)
			/* interactive mode */
			popup.add(type, name, highlight);
	}

	/* implementation of Interface::popup_show() */
	void popup_show_impl(void);
	/* implementation of Interface::popup_is_shown() */
	inline bool
	popup_is_shown_impl(void)
	{
		return popup.is_shown();
	}

	/* implementation of Interface::popup_clear() */
	void popup_clear_impl(void);

	/* main entry point (implementation) */
	void event_loop_impl(void);

private:
	void init_color_safe(guint color, guint32 rgb);
	void restore_colors(void);

	void init_screen(void);
	void init_interactive(void);
	void restore_batch(void);

	void resize_all_windows(void);

	void set_window_title(const gchar *title);
	void draw_info(void);
	void draw_cmdline(void);

	friend void event_loop_iter();
} InterfaceCurrent;

} /* namespace SciTECO */

#endif
