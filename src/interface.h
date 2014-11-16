/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

#ifndef __INTERFACE_H
#define __INTERFACE_H

#include <stdarg.h>
#include <signal.h>

#include <glib.h>

#include <Scintilla.h>

#include "undo.h"

namespace SciTECO {

/* avoid include dependency conflict */
class QRegister;
class Buffer;
extern sig_atomic_t sigint_occurred;

class View {
	class UndoTokenMessage : public UndoToken {
		View *view;

		unsigned int iMessage;
		uptr_t wParam;
		sptr_t lParam;

	public:
		UndoTokenMessage(View *_view, unsigned int _iMessage,
				 uptr_t _wParam = 0, sptr_t _lParam = 0)
				: UndoToken(), view(_view),
		                  iMessage(_iMessage),
				  wParam(_wParam), lParam(_lParam) {}

		void
		run(void)
		{
			view->ssm(iMessage, wParam, lParam);
		}
	};

	class UndoTokenSetRepresentations : public UndoToken {
		View *view;

	public:
		UndoTokenSetRepresentations(View *_view)
		                           : view(_view) {}

		void
		run(void)
		{
			view->set_representations();
		}
	};

public:
	virtual ~View() {}

	virtual sptr_t ssm(unsigned int iMessage,
			   uptr_t wParam = 0, sptr_t lParam = 0) = 0;
	inline void
	undo_ssm(unsigned int iMessage,
		 uptr_t wParam = 0, sptr_t lParam = 0)
	{
		undo.push(new UndoTokenMessage(this, iMessage, wParam, lParam));
	}

	void set_representations(void);
	inline void
	undo_set_representations(void)
	{
		undo.push(new UndoTokenSetRepresentations(this));
	}

protected:
	void initialize(void);
};

/*
 * Base class for all user interfaces - used mereley as a class interface.
 * The actual instance of the interface has the platform-specific type
 * (e.g. InterfaceGtk) since we would like to have the benefits of using
 * classes but avoid the calling overhead when invoking virtual methods
 * on Interface pointers.
 * There's only one Interface* instance in the system.
 */
class Interface {
	class UndoTokenShowView : public UndoToken {
		View *view;

	public:
		UndoTokenShowView(View *_view)
		                 : view(_view) {}

		void run(void);
	};

	template <class Type>
	class UndoTokenInfoUpdate : public UndoToken {
		Type *obj;

	public:
		UndoTokenInfoUpdate(Type *_obj)
				   : obj(_obj) {}

		/*
		 * Implemented at bottom, so we can reference
		 * the singleton interface object.
		 * Alternative would be to do an extern explicit
		 * template instantiation.
		 */
		void run(void);
	};

public:
	virtual GOptionGroup *
	get_options(void)
	{
		return NULL;
	}
	/* expected to initialize Scintilla */
	virtual void main(int &argc, char **&argv) = 0;

	enum MessageType {
		MSG_USER,
		MSG_INFO,
		MSG_WARNING,
		MSG_ERROR
	};
	virtual void vmsg(MessageType type, const gchar *fmt, va_list ap) = 0;
	inline void
	msg(MessageType type, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4)
	{
		va_list ap;

		va_start(ap, fmt);
		vmsg(type, fmt, ap);
		va_end(ap);
	}
	virtual void msg_clear(void) {}

	virtual void show_view(View *view) = 0;
	inline void
	undo_show_view(View *view)
	{
		undo.push(new UndoTokenShowView(view));
	}
	virtual View *get_current_view(void) = 0;

	virtual sptr_t ssm(unsigned int iMessage,
			   uptr_t wParam = 0, sptr_t lParam = 0) = 0;
	virtual void undo_ssm(unsigned int iMessage,
	                      uptr_t wParam = 0, sptr_t lParam = 0) = 0;

	virtual void info_update(QRegister *reg) = 0;
	virtual void info_update(Buffer *buffer) = 0;

	template <class Type>
	inline void
	undo_info_update(Type *obj)
	{
		undo.push(new UndoTokenInfoUpdate<Type>(obj));
	}

	/* NULL means to redraw the current cmdline if necessary */
	virtual void cmdline_update(const gchar *cmdline = NULL) = 0;

	enum PopupEntryType {
		POPUP_PLAIN,
		POPUP_FILE,
		POPUP_DIRECTORY
	};
	virtual void popup_add(PopupEntryType type,
			       const gchar *name, bool highlight = false) = 0;
	virtual void popup_show(void) = 0;
	virtual void popup_clear(void) = 0;

	virtual inline bool
	is_interrupted(void)
	{
		return sigint_occurred != FALSE;
	}

	/* main entry point */
	virtual void event_loop(void) = 0;

	/*
	 * Interfacing to the external SciTECO world
	 * See main.cpp
	 */
protected:
	void stdio_vmsg(MessageType type, const gchar *fmt, va_list ap);
public:
	void process_notify(SCNotification *notify);
};

} /* namespace SciTECO */

#ifdef INTERFACE_GTK
#include "interface-gtk.h"
#elif defined(INTERFACE_NCURSES)
#include "interface-ncurses.h"
#else
#error No interface selected!
#endif

namespace SciTECO {

/* object defined in main.cpp */
extern InterfaceCurrent interface;

template <class Type>
void
Interface::UndoTokenInfoUpdate<Type>::run(void)
{
	interface.info_update(obj);
}

} /* namespace SciTECO */

#endif
