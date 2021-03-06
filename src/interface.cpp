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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>
#include <SciLexer.h>

#include "sciteco.h"
#include "interface.h"

namespace SciTECO {

template <class ViewImpl>
void
View<ViewImpl>::set_representations(void)
{
	static const char *reps[] = {
		"^@", "^A", "^B", "^C", "^D", "^E", "^F", "^G",
		"^H", "TAB" /* ^I */, "LF" /* ^J */, "^K", "^L", "CR" /* ^M */, "^N", "^O",
		"^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W",
		"^X", "^Y", "^Z", "$" /* ^[ */, "^\\", "^]", "^^", "^_"
	};

	for (guint cc = 0; cc < G_N_ELEMENTS(reps); cc++) {
		gchar buf[] = {(gchar)cc, '\0'};
		ssm(SCI_SETREPRESENTATION, (uptr_t)buf, (sptr_t)reps[cc]);
	}
}

template <class ViewImpl>
void
View<ViewImpl>::setup(void)
{
	/*
	 * Start with or without undo collection,
	 * depending on undo.enabled.
	 */
	ssm(SCI_SETUNDOCOLLECTION, undo.enabled);

	ssm(SCI_SETFOCUS, TRUE);

	/*
	 * Some Scintilla implementations show the horizontal
	 * scroll bar by default.
	 * Ensure it is never displayed by default.
	 */
	ssm(SCI_SETHSCROLLBAR, FALSE);

	/*
	 * Only margin 1 is given a width by default.
	 * To provide a minimalist default view, it is disabled.
	 */
	ssm(SCI_SETMARGINWIDTHN, 1, 0);

	/*
	 * Set some basic styles in order to provide
	 * a consistent look across UIs if no profile
	 * is used. This makes writing UI-agnostic profiles
	 * and color schemes easier.
	 * FIXME: Some settings like fonts should probably
	 * be set per UI (i.e. Scinterm doesn't use it,
	 * GTK might try to use a system-wide default
	 * monospaced font).
	 */
	ssm(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
	ssm(SCI_SETCARETPERIOD, 0);
	ssm(SCI_SETCARETFORE, 0xFFFFFF);

	ssm(SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	ssm(SCI_STYLESETBACK, STYLE_DEFAULT, 0x000000);
	ssm(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	ssm(SCI_STYLECLEARALL);

	/*
	 * FIXME: The line number background is apparently not
	 * affected by SCI_STYLECLEARALL
	 */
	ssm(SCI_STYLESETBACK, STYLE_LINENUMBER, 0x000000);

	/*
	 * Use white as the default background color
	 * for call tips. Necessary since this style is also
	 * used for popup windows and we need to provide a sane
	 * default if no color-scheme is applied (and --no-profile).
	 */
	ssm(SCI_STYLESETFORE, STYLE_CALLTIP, 0x000000);
	ssm(SCI_STYLESETBACK, STYLE_CALLTIP, 0xFFFFFF);
}

template class View<ViewCurrent>;

template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::UndoTokenShowView::run(void)
{
	/*
	 * Implementing this here allows us to reference
	 * `interface`
	 */
	interface.show_view(view);
}

template <class InterfaceImpl, class ViewImpl>
template <class Type>
void
Interface<InterfaceImpl, ViewImpl>::UndoTokenInfoUpdate<Type>::run(void)
{
	interface.info_update(obj);
}

/**
 * Print a message to the appropriate stdio streams.
 *
 * This method has similar semantics to `vprintf`, i.e.
 * it leaves `ap` undefined. Therefore to pass the format
 * string and arguments to another `vprintf`-like function,
 * you have to copy the arguments via `va_copy`.
 */
template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::stdio_vmsg(MessageType type, const gchar *fmt, va_list ap)
{
	FILE *stream = stdout;

	switch (type) {
	case MSG_USER:
		break;
	case MSG_INFO:
		fputs("Info: ", stream);
		break;
	case MSG_WARNING:
		stream = stderr;
		fputs("Warning: ", stream);
		break;
	case MSG_ERROR:
		stream = stderr;
		fputs("Error: ", stream);
		break;
	}

	g_vfprintf(stream, fmt, ap);
	fputc('\n', stream);
}

template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::process_notify(SCNotification *notify)
{
#ifdef DEBUG
	g_printf("SCINTILLA NOTIFY: code=%d\n", notify->nmhdr.code);
#endif
}

template class Interface<InterfaceCurrent, ViewCurrent>;

} /* namespace SciTECO */
