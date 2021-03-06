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

#include <string.h>
#include <exception>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "memory.h"
#include "string-utils.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "goto.h"
#include "qregisters.h"
#include "ring.h"
#include "parser.h"
#include "symbols.h"
#include "search.h"
#include "spawn.h"
#include "glob.h"
#include "help.h"
#include "cmdline.h"
#include "ioview.h"
#include "error.h"

namespace SciTECO {

//#define DEBUG

gint macro_pc = 0;

namespace States {
	StateStart		start;
	StateControl		control;
	StateASCII		ascii;
	StateEscape		escape;
	StateFCommand		fcommand;
	StateChangeDir		changedir;
	StateCondCommand	condcommand;
	StateECommand		ecommand;
	StateScintilla_symbols	scintilla_symbols;
	StateScintilla_lParam	scintilla_lparam;
	StateInsert		insert_building(true);
	StateInsert		insert_nobuilding(false);
	StateInsertIndent	insert_indent;

	State *current = &start;
}

namespace Modifiers {
	static bool colon = false;
	static bool at = false;
}

enum Mode mode = MODE_NORMAL;

/* FIXME: perhaps integrate into Mode */
static bool skip_else = false;

static gint nest_level = 0;

gchar *strings[2] = {NULL, NULL};
gchar escape_char = CTL_KEY_ESC;

LoopStack loop_stack;

/**
 * Loop frame pointer: The number of elements on
 * the loop stack when a macro invocation frame is
 * created.
 * This is used to perform checks for flow control
 * commands to avoid jumping with invalid PCs while
 * not creating a new stack per macro frame.
 */
static guint loop_stack_fp = 0;

/**
 * Handles all expected exceptions, converting them to
 * SciTECO::Error and preparing them for stack frame insertion.
 * This method will only throw SciTECO::Error and
 * SciTECO::Cmdline *.
 */
void
Execute::step(const gchar *macro, gint stop_pos)
{
	try {
		/*
		 * Convert bad_alloc and other C++ standard
		 * library exceptions.
		 * bad_alloc should no longer be thrown, though
		 * since new/delete uses Glib allocations and we
		 * uniformly terminate abnormally in case of OOM.
		 */
		try {
			while (macro_pc < stop_pos) {
#ifdef DEBUG
				g_printf("EXEC(%d): input='%c'/%x, state=%p, mode=%d\n",
					 macro_pc, macro[macro_pc], macro[macro_pc],
					 States::current, mode);
#endif

				if (interface.is_interrupted())
					throw Error("Interrupted");

				memlimit.check();

				State::input(macro[macro_pc]);
				macro_pc++;
			}

			/*
			 * Provide interactive feedback when the
			 * PC is at the end of the command line.
			 * This will actually be called in other situations,
			 * like at the end of macros but that does not hurt.
			 * It should perhaps be in Cmdline::insert(),
			 * but doing it here ensures that exceptions get
			 * normalized.
			 */
			States::current->refresh();
		} catch (std::exception &error) {
			throw StdError(error);
		}
	} catch (Error &error) {
		error.set_coord(macro, macro_pc);
		throw; /* forward */
	}
}

/*
 * may throw non SciTECO::Error exceptions which are not to be
 * associated with the macro invocation stack frame
 */
void
Execute::macro(const gchar *macro, bool locals)
{
	GotoTable *parent_goto_table = Goto::table;
	GotoTable macro_goto_table(false);

	QRegisterTable *parent_locals = QRegisters::locals;
	/*
	 * NOTE: A local QReg table is not required
	 * for local macro calls (:M).
	 * However allocating it on the stack on-demand is
	 * tricky (VLAs are not in standard C++ and alloca()
	 * is buggy on MSCVRT), so we always reserve a
	 * local Q-Reg table.
	 * This is OK since the table object itself is very
	 * small and it's empty by default.
	 * Best would be to let Execute::macro() be a wrapper
	 * around something like Execute::local_macro() which
	 * cares about local Q-Reg allocation, but the special
	 * handling of currently-edited local Q-Regs below
	 * prevents this.
	 */
	QRegisterTable macro_locals(false);

	State *parent_state = States::current;
	gint parent_pc = macro_pc;
	guint parent_loop_fp = loop_stack_fp;

	guint parent_brace_level = expressions.brace_level;

	/*
	 * need this to fixup state on rubout: state machine emits undo token
	 * resetting state to parent's one, but the macro executed also emitted
	 * undo tokens resetting the state to StateStart
	 */
	undo.push_var(States::current) = &States::start;
	macro_pc = 0;
	loop_stack_fp = loop_stack.items();

	Goto::table = &macro_goto_table;

	/*
	 * Locals are only initialized when needed to
	 * improve the speed of local macro calls.
	 */
	if (locals) {
		macro_locals.insert_defaults();
		QRegisters::locals = &macro_locals;
	}

	try {
		try {
			step(macro, strlen(macro));
		} catch (Return &info) {
			/*
			 * Macro returned - handle like regular
			 * end of macro, even though some checks
			 * are unnecessary here.
			 * macro_pc will still point to the return PC.
			 */
			g_assert(States::current == &States::start);

			/*
			 * Discard all braces, except the current one.
			 */
			expressions.brace_return(parent_brace_level, info.args);

			/*
			 * Clean up the loop stack.
			 * We are allowed to return in loops.
			 * NOTE: This does not have to be undone.
			 */
			loop_stack.clear(loop_stack_fp);
		}

		if (G_UNLIKELY(loop_stack.items() > loop_stack_fp)) {
			Error error("Unterminated loop");
			error.set_coord(macro, loop_stack.peek().pc);
			throw error;
		}

		/*
		 * Subsequent errors must still be
		 * attached to this macro invocation
		 * via Error::set_coord()
		 */
		try {
			if (G_UNLIKELY(Goto::skip_label))
				throw Error("Label \"%s\" not found",
				            Goto::skip_label);

			/*
			 * Some states (esp. commands involving a
			 * "lookahead") are valid at the end of a macro.
			 */
			States::current->end_of_macro();

			/*
			 * This handles the problem of Q-Registers
			 * local to the macro invocation being edited
			 * when the macro terminates.
			 * QRegisterTable::clear() throws an error
			 * if this happens and the Q-Reg editing
			 * is undone.
			 */
			if (locals)
				QRegisters::locals->clear();
		} catch (Error &error) {
			error.set_coord(macro, macro_pc);
			throw; /* forward */
		}
	} catch (...) {
		g_free(Goto::skip_label);
		Goto::skip_label = NULL;

		QRegisters::locals = parent_locals;
		Goto::table = parent_goto_table;

		loop_stack_fp = parent_loop_fp;
		macro_pc = parent_pc;
		States::current = parent_state;

		throw; /* forward */
	}

	QRegisters::locals = parent_locals;
	Goto::table = parent_goto_table;

	loop_stack_fp = parent_loop_fp;
	macro_pc = parent_pc;
	States::current = parent_state;
}

void
Execute::file(const gchar *filename, bool locals)
{
	GError *gerror = NULL;
	gchar *macro_str, *p;

	if (!g_file_get_contents(filename, &macro_str, NULL, &gerror))
		throw GlibError(gerror);

	/* only when executing files, ignore Hash-Bang line */
	if (*macro_str == '#') {
		p = strpbrk(macro_str, "\r\n");
		if (G_UNLIKELY(!p))
			/* empty script */
			goto cleanup;
		p++;
	} else {
		p = macro_str;
	}

	try {
		macro(p, locals);
	} catch (Error &error) {
		error.pos += p - macro_str;
		if (*macro_str == '#')
			error.line++;
		error.add_frame(new Error::FileFrame(filename));

		g_free(macro_str);
		throw; /* forward */
	} catch (...) {
		g_free(macro_str);
		throw; /* forward */
	}

cleanup:
	g_free(macro_str);
}

State::State()
{
	for (guint i = 0; i < G_N_ELEMENTS(transitions); i++)
		transitions[i] = NULL;
}

bool
State::eval_colon(void)
{
	if (!Modifiers::colon)
		return false;

	undo.push_var<bool>(Modifiers::colon);
	Modifiers::colon = false;
	return true;
}

void
State::input(gchar chr)
{
	State *state = States::current;

	for (;;) {
		State *next = state->get_next_state(chr);

		if (next == state)
			break;

		state = next;
		chr = '\0';
	}

	if (state != States::current) {
		undo.push_var<State *>(States::current);
		States::current = state;
	}
}

State *
State::get_next_state(gchar chr)
{
	State *next = NULL;
	guint upper = String::toupper(chr);

	if (upper < G_N_ELEMENTS(transitions))
		next = transitions[upper];
	if (!next)
		next = custom(chr);
	if (!next)
		throw SyntaxError(chr);

	return next;
}

void
StringBuildingMachine::reset(void)
{
	MicroStateMachine<gchar *>::reset();
	undo.push_obj(qregspec_machine) = NULL;
	undo.push_var(mode) = MODE_NORMAL;
	undo.push_var(toctl) = false;
}

bool
StringBuildingMachine::input(gchar chr, gchar *&result)
{
	QRegister *reg;
	gchar *str;

	switch (mode) {
	case MODE_UPPER:
		chr = g_ascii_toupper(chr);
		break;
	case MODE_LOWER:
		chr = g_ascii_tolower(chr);
		break;
	default:
		break;
	}

	if (toctl) {
		if (chr != '^')
			chr = CTL_KEY(String::toupper(chr));
		undo.push_var(toctl) = false;
	} else if (chr == '^') {
		undo.push_var(toctl) = true;
		return false;
	}

MICROSTATE_START;
	switch (chr) {
	case CTL_KEY('Q'):
	case CTL_KEY('R'): set(&&StateEscaped); break;
	case CTL_KEY('V'): set(&&StateLower); break;
	case CTL_KEY('W'): set(&&StateUpper); break;
	case CTL_KEY('E'): set(&&StateCtlE); break;
	default:
		goto StateEscaped;
	}

	return false;

StateLower:
	set(StateStart);

	if (chr != CTL_KEY('V')) {
		result = String::chrdup(g_ascii_tolower(chr));
		return true;
	}

	undo.push_var(mode) = MODE_LOWER;
	return false;

StateUpper:
	set(StateStart);

	if (chr != CTL_KEY('W')) {
		result = String::chrdup(g_ascii_toupper(chr));
		return true;
	}

	undo.push_var(mode) = MODE_UPPER;
	return false;

StateCtlE:
	switch (String::toupper(chr)) {
	case '\\':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlENum);
		break;
	case 'U':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEU);
		break;
	case 'Q':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEQ);
		break;
	case '@':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEQuote);
		break;
	case 'N':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEN);
		break;
	default:
		result = (gchar *)g_malloc(3);

		set(StateStart);
		result[0] = CTL_KEY('E');
		result[1] = chr;
		result[2] = '\0';
		return true;
	}

	return false;

StateCtlENum:
	if (!qregspec_machine->input(chr, reg))
		return false;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	result = g_strdup(expressions.format(reg->get_integer()));
	return true;

StateCtlEU:
	if (!qregspec_machine->input(chr, reg))
		return false;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	result = String::chrdup((gchar)reg->get_integer());
	return true;

StateCtlEQ:
	if (!qregspec_machine->input(chr, reg))
		return false;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	result = reg->get_string();
	return true;

StateCtlEQuote:
	if (!qregspec_machine->input(chr, reg))
		return false;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	str = reg->get_string();
	result = g_shell_quote(str);
	g_free(str);
	return true;

StateCtlEN:
	if (!qregspec_machine->input(chr, reg))
		return false;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	str = reg->get_string();
	result = Globber::escape_pattern(str);
	g_free(str);
	return true;

StateEscaped:
	set(StateStart);
	result = String::chrdup(chr);
	return true;
}

StringBuildingMachine::~StringBuildingMachine()
{
	delete qregspec_machine;
}

State *
StateExpectString::custom(gchar chr)
{
	if (chr == '\0') {
		BEGIN_EXEC(this);
		initial();
		return this;
	}

	/*
	 * String termination handling
	 */
	if (Modifiers::at) {
		if (last)
			undo.push_var(Modifiers::at) = false;

		switch (escape_char) {
		case CTL_KEY_ESC:
		case '{':
			undo.push_var(escape_char) = String::toupper(chr);
			return this;
		}
	}

	if (escape_char == '{') {
		switch (chr) {
		case '{':
			undo.push_var(nesting)++;
			break;
		case '}':
			undo.push_var(nesting)--;
			break;
		}
	} else if (String::toupper(chr) == escape_char) {
		undo.push_var(nesting)--;
	}

	if (!nesting) {
		State *next;
		gchar *string = strings[0];

		undo.push_str(strings[0]) = NULL;
		if (last)
			undo.push_var(escape_char) = CTL_KEY_ESC;
		nesting = 1;

		if (string_building)
			machine.reset();

		try {
			/*
			 * Call process() even if interactive feedback
			 * has not been requested using refresh().
			 * This is necessary since commands are either
			 * written for interactive execution or not,
			 * so they may do their main activity in process().
			 */
			if (insert_len)
				process(string ? : "", insert_len);
			next = done(string ? : "");
		} catch (...) {
			g_free(string);
			throw;
		}

		g_free(string);
		insert_len = 0;
		return next;
	}

	BEGIN_EXEC(this);

	/*
	 * String building characters and
	 * string argument accumulation.
	 *
	 * NOTE: As an optimization insert_len is not
	 * restored on undo since that is only
	 * necessary in interactive mode and we get
	 * called once per character when this is necessary.
	 * If this gets too confusing, just undo changes
	 * to insert_len.
	 */
	if (string_building) {
		gchar *insert;

		if (!machine.input(chr, insert))
			return this;

		undo.push_str(strings[0]);
		String::append(strings[0], insert);
		insert_len += strlen(insert);

		g_free(insert);
	} else {
		undo.push_str(strings[0]);
		String::append(strings[0], chr);
		insert_len++;
	}

	return this;
}

void
StateExpectString::refresh(void)
{
	/* never calls process() in parse-only mode */
	if (insert_len)
		process(strings[0], insert_len);
	insert_len = 0;
}

State *
StateExpectFile::done(const gchar *str)
{
	gchar *filename = expand_path(str);
	State *next;

	try {
		next = got_file(filename);
	} catch (...) {
		g_free(filename);
		throw;
	}

	g_free(filename);
	return next;
}

StateStart::StateStart()
{
	transitions['\0'] = this;
	init(" \f\r\n\v");

	transitions['$'] = &States::escape;
	transitions['!'] = &States::label;
	transitions['O'] = &States::gotocmd;
	transitions['^'] = &States::control;
	transitions['F'] = &States::fcommand;
	transitions['"'] = &States::condcommand;
	transitions['E'] = &States::ecommand;
	transitions['I'] = &States::insert_building;
	transitions['?'] = &States::gethelp;
	transitions['S'] = &States::search;
	transitions['N'] = &States::searchall;

	transitions['['] = &States::pushqreg;
	transitions[']'] = &States::popqreg;
	transitions['G'] = &States::getqregstring;
	transitions['Q'] = &States::queryqreg;
	transitions['U'] = &States::setqreginteger;
	transitions['%'] = &States::increaseqreg;
	transitions['M'] = &States::macro;
	transitions['X'] = &States::copytoqreg;
}

void
StateStart::insert_integer(tecoInt v)
{
	const gchar *str = expressions.format(v);

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_ADDTEXT, strlen(str), (sptr_t)str);
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	if (current_doc_must_undo())
		interface.undo_ssm(SCI_UNDO);
}

tecoInt
StateStart::read_integer(void)
{
	uptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
	gchar c = (gchar)interface.ssm(SCI_GETCHARAT, pos);
	tecoInt v = 0;
	gint sign = 1;

	if (c == '-') {
		pos++;
		sign = -1;
	}

	for (;;) {
		c = String::toupper((gchar)interface.ssm(SCI_GETCHARAT, pos));
		if (c >= '0' && c <= '0' + MIN(expressions.radix, 10) - 1)
			v = (v*expressions.radix) + (c - '0');
		else if (c >= 'A' &&
			 c <= 'A' + MIN(expressions.radix - 10, 26) - 1)
			v = (v*expressions.radix) + 10 + (c - 'A');
		else
			break;

		pos++;
	}

	return sign * v;
}

tecoBool
StateStart::move_chars(tecoInt n)
{
	sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);

	if (!Validate::pos(pos + n))
		return FAILURE;

	interface.ssm(SCI_GOTOPOS, pos + n);
	if (current_doc_must_undo())
		interface.undo_ssm(SCI_GOTOPOS, pos);

	return SUCCESS;
}

tecoBool
StateStart::move_lines(tecoInt n)
{
	sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
	sptr_t line = interface.ssm(SCI_LINEFROMPOSITION, pos) + n;

	if (!Validate::line(line))
		return FAILURE;

	interface.ssm(SCI_GOTOLINE, line);
	if (current_doc_must_undo())
		interface.undo_ssm(SCI_GOTOPOS, pos);

	return SUCCESS;
}

tecoBool
StateStart::delete_words(tecoInt n)
{
	sptr_t pos, size;

	if (!n)
		return SUCCESS;

	pos = interface.ssm(SCI_GETCURRENTPOS);
	size = interface.ssm(SCI_GETLENGTH);
	interface.ssm(SCI_BEGINUNDOACTION);
	/*
	 * FIXME: would be nice to do this with constant amount of
	 * editor messages. E.g. by using custom algorithm accessing
	 * the internal document buffer.
	 */
	if (n > 0) {
		while (n--) {
			sptr_t size = interface.ssm(SCI_GETLENGTH);
			interface.ssm(SCI_DELWORDRIGHTEND);
			if (size == interface.ssm(SCI_GETLENGTH))
				break;
		}
	} else {
		n *= -1;
		while (n--) {
			sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
			//interface.ssm(SCI_DELWORDLEFTEND);
			interface.ssm(SCI_WORDLEFTEND);
			if (pos == interface.ssm(SCI_GETCURRENTPOS))
				break;
			interface.ssm(SCI_DELWORDRIGHTEND);
		}
	}
	interface.ssm(SCI_ENDUNDOACTION);

	if (n >= 0) {
		if (size != interface.ssm(SCI_GETLENGTH)) {
			interface.ssm(SCI_UNDO);
			interface.ssm(SCI_GOTOPOS, pos);
		}
		return FAILURE;
	}

	interface.undo_ssm(SCI_GOTOPOS, pos);
	if (current_doc_must_undo())
		interface.undo_ssm(SCI_UNDO);
	ring.dirtify();

	return SUCCESS;
}

State *
StateStart::custom(gchar chr)
{
	tecoInt v;
	tecoBool rc;

	/*
	 * <CTRL/x> commands implemented in StateControl
	 */
	if (IS_CTL(chr))
		return States::control.get_next_state(CTL_ECHO(chr));

	/*
	 * arithmetics
	 */
	/*$ 0 1 2 3 4 5 6 7 8 9 digit number
	 * [n]0|1|2|3|4|5|6|7|8|9 -> n*Radix+X -- Append digit
	 *
	 * Integer constants in \*(ST may be thought of and are
	 * technically sequences of single-digit commands.
	 * These commands take one argument from the stack
	 * (0 is implied), multiply it with the current radix
	 * (2, 8, 10, 16, ...), add the digit's value and
	 * return the resultant integer.
	 *
	 * The command-like semantics of digits may be abused
	 * in macros, for instance to append digits to computed
	 * integers.
	 * It is not an error to append a digit greater than the
	 * current radix - this may be changed in the future.
	 */
	if (g_ascii_isdigit(chr)) {
		BEGIN_EXEC(this);
		expressions.add_digit(chr);
		return this;
	}

	chr = String::toupper(chr);
	switch (chr) {
	case '/':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_DIV);
		break;

	case '*':
		if (cmdline.len == 1 && cmdline[0] == '*')
			/* special save last commandline command */
			return &States::save_cmdline;

		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_MUL);
		break;

	case '+':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_ADD);
		break;

	case '-':
		BEGIN_EXEC(this);
		if (!expressions.args())
			expressions.set_num_sign(-expressions.num_sign);
		else
			expressions.push_calc(Expressions::OP_SUB);
		break;

	case '&':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_AND);
		break;

	case '#':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_OR);
		break;

	case '(':
		BEGIN_EXEC(this);
		if (expressions.num_sign < 0) {
			expressions.set_num_sign(1);
			expressions.eval();
			expressions.push(-1);
			expressions.push_calc(Expressions::OP_MUL);
		}
		expressions.brace_open();
		break;

	case ')':
		BEGIN_EXEC(this);
		expressions.brace_close();
		break;

	case ',':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(Expressions::OP_NEW);
		break;

	/*$ "." dot
	 * \&. -> dot -- Return buffer position
	 *
	 * \(lq.\(rq pushes onto the stack, the current
	 * position (also called <dot>) of the currently
	 * selected buffer or Q-Register.
	 */
	case '.':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETCURRENTPOS));
		break;

	/*$ Z size
	 * Z -> size -- Return buffer size
	 *
	 * Pushes onto the stack, the size of the currently selected
	 * buffer or Q-Register.
	 * This is value is also the buffer position of the document's
	 * end.
	 */
	case 'Z':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

	/*$ H
	 * H -> 0,Z -- Return range for entire buffer
	 *
	 * Pushes onto the stack the integer 0 (position of buffer
	 * beginning) and the current buffer's size.
	 * It is thus often equivalent to the expression
	 * \(lq0,Z\(rq, or more generally \(lq(0,Z)\(rq.
	 */
	case 'H':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(0);
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

	/*$ "\\"
	 * n\\ -- Insert or read ASCII numbers
	 * \\ -> n
	 *
	 * Backslash pops a value from the stack, formats it
	 * according to the current radix and inserts it in the
	 * current buffer or Q-Register at dot.
	 * If <n> is omitted (empty stack), it does the reverse -
	 * it reads from the current buffer position an integer
	 * in the current radix and pushes it onto the stack.
	 * Dot is not changed when reading integers.
	 *
	 * In other words, the command serializes or deserializes
	 * integers as ASCII characters.
	 */
	case '\\':
		BEGIN_EXEC(this);
		expressions.eval();
		if (expressions.args())
			insert_integer(expressions.pop_num_calc());
		else
			expressions.push(read_integer());
		break;

	/*
	 * control structures (loops)
	 */
	case '<':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			undo.push_var(nest_level)++;
		} else {
			LoopContext ctx;

			BEGIN_EXEC(this);

			expressions.eval();
			ctx.pass_through = eval_colon();
			ctx.counter = expressions.pop_num_calc(0, -1);
			if (ctx.counter) {
				/*
				 * Non-colon modified, we add implicit
				 * braces, so loop body won't see parameters.
				 * Colon modified, loop starts can be used
				 * to process stack elements which is symmetric
				 * to ":>".
				 */
				if (!ctx.pass_through)
					expressions.brace_open();

				ctx.pc = macro_pc;
				loop_stack.push(ctx);
				LoopStack::undo_pop<loop_stack>();
			} else {
				/* skip to end of loop */
				undo.push_var(mode) = MODE_PARSE_ONLY_LOOP;
			}
		}
		break;

	case '>':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			if (!nest_level)
				undo.push_var(mode) = MODE_NORMAL;
			else
				undo.push_var(nest_level)--;
		} else {
			BEGIN_EXEC(this);

			if (loop_stack.items() <= loop_stack_fp)
				throw Error("Loop end without corresponding "
				            "loop start command");
			LoopContext &ctx = loop_stack.peek();
			bool colon_modified = eval_colon();

			/*
			 * Colon-modified loop ends can be used to
			 * aggregate values on the stack.
			 * A non-colon modified ">" behaves like ":>"
			 * for pass-through loop starts, though.
			 */
			if (!ctx.pass_through) {
				if (colon_modified) {
					expressions.eval();
					expressions.push(Expressions::OP_NEW);
				} else {
					expressions.discard_args();
				}
			}

			if (ctx.counter == 1) {
				/* this was the last loop iteration */
				if (!ctx.pass_through)
					expressions.brace_close();
				LoopStack::undo_push<loop_stack>(loop_stack.pop());
			} else {
				/*
				 * Repeat loop:
				 * NOTE: One undo token per iteration could
				 * be avoided by saving the original counter
				 * in the LoopContext.
				 * We do however optimize the case of infinite loops
				 * because the loop counter does not have to be
				 * updated.
				 */
				macro_pc = ctx.pc;
				if (ctx.counter >= 0)
					undo.push_var(ctx.counter) = ctx.counter - 1;
			}
		}
		break;

	/*$ ";" break
	 * [bool]; -- Conditionally break from loop
	 * [bool]:;
	 *
	 * Breaks from the current inner-most loop if <bool>
	 * signifies failure (non-negative value).
	 * If colon-modified, breaks from the loop if <bool>
	 * signifies success (negative value).
	 *
	 * If the condition code cannot be popped from the stack,
	 * the global search register's condition integer
	 * is implied instead.
	 * This way, you may break on search success/failures
	 * without colon-modifying the search command (or at a
	 * later point).
	 *
	 * Executing \(lq;\(rq outside of iterations in the current
	 * macro invocation level yields an error. It is thus not
	 * possible to let a macro break a caller's loop.
	 */
	case ';':
		BEGIN_EXEC(this);

		if (loop_stack.items() <= loop_stack_fp)
			throw Error("<;> only allowed in iterations");

		v = QRegisters::globals["_"]->get_integer();
		rc = expressions.pop_num_calc(0, v);
		if (eval_colon())
			rc = ~rc;

		if (IS_FAILURE(rc)) {
			LoopContext ctx = loop_stack.pop();

			expressions.discard_args();
			if (!ctx.pass_through)
				expressions.brace_close();

			LoopStack::undo_push<loop_stack>(ctx);

			/* skip to end of loop */
			undo.push_var(mode) = MODE_PARSE_ONLY_LOOP;
		}
		break;

	/*
	 * control structures (conditionals)
	 */
	case '|':
		if (mode == MODE_PARSE_ONLY_COND) {
			if (!skip_else && !nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			}
			return this;
		}
		BEGIN_EXEC(this);

		/* skip to end of conditional; skip ELSE-part */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	case '\'':
		if (mode != MODE_PARSE_ONLY_COND)
			break;

		if (!nest_level) {
			undo.push_var<Mode>(mode);
			mode = MODE_NORMAL;
			undo.push_var<bool>(skip_else);
			skip_else = false;
		} else {
			undo.push_var<gint>(nest_level);
			nest_level--;
		}
		break;

	/*
	 * Command-line editing
	 */
	/*$ "{" "}"
	 * { -- Edit command line
	 * }
	 *
	 * The opening curly bracket is a powerful command
	 * to edit command lines but has very simple semantics.
	 * It copies the current commandline into the global
	 * command line editing register (called Escape, i.e.
	 * ASCII 27) and edits this register.
	 * The curly bracket itself is not copied.
	 *
	 * The command line may then be edited using any
	 * \*(ST command or construct.
	 * You may switch between the command line editing
	 * register and other registers or buffers.
	 * The user will then usually reapply (called update)
	 * the current command-line.
	 *
	 * The closing curly bracket will update the current
	 * command-line with the contents of the global command
	 * line editing register.
	 * To do so it merely rubs-out the current command-line
	 * up to the first changed character and inserts
	 * all characters following from the updated command
	 * line into the command stream.
	 * To prevent the undesired rubout of the entire
	 * command-line, the replacement command ("}") is only
	 * allowed when the replacement register currently edited
	 * since it will otherwise be usually empty.
	 *
	 * .B Note:
	 *   - Command line editing only works on command lines,
	 *     but not arbitrary macros.
	 *     It is therefore not available in batch mode and
	 *     will yield an error if used.
	 *   - Command line editing commands may be safely used
	 *     from macro invocations.
	 *     Such macros are called command line editing macros.
	 *   - A command line update from a macro invocation will
	 *     always yield to the outer-most macro level (i.e.
	 *     the command line macro).
	 *     Code following the update command in the macro
	 *     will thus never be executed.
	 *   - As a safe-guard against command line trashing due
	 *     to erroneous changes at the beginning of command
	 *     lines, a backup mechanism is implemented:
	 *     If the updated command line yields an error at
	 *     any command during the update, the original
	 *     command line will be restored with an algorithm
	 *     similar to command line updating and the update
	 *     command will fail instead.
	 *     That way it behaves like any other command that
	 *     yields an error:
	 *     The character resulting in the update is rejected
	 *     by the command line input subsystem.
	 *   - In the rare case that an aforementioned command line
	 *     backup fails, the commands following the erroneous
	 *     character will not be inserted again (will be lost).
	 */
	case '{':
		BEGIN_EXEC(this);
		if (!undo.enabled)
			throw Error("Command-line editing only possible in "
				    "interactive mode");

		current_doc_undo_edit();
		QRegisters::globals.edit(CTL_KEY_ESC_STR);

		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_CLEARALL);
		interface.ssm(SCI_ADDTEXT, cmdline.pc, (sptr_t)cmdline.str);
		interface.ssm(SCI_SCROLLCARET);
		interface.ssm(SCI_ENDUNDOACTION);

		/* must always support undo on global register */
		interface.undo_ssm(SCI_UNDO);
		break;

	case '}':
		BEGIN_EXEC(this);
		if (!undo.enabled)
			throw Error("Command-line editing only possible in "
				    "interactive mode");
		if (QRegisters::current != QRegisters::globals[CTL_KEY_ESC_STR])
			throw Error("Command-line replacement only allowed when "
				    "editing the replacement register");

		/* replace cmdline in the outer macro environment */
		cmdline.replace();
		/* never reached */

	/*
	 * modifiers
	 */
	case '@':
		/*
		 * @ modifier has syntactic significance, so set it even
		 * in PARSE_ONLY* modes
		 */
		undo.push_var<bool>(Modifiers::at);
		Modifiers::at = true;
		break;

	case ':':
		BEGIN_EXEC(this);
		undo.push_var<bool>(Modifiers::colon);
		Modifiers::colon = true;
		break;

	/*
	 * commands
	 */
	/*$ J jump
	 * [position]J -- Go to position in buffer
	 * [position]:J -> Success|Failure
	 *
	 * Sets dot to <position>.
	 * If <position> is omitted, 0 is implied and \(lqJ\(rq will
	 * go to the beginning of the buffer.
	 *
	 * If <position> is outside the range of the buffer, the
	 * command yields an error.
	 * If colon-modified, the command will instead return a
	 * condition boolean signalling whether the position could
	 * be changed or not.
	 */
	case 'J':
		BEGIN_EXEC(this);
		v = expressions.pop_num_calc(0, 0);
		if (Validate::pos(v)) {
			if (current_doc_must_undo())
				interface.undo_ssm(SCI_GOTOPOS,
				                   interface.ssm(SCI_GETCURRENTPOS));
			interface.ssm(SCI_GOTOPOS, v);

			if (eval_colon())
				expressions.push(SUCCESS);
		} else if (eval_colon()) {
			expressions.push(FAILURE);
		} else {
			throw MoveError("J");
		}
		break;

	/*$ C move
	 * [n]C -- Move dot <n> characters
	 * -C
	 * [n]:C -> Success|Failure
	 *
	 * Adds <n> to dot. 1 or -1 is implied if <n> is omitted.
	 * Fails if <n> would move dot off-page.
	 * The colon modifier results in a success-boolean being
	 * returned instead.
	 */
	case 'C':
		BEGIN_EXEC(this);
		rc = move_chars(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("C");
		break;

	/*$ R reverse
	 * [n]R -- Move dot <n> characters backwards
	 * -R
	 * [n]:R -> Success|Failure
	 *
	 * Subtracts <n> from dot.
	 * It is equivalent to \(lq-nC\(rq.
	 */
	case 'R':
		BEGIN_EXEC(this);
		rc = move_chars(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("R");
		break;

	/*$ L line
	 * [n]L -- Move dot <n> lines forwards
	 * -L
	 * [n]:L -> Success|Failure
	 *
	 * Move dot to the beginning of the line specified
	 * relatively to the current line.
	 * Therefore a value of 0 for <n> goes to the
	 * beginning of the current line, 1 will go to the
	 * next line, -1 to the previous line etc.
	 * If <n> is omitted, 1 or -1 is implied depending on
	 * the sign prefix.
	 *
	 * If <n> would move dot off-page, the command yields
	 * an error.
	 * The colon-modifer results in a condition boolean
	 * being returned instead.
	 */
	case 'L':
		BEGIN_EXEC(this);
		rc = move_lines(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("L");
		break;

	/*$ B backwards
	 * [n]B -- Move dot <n> lines backwards
	 * -B
	 * [n]:B -> Success|Failure
	 *
	 * Move dot to the beginning of the line <n>
	 * lines before the current one.
	 * It is equivalent to \(lq-nL\(rq.
	 */
	case 'B':
		BEGIN_EXEC(this);
		rc = move_lines(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("B");
		break;

	/*$ W word
	 * [n]W -- Move dot by words
	 * -W
	 * [n]:W -> Success|Failure
	 *
	 * Move dot <n> words forward.
	 *   - If <n> is positive, dot is positioned at the beginning
	 *     of the word <n> words after the current one.
	 *   - If <n> is negative, dot is positioned at the end
	 *     of the word <n> words before the current one.
	 *   - If <n> is zero, dot is not moved.
	 *
	 * \(lqW\(rq uses Scintilla's definition of a word as
	 * configurable using the
	 * .B SCI_SETWORDCHARS
	 * message.
	 *
	 * Otherwise, the command's behaviour is analogous to
	 * the \(lqC\(rq command.
	 */
	case 'W': {
		sptr_t pos;
		unsigned int msg = SCI_WORDRIGHTEND;

		BEGIN_EXEC(this);
		v = expressions.pop_num_calc();

		pos = interface.ssm(SCI_GETCURRENTPOS);
		/*
		 * FIXME: would be nice to do this with constant amount of
		 * editor messages. E.g. by using custom algorithm accessing
		 * the internal document buffer.
		 */
		if (v < 0) {
			v *= -1;
			msg = SCI_WORDLEFTEND;
		}
		while (v--) {
			sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
			interface.ssm(msg);
			if (pos == interface.ssm(SCI_GETCURRENTPOS))
				break;
		}
		if (v < 0) {
			if (current_doc_must_undo())
				interface.undo_ssm(SCI_GOTOPOS, pos);
			if (eval_colon())
				expressions.push(SUCCESS);
		} else {
			interface.ssm(SCI_GOTOPOS, pos);
			if (eval_colon())
				expressions.push(FAILURE);
			else
				throw MoveError("W");
		}
		break;
	}

	/*$ V
	 * [n]V -- Delete words forward
	 * -V
	 * [n]:V -> Success|Failure
	 *
	 * Deletes the next <n> words until the end of the
	 * n'th word after the current one.
	 * If <n> is negative, deletes up to end of the
	 * n'th word before the current one.
	 * If <n> is omitted, 1 or -1 is implied depending on the
	 * sign prefix.
	 *
	 * It uses Scintilla's definition of a word as configurable
	 * using the
	 * .B SCI_SETWORDCHARS
	 * message.
	 *
	 * If the words to delete extend beyond the range of the
	 * buffer, the command yields an error.
	 * If colon-modified it instead returns a condition code.
	 */
	case 'V':
		BEGIN_EXEC(this);
		rc = delete_words(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <V>");
		break;

	/*$ Y
	 * [n]Y -- Delete word backwards
	 * -Y
	 * [n]:Y -> Success|Failure
	 *
	 * Delete <n> words backward.
	 * <n>Y is equivalent to \(lq-nV\(rq.
	 */
	case 'Y':
		BEGIN_EXEC(this);
		rc = delete_words(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <Y>");
		break;

	/*$ "=" print
	 * <n>= -- Show value as message
	 *
	 * Shows integer <n> as a message in the message line and/or
	 * on the console.
	 * It is currently always formatted as a decimal integer and
	 * shown with the user-message severity.
	 * The command fails if <n> is not given.
	 */
	/**
	 * @todo perhaps care about current radix
	 * @todo colon-modifier to suppress line-break on console?
	 */
	case '=':
		BEGIN_EXEC(this);
		expressions.eval();
		if (!expressions.args())
			throw ArgExpectedError('=');
		interface.msg(InterfaceCurrent::MSG_USER,
		              "%" TECO_INTEGER_FORMAT,
			      expressions.pop_num_calc());
		break;

	/*$ K kill
	 * [n]K -- Kill lines
	 * -K
	 * from,to K
	 * [n]:K -> Success|Failure
	 * from,to:K -> Success|Failure
	 *
	 * Deletes characters up to the beginning of the
	 * line <n> lines after or before the current one.
	 * If <n> is 0, \(lqK\(rq will delete up to the beginning
	 * of the current line.
	 * If <n> is omitted, the sign prefix will be implied.
	 * So to delete the entire line regardless of the position
	 * in it, one can use \(lq0KK\(rq.
	 *
	 * If the deletion is beyond the buffer's range, the command
	 * will yield an error unless it has been colon-modified
	 * so it returns a condition code.
	 *
	 * If two arguments <from> and <to> are available, the
	 * command is synonymous to <from>,<to>D.
	 */
	case 'K':
	/*$ D delete
	 * [n]D -- Delete characters
	 * -D
	 * from,to D
	 * [n]:D -> Success|Failure
	 * from,to:D -> Success|Failure
	 *
	 * If <n> is positive, the next <n> characters (up to and
	 * character .+<n>) are deleted.
	 * If <n> is negative, the previous <n> characters are
	 * deleted.
	 * If <n> is omitted, the sign prefix will be implied.
	 *
	 * If two arguments can be popped from the stack, the
	 * command will delete the characters with absolute
	 * position <from> up to <to> from the current buffer.
	 *
	 * If the character range to delete is beyond the buffer's
	 * range, the command will yield an error unless it has
	 * been colon-modified so it returns a condition code
	 * instead.
	 */
	case 'D': {
		tecoInt from, len;

		BEGIN_EXEC(this);
		expressions.eval();

		if (expressions.args() <= 1) {
			from = interface.ssm(SCI_GETCURRENTPOS);
			if (chr == 'D') {
				len = expressions.pop_num_calc();
				rc = TECO_BOOL(Validate::pos(from + len));
			} else /* chr == 'K' */ {
				sptr_t line;
				line = interface.ssm(SCI_LINEFROMPOSITION, from) +
				       expressions.pop_num_calc();
				len = interface.ssm(SCI_POSITIONFROMLINE, line)
				    - from;
				rc = TECO_BOOL(Validate::line(line));
			}
			if (len < 0) {
				len *= -1;
				from -= len;
			}
		} else {
			tecoInt to = expressions.pop_num();
			from = expressions.pop_num();
			len = to - from;
			rc = TECO_BOOL(len >= 0 && Validate::pos(from) &&
				       Validate::pos(to));
		}

		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw RangeError(chr);

		if (len == 0 || IS_FAILURE(rc))
			break;

		if (current_doc_must_undo()) {
			interface.undo_ssm(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));
			interface.undo_ssm(SCI_UNDO);
		}

		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_DELETERANGE, from, len);
		interface.ssm(SCI_ENDUNDOACTION);
		ring.dirtify();
		break;
	}

	/*$ A
	 * [n]A -> code -- Get character code from buffer
	 * -A -> code
	 *
	 * Returns the character <code> of the character
	 * <n> relative to dot from the buffer.
	 * This can be an ASCII <code> or Unicode codepoint
	 * depending on Scintilla's encoding of the current
	 * buffer.
	 *   - If <n> is 0, return the <code> of the character
	 *     pointed to by dot.
	 *   - If <n> is 1, return the <code> of the character
	 *     immediately after dot.
	 *   - If <n> is -1, return the <code> of the character
	 *     immediately preceding dot, ecetera.
	 *   - If <n> is omitted, the sign prefix is implied.
	 *
	 * If the position of the queried character is off-page,
	 * the command will yield an error.
	 */
	/** @todo does Scintilla really return code points??? */
	case 'A':
		BEGIN_EXEC(this);
		v = interface.ssm(SCI_GETCURRENTPOS) +
		    expressions.pop_num_calc();
		/*
		 * NOTE: We cannot use Validate::pos() here since
		 * the end of the buffer is not a valid position for <A>.
		 */
		if (v < 0 || v >= interface.ssm(SCI_GETLENGTH))
			throw RangeError("A");
		expressions.push(interface.ssm(SCI_GETCHARAT, v));
		break;

	default:
		throw SyntaxError(chr);
	}

	return this;
}

StateFCommand::StateFCommand()
{
	transitions['\0'] = this;
	transitions['K'] = &States::searchkill;
	transitions['D'] = &States::searchdelete;
	transitions['S'] = &States::replace;
	transitions['R'] = &States::replacedefault;
	transitions['G'] = &States::changedir;
}

State *
StateFCommand::custom(gchar chr)
{
	switch (chr) {
	/*
	 * loop flow control
	 */
	/*$ F<
	 * F< -- Go to loop start or jump to beginning of macro
	 *
	 * Immediately jumps to the current loop's start.
	 * Also works from inside conditionals.
	 *
	 * Outside of loops \(em or in a macro without
	 * a loop \(em this jumps to the beginning of the macro.
	 */
	case '<':
		BEGIN_EXEC(&States::start);
		/* FIXME: what if in brackets? */
		expressions.discard_args();

		macro_pc = loop_stack.items() > loop_stack_fp
					? loop_stack.peek().pc : -1;
		break;

	/*$ F> continue
	 * F> -- Go to loop end
	 * :F>
	 *
	 * Jumps to the current loop's end.
	 * If the loop has remaining iterations or runs indefinitely,
	 * the jump is performed immediately just as if \(lq>\(rq
	 * had been executed.
	 * If the loop has reached its last iteration, \*(ST will
	 * parse until the loop end command has been found and control
	 * resumes after the end of the loop.
	 *
	 * In interactive mode, if the loop is incomplete and must
	 * be exited, you can type in the loop's remaining commands
	 * without them being executed (but they are parsed).
	 *
	 * When colon-modified, \fB:F>\fP behaves like \fB:>\fP
	 * and allows numbers to be aggregated on the stack.
	 *
	 * Calling \fBF>\fP outside of a loop at the current
	 * macro invocation level will throw an error.
	 */
	/*
	 * NOTE: This is almost identical to the normal
	 * loop end since we don't really want to or need to
	 * parse till the end of the loop.
	 */
	case '>': {
		BEGIN_EXEC(&States::start);

		if (loop_stack.items() <= loop_stack_fp)
			throw Error("Jump to loop end without corresponding "
			            "loop start command");
		LoopContext &ctx = loop_stack.peek();
		bool colon_modified = eval_colon();

		if (!ctx.pass_through) {
			if (colon_modified) {
				expressions.eval();
				expressions.push(Expressions::OP_NEW);
			} else {
				expressions.discard_args();
			}
		}

		if (ctx.counter == 1) {
			/* this was the last loop iteration */
			if (!ctx.pass_through)
				expressions.brace_close();
			LoopStack::undo_push<loop_stack>(loop_stack.pop());
			/* skip to end of loop */
			undo.push_var(mode) = MODE_PARSE_ONLY_LOOP;
		} else {
			/* repeat loop */
			macro_pc = ctx.pc;
			ctx.counter = MAX(ctx.counter - 1, -1);
		}
		break;
	}

	/*
	 * conditional flow control
	 */
	/*$ "F'"
	 * F\' -- Jump to end of conditional
	 */
	case '\'':
		BEGIN_EXEC(&States::start);
		/* skip to end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		undo.push_var<bool>(skip_else);
		skip_else = true;
		break;

	/*$ F|
	 * F| -- Jump to else-part of conditional
	 *
	 * Jump to else-part of conditional or end of
	 * conditional (only if invoked from inside the
	 * condition's else-part).
	 */
	case '|':
		BEGIN_EXEC(&States::start);
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	default:
		throw SyntaxError(chr);
	}

	return &States::start;
}

void
UndoTokenChangeDir::run(void)
{
	/*
	 * Changing the directory on rub-out may fail.
	 * This is handled silently.
	 */
	g_chdir(dir);
}

/*$ FG cd change-dir folder-go
 * FG[directory]$ -- Change working directory
 *
 * Changes the process' current working directory
 * to <directory> which affects all subsequent
 * operations on relative file names like
 * tab-completions.
 * It is also inherited by external processes spawned
 * via \fBEC\fP and \fBEG\fP.
 *
 * If <directory> is omitted, the working directory
 * is changed to the current user's home directory
 * as set by the \fBHOME\fP environment variable
 * (i.e. its corresponding \(lq$HOME\(rq environment
 * register).
 * This variable is always initialized by \*(ST
 * (see \fBsciteco\fP(1)).
 * Therefore the expression \(lqFG\fB$\fP\(rq is
 * exactly equivalent to both \(lqFG~\fB$\fP\(rq and
 * \(lqFG^EQ[$HOME]\fB$\fP\(rq.
 *
 * The current working directory is also mapped to
 * the special global Q-Register \(lq$\(rq (dollar sign)
 * which may be used retrieve the current working directory.
 *
 * String-building characters are enabled on this
 * command and directories can be tab-completed.
 */
State *
StateChangeDir::got_file(const gchar *filename)
{
	gchar *dir;

	BEGIN_EXEC(&States::start);

	/* passes ownership of string to undo token object */
	undo.push_own<UndoTokenChangeDir>(g_get_current_dir());

	dir = *filename ? g_strdup(filename)
	                : QRegisters::globals["$HOME"]->get_string();

	if (g_chdir(dir)) {
		/* FIXME: Is errno usable on Windows here? */
		Error err("Cannot change working directory "
		          "to \"%s\"", dir);
		g_free(dir);
		throw err;
	}

	g_free(dir);
	return &States::start;
}

StateCondCommand::StateCondCommand()
{
	transitions['\0'] = this;
}

State *
StateCondCommand::custom(gchar chr)
{
	tecoInt value = 0;
	bool result;

	switch (mode) {
	case MODE_PARSE_ONLY_COND:
		undo.push_var(nest_level)++;
		break;

	case MODE_NORMAL:
		expressions.eval();

		if (chr == '~')
			/* don't pop value for ~ conditionals */
			break;

		if (!expressions.args())
			throw ArgExpectedError('"');
		value = expressions.pop_num_calc();
		break;

	default:
		break;
	}

	switch (String::toupper(chr)) {
	case '~':
		BEGIN_EXEC(&States::start);
		result = !expressions.args();
		break;
	case 'A':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalpha((gchar)value);
		break;
	case 'C':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalnum((gchar)value) ||
			 value == '.' || value == '$' || value == '_';
		break;
	case 'D':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isdigit((gchar)value);
		break;
	case 'I':
		BEGIN_EXEC(&States::start);
		result = G_IS_DIR_SEPARATOR((gchar)value);
		break;
	case 'S':
	case 'T':
		BEGIN_EXEC(&States::start);
		result = IS_SUCCESS(value);
		break;
	case 'F':
	case 'U':
		BEGIN_EXEC(&States::start);
		result = IS_FAILURE(value);
		break;
	case 'E':
	case '=':
		BEGIN_EXEC(&States::start);
		result = value == 0;
		break;
	case 'G':
	case '>':
		BEGIN_EXEC(&States::start);
		result = value > 0;
		break;
	case 'L':
	case '<':
		BEGIN_EXEC(&States::start);
		result = value < 0;
		break;
	case 'N':
		BEGIN_EXEC(&States::start);
		result = value != 0;
		break;
	case 'R':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalnum((gchar)value);
		break;
	case 'V':
		BEGIN_EXEC(&States::start);
		result = g_ascii_islower((gchar)value);
		break;
	case 'W':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isupper((gchar)value);
		break;
	default:
		throw Error("Invalid conditional type \"%c\"", chr);
	}

	if (!result)
		/* skip to ELSE-part or end of conditional */
		undo.push_var(mode) = MODE_PARSE_ONLY_COND;

	return &States::start;
}

StateControl::StateControl()
{
	transitions['\0'] = this;
	transitions['I'] = &States::insert_indent;
	transitions['U'] = &States::ctlucommand;
	transitions['^'] = &States::ascii;
	transitions['['] = &States::escape;
}

State *
StateControl::custom(gchar chr)
{
	switch (String::toupper(chr)) {
	/*$ ^C exit
	 * ^C -- Exit program immediately
	 *
	 * Lets the top-level macro return immediately
	 * regardless of the current macro invocation frame.
	 * This command is only allowed in batch mode,
	 * so it is not invoked accidentally when using
	 * the CTRL+C immediate editing command to
	 * interrupt long running operations.
	 * When using \fB^C\fP in a munged file,
	 * interactive mode is never started, so it behaves
	 * effectively just like \(lq-EX\fB$$\fP\(rq
	 * (when executed in the top-level macro at least).
	 *
	 * The \fBquit\fP hook is still executed.
	 */
	case 'C':
		BEGIN_EXEC(&States::start);
		if (undo.enabled)
			throw Error("<^C> not allowed in interactive mode");
		quit_requested = true;
		throw Quit();

	/*$ ^O octal
	 * ^O -- Set radix to 8 (octal)
	 */
	case 'O':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(8);
		break;

	/*$ ^D decimal
	 * ^D -- Set radix to 10 (decimal)
	 */
	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(10);
		break;

	/*$ ^R radix
	 * radix^R -- Set and get radix
	 * ^R -> radix
	 *
	 * Set current radix to arbitrary value <radix>.
	 * If <radix> is omitted, the command instead
	 * returns the current radix.
	 */
	case 'R':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args())
			expressions.push(expressions.radix);
		else
			expressions.set_radix(expressions.pop_num_calc());
		break;

	/*
	 * Additional numeric operations
	 */
	/*$ ^_ negate
	 * n^_ -> ~n -- Binary negation
	 *
	 * Binary negates (complements) <n> and returns
	 * the result.
	 * Binary complements are often used to negate
	 * \*(ST booleans.
	 */
	case '_':
		BEGIN_EXEC(&States::start);
		expressions.push(~expressions.pop_num_calc());
		break;

	case '*':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_POW);
		break;

	case '/':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_MOD);
		break;

	case '#':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_XOR);
		break;

	default:
		throw Error("Unsupported command <^%c>", chr);
	}

	return &States::start;
}

/*$ ^^ ^^c
 * ^^c -> n -- Get ASCII code of character
 *
 * Returns the ASCII code of the character <c>
 * that is part of the command.
 * Can be used in place of integer constants for improved
 * readability.
 * For instance ^^A will return 65.
 *
 * Note that this command can be typed CTRL+Caret or
 * Caret-Caret.
 */
StateASCII::StateASCII() : State()
{
	transitions['\0'] = this;
}

State *
StateASCII::custom(gchar chr)
{
	BEGIN_EXEC(&States::start);

	expressions.push(chr);

	return &States::start;
}

/*
 * The Escape state is special, as it implements
 * a kind of "lookahead" for the ^[ command (discard all
 * arguments).
 * It is not executed immediately as usual in SciTECO
 * but only if not followed by an escape character.
 * This is necessary since $$ is the macro return
 * and command-line termination command and it must not
 * discard arguments.
 * Deferred execution of ^[ is possible since it does
 * not have any visible side-effects - its effects can
 * only be seen when executing the following command.
 */
StateEscape::StateEscape()
{
	transitions['\0'] = this;
}

State *
StateEscape::custom(gchar chr)
{
	/*$ ^[^[ ^[$ $$ terminate return
	 * [a1,a2,...]$$ -- Terminate command line or return from macro
	 * [a1,a2,...]^[$
	 *
	 * Returns from the current macro invocation.
	 * This will pass control to the calling macro immediately
	 * and is thus faster than letting control reach the macro's end.
	 * Also, direct arguments to \fB$$\fP will be left on the expression
	 * stack when the macro returns.
	 * \fB$$\fP closes loops automatically and is thus safe to call
	 * from loop bodies.
	 * Furthermore, it has defined semantics when executed
	 * from within braced expressions:
	 * All braces opened in the current macro invocation will
	 * be closed and their values discarded.
	 * Only the direct arguments to \fB$$\fP will be kept.
	 *
	 * Returning from the top-level macro in batch mode
	 * will exit the program or start up interactive mode depending
	 * on whether program exit has been requested.
	 * \(lqEX\fB$$\fP\(rq is thus a common idiom to exit
	 * prematurely.
	 *
	 * In interactive mode, returning from the top-level macro
	 * (i.e. typing \fB$$\fP at the command line) has the
	 * effect of command line termination.
	 * The arguments to \fB$$\fP are currently not used
	 * when terminating a command line \(em the new command line
	 * will always start with a clean expression stack.
	 *
	 * The first \fIescape\fP of \fB$$\fP may be typed either
	 * as an escape character (ASCII 27), in up-arrow mode
	 * (e.g. \fB^[$\fP) or as a dollar character \(em the
	 * second character must be either a real escape character
	 * or a dollar character.
	 */
	if (chr == CTL_KEY_ESC || chr == '$') {
		BEGIN_EXEC(&States::start);
		States::current = &States::start;
		expressions.eval();
		throw Return(expressions.args());
	}

	/*
	 * Alternatives: ^[, <CTRL/[>, <ESC>, $ (dollar)
	 */
	/*$ ^[ $ escape discard
	 * $ -- Discard all arguments
	 * ^[
	 *
	 * Pops and discards all values from the stack that
	 * might otherwise be used as arguments to following
	 * commands.
	 * Therefore it stops popping on stack boundaries like
	 * they are introduced by arithmetic brackets or loops.
	 *
	 * Note that ^[ is usually typed using the Escape key.
	 * CTRL+[ however is possible as well and equivalent to
	 * Escape in every manner.
	 * The up-arrow notation however is processed like any
	 * ordinary command and only works at the begining of
	 * a command.
	 * Additionally, this command may be written as a single
	 * dollar character.
	 */
	if (mode == MODE_NORMAL)
		expressions.discard_args();
	return States::start.get_next_state(chr);
}

void
StateEscape::end_of_macro(void)
{
	/*
	 * Due to the deferred nature of ^[,
	 * it is valid to end in the "escape" state.
	 */
	expressions.discard_args();
}

StateECommand::StateECommand()
{
	transitions['\0'] = this;
	transitions['%'] = &States::epctcommand;
	transitions['B'] = &States::editfile;
	transitions['C'] = &States::executecommand;
	transitions['G'] = &States::egcommand;
	transitions['I'] = &States::insert_nobuilding;
	transitions['M'] = &States::macro_file;
	transitions['N'] = &States::glob_pattern;
	transitions['S'] = &States::scintilla_symbols;
	transitions['Q'] = &States::eqcommand;
	transitions['U'] = &States::eucommand;
	transitions['W'] = &States::savefile;
}

State *
StateECommand::custom(gchar chr)
{
	switch (String::toupper(chr)) {
	/*$ EF close
	 * [bool]EF -- Remove buffer from ring
	 * -EF
	 *
	 * Removes buffer from buffer ring, effectively
	 * closing it.
	 * If the buffer is dirty (modified), EF will yield
	 * an error.
	 * <bool> may be a specified to enforce closing dirty
	 * buffers.
	 * If it is a Failure condition boolean (negative),
	 * the buffer will be closed unconditionally.
	 * If <bool> is absent, the sign prefix (1 or -1) will
	 * be implied, so \(lq-EF\(rq will always close the buffer.
	 *
	 * It is noteworthy that EF will be executed immediately in
	 * interactive mode but can be rubbed out at a later time
	 * to reopen the file.
	 * Closed files are kept in memory until the command line
	 * is terminated.
	 */
	case 'F':
		BEGIN_EXEC(&States::start);
		if (QRegisters::current)
			throw Error("Q-Register currently edited");

		if (IS_FAILURE(expressions.pop_num_calc()) &&
		    ring.current->dirty)
			throw Error("Buffer \"%s\" is dirty",
				    ring.current->filename ? : "(Unnamed)");

		ring.close();
		break;

	/*$ ED flags
	 * flags ED -- Set and get ED-flags
	 * [off,]on ED
	 * ED -> flags
	 *
	 * With arguments, the command will set the \fBED\fP flags.
	 * <flags> is a bitmap of flags to set.
	 * Specifying one argument to set the flags is a special
	 * case of specifying two arguments that allow to control
	 * which flags to enable/disable.
	 * <off> is a bitmap of flags to disable (set to 0 in ED
	 * flags) and <on> is a bitmap of flags that is ORed into
	 * the flags variable.
	 * If <off> is omitted, the value 0^_ is implied.
	 * In otherwords, all flags are turned off before turning
	 * on the <on> flags.
	 * Without any argument ED returns the current flags.
	 *
	 * Currently, the following flags are used by \*(ST:
	 *   - 8: Enable/disable automatic folding of case-insensitive
	 *     command characters during interactive key translation.
	 *     The case of letter keys is inverted, so one or two
	 *     character commands will typically be inserted upper-case,
	 *     but you can still press Shift to insert lower-case letters.
	 *     Case-insensitive Q-Register specifications are not
	 *     case folded.
	 *     This is thought to improve the readability of the command
	 *     line macro.
	 *   - 16: Enable/disable automatic translation of end of
	 *     line sequences to and from line feed.
	 *   - 32: Enable/Disable buffer editing hooks
	 *     (via execution of macro in global Q-Register \(lqED\(rq)
	 *   - 64: Enable/Disable function key macros
	 *   - 128: Enable/Disable enforcement of UNIX98
	 *     \(lq/bin/sh\(rq emulation for operating system command
	 *     executions
	 *   - 256: Enable/Disable \fBxterm\fP(1) clipboard support.
	 *     Should only be enabled if XTerm allows the
	 *     \fIGetSelection\fP and \fISetSelection\fP window
	 *     operations.
	 *
	 * The features controlled thus are discribed in other sections
	 * of this manual.
	 *
	 * The default value of the \fBED\fP flags is 16
	 * (only automatic EOL translation enabled).
	 */
	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args()) {
			expressions.push(Flags::ed);
		} else {
			tecoInt on = expressions.pop_num_calc();
			tecoInt off = expressions.pop_num_calc(0, ~(tecoInt)0);

			undo.push_var(Flags::ed);
			Flags::ed = (Flags::ed & ~off) | on;
		}
		break;

	/*$ EJ properties
	 * [key]EJ -> value -- Get and set system properties
	 * -EJ -> value
	 * value,keyEJ
	 * rgb,color,3EJ
	 *
	 * This command may be used to get and set system
	 * properties.
	 * With one argument, it retrieves a numeric property
	 * identified by \fIkey\fP.
	 * If \fIkey\fP is omitted, the prefix sign is implied
	 * (1 or -1).
	 * With two arguments, it sets property \fIkey\fP to
	 * \fIvalue\fP and returns nothing. Some property \fIkeys\fP
	 * may require more than one value. Properties may be
	 * write-only or read-only.
	 *
	 * The following property keys are defined:
	 * .IP 0 4
	 * The current user interface: 1 for Curses, 2 for GTK
	 * (\fBread-only\fP)
	 * .IP 1
	 * The current numbfer of buffers: Also the numeric id
	 * of the last buffer in the ring. This is implied if
	 * no argument is given, so \(lqEJ\(rq returns the number
	 * of buffers in the ring.
	 * (\fBread-only\fP)
	 * .IP 2
	 * The current memory limit in bytes.
	 * This limit helps to prevent dangerous out-of-memory
	 * conditions (e.g. resulting from infinite loops) by
	 * constantly sampling the memory requirements of \*(ST.
	 * Note that not all platforms support precise measurements
	 * of the current memory usage \(em \*(ST will fall back
	 * to an approximation which might be less than the actual
	 * usage on those platforms.
	 * Memory limiting is effective in batch and interactive mode.
	 * Commands which would exceed that limit will fail instead
	 * allowing users to recover in interactive mode, e.g. by
	 * terminating the command line.
	 * When getting, a zero value indicates that memory limiting is
	 * disabled.
	 * Setting a value less than or equal to 0 as in
	 * \(lq0,2EJ\(rq disables the limit.
	 * \fBWarning:\fP Disabling memory limiting may provoke
	 * out-of-memory errors in long running or infinite loops
	 * (interactive mode) that result in abnormal program
	 * termination.
	 * Setting a new limit may fail if the current memory
	 * requirements are too large for the new limit \(em if
	 * this happens you may have to clear your command-line
	 * first.
	 * Memory limiting is enabled by default.
	 * .IP 3
	 * This \fBwrite-only\fP property allows redefining the
	 * first 16 entries of the terminal color palette \(em a
	 * feature required by some
	 * color schemes when using the Curses user interface.
	 * When setting this property, you are making a request
	 * to define the terminal \fIcolor\fP as the Scintilla-compatible
	 * RGB color value given in the \fIrgb\fP parameter.
	 * \fIcolor\fP must be a value between 0 and 15
	 * corresponding to black, red, green, yellow, blue, magenta,
	 * cyan, white, bright black, bright red, etc. in that order.
	 * The \fIrgb\fP value has the format 0xBBGGRR, i.e. the red
	 * component is the least-significant byte and all other bytes
	 * are ignored.
	 * Note that on curses, RGB color values sent to Scintilla
	 * are actually mapped to these 16 colors by the Scinterm port
	 * and may represent colors with no resemblance to the \(lqRGB\(rq
	 * value used (depending on the current palette) \(em they should
	 * instead be viewed as placeholders for 16 standard terminal
	 * color codes.
	 * Please refer to the Scinterm manual for details on the allowed
	 * \(lqRGB\(rq values and how they map to terminal colors.
	 * This command provides a crude way to request exact RGB colors
	 * for the first 16 terminal colors.
	 * The color definition may be queued or be completely ignored
	 * on other user interfaces and no feedback is given
	 * if it fails. In fact feedback cannot be given reliably anyway.
	 * Note that on 8 color terminals, only the first 8 colors
	 * can be redefined (if you are lucky).
	 * Note that due to restrictions of most terminal emulators
	 * and some curses implementations, this command simply will not
	 * restore the original palette entry or request
	 * when rubbed out and should generally only be used in
	 * \fIbatch-mode\fP \(em typically when loading a color scheme.
	 * For the same reasons \(em even though \*(ST tries hard to
	 * restore the original palette on exit \(em palette changes may
	 * persist after \*(ST terminates on most terminal emulators on Unix.
	 * The only emulator which will restore their default palette
	 * on exit the author is aware of is \fBxterm\fP(1) and
	 * the Linux console driver.
	 * You have been warned. Good luck.
	 */
	case 'J': {
		BEGIN_EXEC(&States::start);

		enum {
			EJ_USER_INTERFACE = 0,
			EJ_BUFFERS,
			EJ_MEMORY_LIMIT,
			EJ_INIT_COLOR
		};
		tecoInt property;

		expressions.eval();
		property = expressions.pop_num_calc();
		if (expressions.args() > 0) {
			/* set property */
			tecoInt value = expressions.pop_num_calc();

			switch (property) {
			case EJ_MEMORY_LIMIT:
				memlimit.set_limit(MAX(0, value));
				break;

			case EJ_INIT_COLOR:
				if (value < 0 || value >= 16)
					throw Error("Invalid color code %" TECO_INTEGER_FORMAT
					            " specified for <EJ>", value);
				if (!expressions.args())
					throw ArgExpectedError("EJ");
				interface.init_color((guint)value,
				                     (guint32)expressions.pop_num_calc());
				break;

			default:
				throw Error("Cannot set property %" TECO_INTEGER_FORMAT
				            " for <EJ>", property);
			}

			break;
		}

		switch (property) {
		case EJ_USER_INTERFACE:
#ifdef INTERFACE_CURSES
			expressions.push(1);
#elif defined(INTERFACE_GTK)
			expressions.push(2);
#else
#error Missing value for current interface!
#endif
			break;

		case EJ_BUFFERS:
			expressions.push(ring.get_id(ring.last()));
			break;

		case EJ_MEMORY_LIMIT:
			expressions.push(memlimit.limit);
			break;

		default:
			throw Error("Invalid property %" TECO_INTEGER_FORMAT
			            " for <EJ>", property);
		}
		break;
	}

	/*$ EL eol
	 * 0EL -- Set or get End of Line mode
	 * 13,10:EL
	 * 1EL
	 * 13:EL
	 * 2EL
	 * 10:EL
	 * EL -> 0 | 1 | 2
	 * :EL -> 13,10 | 13 | 10
	 *
	 * Sets or gets the current document's End Of Line (EOL) mode.
	 * This is a thin wrapper around Scintilla's
	 * \fBSCI_SETEOLMODE\fP and \fBSCI_GETEOLMODE\fP messages but is
	 * shorter to type and supports restoring the EOL mode upon rubout.
	 * Like the Scintilla message, <EL> does \fBnot\fP change the
	 * characters in the current document.
	 * If automatic EOL translation is activated (which is the default),
	 * \*(ST will however use this information when saving files or
	 * writing to external processes.
	 *
	 * With one argument, the EOL mode is set according to these
	 * constants:
	 * .IP 0 4
	 * Carriage return (ASCII 13), followed by line feed (ASCII 10).
	 * This is the default EOL mode on DOS/Windows.
	 * .IP 1
	 * Carriage return (ASCII 13).
	 * The default EOL mode on old Mac OS systems.
	 * .IP 2
	 * Line feed (ASCII 10).
	 * The default EOL mode on POSIX/UNIX systems.
	 *
	 * In its colon-modified form, the EOL mode is set according
	 * to the EOL characters on the expression stack.
	 * \*(ST will only pop as many values as are necessary to
	 * determine the EOL mode.
	 *
	 * Without arguments, the current EOL mode is returned.
	 * When colon-modified, the current EOL mode's character sequence
	 * is pushed onto the expression stack.
	 */
	case 'L':
		BEGIN_EXEC(&States::start);

		expressions.eval();
		if (expressions.args() > 0) {
			gint eol_mode;

			if (eval_colon()) {
				switch (expressions.pop_num_calc()) {
				case '\r':
					eol_mode = SC_EOL_CR;
					break;
				case '\n':
					if (!expressions.args()) {
						eol_mode = SC_EOL_LF;
						break;
					}
					if (expressions.pop_num_calc() == '\r') {
						eol_mode = SC_EOL_CRLF;
						break;
					}
					/* fall through */
				default:
					throw Error("Invalid EOL sequence for <EL>");
				}
			} else {
				eol_mode = expressions.pop_num_calc();
				switch (eol_mode) {
				case SC_EOL_CRLF:
				case SC_EOL_CR:
				case SC_EOL_LF:
					break;
				default:
					throw Error("Invalid EOL mode %d for <EL>",
					            eol_mode);
				}
			}

			interface.undo_ssm(SCI_SETEOLMODE,
			                   interface.ssm(SCI_GETEOLMODE));
			interface.ssm(SCI_SETEOLMODE, eol_mode);
		} else if (eval_colon()) {
			expressions.push_str(get_eol_seq(interface.ssm(SCI_GETEOLMODE)));
		} else {
			expressions.push(interface.ssm(SCI_GETEOLMODE));
		}
		break;

	/*$ EX exit
	 * [bool]EX -- Exit program
	 * -EX
	 * :EX
	 *
	 * Exits \*(ST, or rather requests program termination
	 * at the end of the top-level macro.
	 * Therefore instead of exiting immediately which
	 * could be annoying in interactive mode, EX will
	 * result in program termination only when the command line
	 * is terminated.
	 * This allows EX to be rubbed out and used in macros.
	 * The usual command to exit \*(ST in interactive mode
	 * is thus \(lqEX\fB$$\fP\(rq.
	 * In batch mode EX will exit the program if control
	 * reaches the end of the munged file \(em instead of
	 * starting up interactive mode.
	 *
	 * If any buffer is dirty (modified), EX will yield
	 * an error.
	 * When specifying <bool> as a success/truth condition
	 * boolean, EX will not check whether there are modified
	 * buffers and will always succeed.
	 * If <bool> is omitted, the sign prefix is implied
	 * (1 or -1).
	 * In other words \(lq-EX\fB$$\fP\(rq is the usual
	 * interactive command sequence to discard all unsaved
	 * changes and exit.
	 *
	 * When colon-modified, <bool> is ignored and EX
	 * will instead immediately try to save all modified buffers \(em
	 * this can of course be reversed using rubout.
	 * Saving all buffers can fail, e.g. if the unnamed file
	 * is modified or if there is an IO error.
	 * \(lq:EX\fB$$\fP\(rq is nevertheless the usual interactive
	 * command sequence to exit while saving all modified
	 * buffers.
	 */
	/** @bug what if changing file after EX? will currently still exit */
	case 'X':
		BEGIN_EXEC(&States::start);

		if (eval_colon())
			ring.save_all_dirty_buffers();
		else if (IS_FAILURE(expressions.pop_num_calc()) &&
		         ring.is_any_dirty())
			throw Error("Modified buffers exist");

		undo.push_var(quit_requested) = true;
		break;

	default:
		throw SyntaxError(chr);
	}

	return &States::start;
}

static struct ScintillaMessage {
	unsigned int	iMessage;
	uptr_t		wParam;
	sptr_t		lParam;
} scintilla_message = {0, 0, 0};

/*$ ES scintilla message
 * -- Send Scintilla message
 * [lParam[,wParam]]ESmessage[,wParam]$[lParam]$ -> result
 *
 * Send Scintilla message with code specified by symbolic
 * name <message>, <wParam> and <lParam>.
 * <wParam> may be symbolic when specified as part of the
 * first string argument.
 * If not it is popped from the stack.
 * <lParam> may be specified as a constant string whose
 * pointer is passed to Scintilla if specified as the second
 * string argument.
 * If the second string argument is empty, <lParam> is popped
 * from the stack instead.
 * Parameters popped from the stack may be omitted, in which
 * case 0 is implied.
 * The message's return value is pushed onto the stack.
 *
 * All messages defined by Scintilla (as C macros) can be
 * used by passing their name as a string to ES
 * (e.g. ESSCI_LINESONSCREEN...).
 * The \(lqSCI_\(rq prefix may be omitted and message symbols
 * are case-insensitive.
 * Only the Scintilla lexer symbols (SCLEX_..., SCE_...)
 * may be used symbolically with the ES command as <wParam>,
 * other values must be passed as integers on the stack.
 * In interactive mode, symbols may be auto-completed by
 * pressing Tab.
 * String-building characters are by default interpreted
 * in the string arguments.
 *
 * .BR Warning :
 * Almost all Scintilla messages may be dispatched using
 * this command.
 * \*(ST does not keep track of the editor state changes
 * performed by these commands and cannot undo them.
 * You should never use it to change the editor state
 * (position changes, deletions, etc.) or otherwise
 * rub out will result in an inconsistent editor state.
 * There are however exceptions:
 *   - In the editor profile and batch mode in general,
 *     the ES command may be used freely.
 *   - In the ED hook macro (register \(lqED\(rq),
 *     when a file is added to the ring, most destructive
 *     operations can be performed since rubbing out the
 *     EB command responsible for the hook execution also
 *     removes the buffer from the ring again.
 */
State *
StateScintilla_symbols::done(const gchar *str)
{
	BEGIN_EXEC(&States::scintilla_lparam);

	undo.push_var(scintilla_message);
	if (*str) {
		gchar **symbols = g_strsplit(str, ",", -1);
		tecoInt v;

		if (!symbols[0])
			goto cleanup;
		if (*symbols[0]) {
			v = Symbols::scintilla.lookup(symbols[0], "SCI_");
			if (v < 0)
				throw Error("Unknown Scintilla message symbol \"%s\"",
					    symbols[0]);
			scintilla_message.iMessage = v;
		}

		if (!symbols[1])
			goto cleanup;
		if (*symbols[1]) {
			v = Symbols::scilexer.lookup(symbols[1]);
			if (v < 0)
				throw Error("Unknown Scintilla Lexer symbol \"%s\"",
					    symbols[1]);
			scintilla_message.wParam = v;
		}

		if (!symbols[2])
			goto cleanup;
		if (*symbols[2]) {
			v = Symbols::scilexer.lookup(symbols[2]);
			if (v < 0)
				throw Error("Unknown Scintilla Lexer symbol \"%s\"",
					    symbols[2]);
			scintilla_message.lParam = v;
		}

cleanup:
		g_strfreev(symbols);
	}

	expressions.eval();
	if (!scintilla_message.iMessage) {
		if (!expressions.args())
			throw Error("<ES> command requires at least a message code");

		scintilla_message.iMessage = expressions.pop_num_calc(0, 0);
	}
	if (!scintilla_message.wParam)
		scintilla_message.wParam = expressions.pop_num_calc(0, 0);

	return &States::scintilla_lparam;
}

State *
StateScintilla_lParam::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (!scintilla_message.lParam)
		scintilla_message.lParam = *str ? (sptr_t)str
						: expressions.pop_num_calc(0, 0);

	expressions.push(interface.ssm(scintilla_message.iMessage,
				       scintilla_message.wParam,
				       scintilla_message.lParam));

	undo.push_var(scintilla_message);
	memset(&scintilla_message, 0, sizeof(scintilla_message));

	return &States::start;
}

/*
 * NOTE: cannot support VideoTECO's <n>I because
 * beginning and end of strings must be determined
 * syntactically
 */
/*$ I insert
 * [c1,c2,...]I[text]$ -- Insert text with string building characters
 *
 * First inserts characters for all the values
 * on the argument stack (interpreted as codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> is inserted before <c2>, ecetera.
 * Secondly, the command inserts <text>.
 * In interactive mode, <text> is inserted interactively.
 *
 * String building characters are \fBenabled\fP for the
 * I command.
 * When editing \*(ST macros, using the \fBEI\fP command
 * may be better, since it has string building characters
 * disabled.
 */
/*$ EI
 * [c1,c2,...]EI[text]$ -- Insert text without string building characters
 *
 * Inserts text at the current position in the current
 * document.
 * This command is identical to the \fBI\fP command,
 * except that string building characters are \fBdisabled\fP.
 * Therefore it may be beneficial when editing \*(ST
 * macros.
 */
void
StateInsert::initial(void)
{
	guint args;

	expressions.eval();
	args = expressions.args();
	if (!args)
		return;

	interface.ssm(SCI_BEGINUNDOACTION);
	for (int i = args; i > 0; i--) {
		gchar chr = (gchar)expressions.peek_num(i-1);
		interface.ssm(SCI_ADDTEXT, 1, (sptr_t)&chr);
	}
	for (int i = args; i > 0; i--)
		expressions.pop_num_calc();
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	if (current_doc_must_undo())
		interface.undo_ssm(SCI_UNDO);
}

void
StateInsert::process(const gchar *str, gint new_chars)
{
	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_ADDTEXT, new_chars,
		      (sptr_t)(str + strlen(str) - new_chars));
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	if (current_doc_must_undo())
		interface.undo_ssm(SCI_UNDO);
}

State *
StateInsert::done(const gchar *str)
{
	/* nothing to be done when done */
	return &States::start;
}

/*
 * Alternatives: ^i, ^I, <CTRL/I>, <TAB>
 */
/*$ ^I indent
 * [char,...]^I[text]$ -- Insert with leading indention
 *
 * ^I (usually typed using the Tab key), first inserts
 * all the chars on the stack into the buffer, then indention
 * characters (one tab or multiple spaces) and eventually
 * the optional <text> is inserted interactively.
 * It is thus a derivate of the \fBI\fP (insertion) command.
 *
 * \*(ST uses Scintilla settings to determine the indention
 * characters.
 * If tab use is enabled with the \fBSCI_SETUSETABS\fP message,
 * a single tab character is inserted.
 * Tab use is enabled by default.
 * Otherwise, a number of spaces is inserted up to the
 * next tab stop so that the command's <text> argument
 * is inserted at the beginning of the next tab stop.
 * The size of the tab stops is configured by the
 * \fBSCI_SETTABWIDTH\fP Scintilla message (8 by default).
 * In combination with \*(ST's use of the tab key as an
 * immediate editing command for all insertions, this
 * implements support for different insertion styles.
 * The Scintilla settings apply to the current Scintilla
 * document and are thus local to the currently edited
 * buffer or Q-Register.
 *
 * However for the same reason, the ^I command is not
 * fully compatible with classic TECO which \fIalways\fP
 * inserts a single tab character and should not be used
 * for the purpose of inserting single tabs in generic
 * macros.
 * To insert a single tab character reliably, the idioms
 * \(lq9I$\(rq or \(lqI^I$\(rq may be used.
 *
 * Like the I command, ^I has string building characters
 * \fBenabled\fP.
 */
void
StateInsertIndent::initial(void)
{
	StateInsert::initial();

	interface.ssm(SCI_BEGINUNDOACTION);
	if (interface.ssm(SCI_GETUSETABS)) {
		interface.ssm(SCI_ADDTEXT, 1, (sptr_t)"\t");
	} else {
		gint len = interface.ssm(SCI_GETTABWIDTH);

		len -= interface.ssm(SCI_GETCOLUMN,
		                     interface.ssm(SCI_GETCURRENTPOS)) % len;

		gchar spaces[len];

		memset(spaces, ' ', sizeof(spaces));
		interface.ssm(SCI_ADDTEXT, sizeof(spaces), (sptr_t)spaces);
	}
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	if (current_doc_must_undo())
		interface.undo_ssm(SCI_UNDO);
}

} /* namespace SciTECO */
