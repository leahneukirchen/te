/* te - tiny emacs */

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <curses.h>
#define KEY_DEL 0177

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "vis/text.h"
#include "vis/text-motions.h"
size_t text_undo_emacs(Text *txt, int n);

#define MIN(a, b)  ((a) > (b) ? (b) : (a))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define CTRL(x) ((x) & 0x1f)
#define ISUTF8(c)   (((c)&0xC0)!=0x80)
#define ISASCII(ch) ((unsigned char)ch < 0x80)

typedef struct {
	const char *file;
	const char *name;
	Text *text;
	Mark point;
	Mark mark;
	size_t target_column;

	size_t match_start;
	size_t match_end;

	enum {
		ACTION_OTHER,
		ACTION_INSERT,
		ACTION_YANK,
		ACTION_UNDO,
		ACTION_BACKSPACE,
		ACTION_KILL_EOL,
		ACTION_KILL_WORD,
		ACTION_BACKWARD_KILL_WORD
	} last_action;
} Buffer;

typedef struct {
	Buffer *buf;
	Mark top;
	size_t end;
	int lines, cols;
} View;

char message_buf[128];
Text *killring;


void
message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message_buf, sizeof message_buf, fmt, ap);
	va_end(ap);
}

void
alert(const char *fmt, ...)
{
	flash();

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message_buf, sizeof message_buf, fmt, ap);
	va_end(ap);
}

void
window_title(const char *title)
{
	if (title)		/* push and set title */
		dprintf(1, "\e[22t\e]0;te: %s\007", title);
	else			/* pop title stack */
		dprintf(1, "\e[23t");

}

void
view_render(View *view)
{
	Buffer *buf = view->buf;

	int lines = view->lines;
	int cols = view->cols;

missed:
	erase();
	move(0, 0);

	size_t point = text_mark_get(buf->text, buf->point);
	size_t lineno = text_lineno_by_pos(buf->text, point);
	size_t bol_point = text_pos_by_lineno(buf->text, lineno);

	char buffer[lines*cols*4 + 8];
	size_t top = view->top;

	if (point == EPOS) {
		/* we somehow lost track of point, let's keep it visible */
		message("Huh.");
		point = top;
		buf->point = text_mark_set(buf->text, point);
	}

	if ((int)(point - bol_point) > (lines-3)*(cols-1)) {
		top = point - (lines-3)*(cols-1);
		attron(A_REVERSE);
		addstr("...");
		attroff(A_REVERSE);
	}

	size_t len = text_bytes_get(buf->text, top, sizeof buffer - 1, buffer);
	buffer[len] = 0;

	/* This is a bit more complicated than in vi, because emacs
	   higlights closing brackets when the cursor is after them,
	   but opening ones when the cursor is on them. */
	Filerange limits = { view->top, view->top + len };
	int highlight_brackets;
	size_t highlight_point;
	size_t pos_match = text_bracket_match_symbol(buf->text,
	    point - 1, "(){}[]\"'`", &limits);
	if (pos_match != point - 1) {
		highlight_brackets = pos_match < point;   /* opening? */
		highlight_point = point - 1;
	} else {
		pos_match = text_bracket_match_symbol(buf->text,
		    point, "(){}[]\"'`", &limits);
		highlight_brackets = pos_match > point;   /* closing? */
		highlight_point = point;
	}

	int line = 0;
	int col = 0;
	int cur_y = lines, cur_x = cols;
	size_t i;
	mbstate_t mbstate = { 0 };

	if (buf->match_end && buf->match_start < top)
		attron(A_BOLD);

	for (i = 0; i < len; i++) {
		int on_point = i == point - top;
		if (on_point)
			getyx(stdscr, cur_y, cur_x);
		int on_highlight_point = i == highlight_point - top;

		if (i == buf->match_start - top)
			attron(A_BOLD);
		if (i == buf->match_end - top)
			attroff(A_BOLD);

		if (highlight_brackets)
			if (on_highlight_point || i == pos_match - top)
				attron(A_BOLD);

		if (buffer[i] == '\n') {
			getyx(stdscr, line, col);
			move(line + 1, 0);
			if (line == lines - 3)
				break;
		} else {
			getyx(stdscr, line, col);
			if (col == cols - 1) {
				addch('\\');
				move(line + 1, 0);
				if (on_point)
					getyx(stdscr, cur_y, cur_x);
				if (line == lines - 3)
					break;
			}

			if ((unsigned char)buffer[i] >= 0x80) {
				/* detect invalid UTF-8 */
				wchar_t wchar;
				size_t len = mbrtowc(&wchar, buffer+i, 8, &mbstate);
				if (len == (size_t)-1 && errno == EILSEQ) {
					mbstate = (mbstate_t){ 0 };
					attron(A_REVERSE);
					printw("%02x", (unsigned char)buffer[i]);
					attroff(A_REVERSE);
					continue;
				} else if (len == (size_t)-2) {
					/* I think -2 can't happen here? */
					abort();
				}

				/* UTF-8 is valid, print at once */
				addnstr(buffer+i, len);
				i += len - 1;
			} else if (buffer[i] != '\t' && 0x00 <= buffer[i] && buffer[i] < 0x20) {
				attron(A_BOLD);
				addch('^');
				addch('@' + buffer[i]);
				attroff(A_BOLD);
			} else if (buffer[i] == 0x7f) {
				attron(A_BOLD);
				addch('^');
				addch('?');
				attroff(A_BOLD);
			} else {
				addch((unsigned char)buffer[i]);
			}
		}

		if (highlight_brackets)
			if (on_highlight_point || i == pos_match - top)
				attroff(A_BOLD);
	}
	view->end = top + i;

	attroff(A_BOLD);

	if (point > view->end) {
		/* When lots of line wrapping happened, we may not have reached
		   point yet.  move view->top down 10 lines and try again. */
		size_t top_lineno = text_lineno_by_pos(buf->text, top);
		view->top = text_pos_by_lineno(buf->text, top_lineno + 10);

		goto missed;
	}

	if (point == text_size(buf->text)) {
		getyx(stdscr, cur_y, cur_x);
		if (buffer[i-1] == '\n') {
			addch(' ');
			line++;
		} else {
			// higlight non-EOL file end
			addstr("\xE2\x97\x8A");  // U+25CA LOZENGE
		}
	}

	line++;
	for (; line < lines - 2; line++) {
		mvprintw(line, 0, "~");
	}

	mvprintw(lines - 2, 0, "--%s- %s -- L%ld C%ld B%ld/%ld",
	    text_modified(buf->text) ? "**" : "--",
	    buf->name,
	    lineno,
	    point - bol_point + 1,
	    point,
	    text_size(buf->text)
	);
	mvchgat(lines - 2, 0, view->cols, A_REVERSE, 0, 0);
	mvprintw(lines - 1, 0, "%s", message_buf);

	move(cur_y, cur_x);

	refresh();
}

static void
update_target_column(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	buf->target_column = text_line_width_get(buf->text, point);
}

void
recenter(View *view)
{
	Buffer *buf = view->buf;

	size_t point = text_mark_get(buf->text, buf->point);
	ssize_t lineno = text_lineno_by_pos(buf->text, point);

	size_t top_lineno = MAX(1, lineno - (view->lines-2)/2);
	view->top = text_pos_by_lineno(buf->text, top_lineno);

	buf->last_action = ACTION_OTHER;
}

void
move_line(View *view, int off)
{
	Buffer *buf = view->buf;

	size_t point = text_mark_get(buf->text, buf->point);

	while (off) {
		size_t old_point = point;

		if (off > 0) {
			off--;
			point = text_line_down(buf->text, point);
		} else {
			off++;
			point = text_line_up(buf->text, point);
		}

		if (point == old_point) {
			flash();
			break;
		}
	}

	if (buf->target_column)
		point = text_line_width_set(buf->text, point, buf->target_column);

	buf->point = text_mark_set(buf->text, point);

	if (point < view->top || point > view->end) {
		recenter(view);
	}

	buf->last_action = ACTION_OTHER;
}

void
move_char(Buffer *buf, int off)
{
	size_t point = text_mark_get(buf->text, buf->point);

	while (off) {
		size_t old_point = point;

		if (off > 0) {
			off--;
			point = text_char_next(buf->text, point);
		} else {
			off++;
			point = text_char_prev(buf->text, point);
		}

		if (point == old_point) {
			flash();
			break;
		}
	}

	buf->point = text_mark_set(buf->text, point);
	update_target_column(buf);

	buf->last_action = ACTION_OTHER;
}

void
move_paragraph(Buffer *buf, int off)
{
	size_t point = text_mark_get(buf->text, buf->point);

	while (off) {
		size_t old_point = point;

		if (off > 0) {
			off--;
			point = text_paragraph_next(buf->text, point);
		} else {
			off++;
			point = text_paragraph_prev(buf->text, point);
		}

		if (point == old_point) {
			flash();
			break;
		}
	}

	buf->point = text_mark_set(buf->text, point);

	buf->last_action = ACTION_OTHER;

}

static void
record_undo(Buffer *buf)
{
	text_snapshot(buf->text);
}

void
insert_char(Buffer *buf, int ch)
{
	if (buf->last_action != ACTION_INSERT)
		record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	const char c = ch;
	text_insert(buf->text, point, &c, 1);

	update_target_column(buf);

	buf->last_action = ACTION_INSERT;
}

void
backspace(Buffer *buf)
{
	if (buf->last_action != ACTION_BACKSPACE)
		record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	size_t prev = text_char_prev(buf->text, point);
	if (point == prev) {
		flash();
		return;
	}

	text_delete(buf->text, prev, point - prev);
	buf->point = text_mark_set(buf->text, prev);

	update_target_column(buf);

	buf->last_action = ACTION_BACKSPACE;
}

void
delete(Buffer *buf)
{
	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	size_t next = text_char_next(buf->text, point);
	if (point == next) {
		flash();
		return;
	}

	text_delete(buf->text, point, next - point);
	buf->point = text_mark_set(buf->text, point);

	buf->last_action = ACTION_OTHER;
}

void
move_bol(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	point = text_line_begin(buf->text, point);
	buf->point = text_mark_set(buf->text, point);
	update_target_column(buf);

	buf->last_action = ACTION_OTHER;
}

void
move_eol(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	point = text_line_end(buf->text, point);
	buf->point = text_mark_set(buf->text, point);
	update_target_column(buf);

	buf->last_action = ACTION_OTHER;
}

void
set_mark(Buffer *buf)
{
	// XXX implement mark ring
	buf->mark = buf->point;
	message("Mark set");

	buf->last_action = ACTION_OTHER;
}

static void
save_range(Buffer *buf, size_t from, size_t to, int pend)
{
	size_t len = to - from;
	char *killstr = malloc(len);
	if (!killstr)
		return;
	text_bytes_get(buf->text, from, len, killstr);

	if (pend == 0) {  /* replace */
		text_snapshot(killring);
		text_delete(killring, 0, text_size(killring));
		text_insert(killring, 0, killstr, len);
	} else if (pend == 1) { /* append */
		text_insert(killring, text_size(killring), killstr, len);
	} else if (pend == -1) { /* prepend */
		text_insert(killring, 0, killstr, len);
	}

	free(killstr);
}


static void
save_region(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	size_t mark = text_mark_get(buf->text, buf->mark);
	if (mark > point) {
		size_t t = point;
		point = mark;
		mark = t;
	}

	save_range(buf, mark, point, 0);
}

void
kill_region(Buffer *buf)
{
	record_undo(buf);

	save_region(buf);

	if (buf->mark == buf->point)
		return;

	size_t point = text_mark_get(buf->text, buf->point);
	size_t mark = text_mark_get(buf->text, buf->mark);
	if (mark > point) {
		size_t t = point;
		point = mark;
		mark = t;
	}

	Filerange range = { mark, point };
	text_delete_range(buf->text, &range);

	buf->point = buf->mark = text_mark_set(buf->text, mark);
	update_target_column(buf);

	buf->last_action = ACTION_OTHER;
}

void
kill_region_save(View *view)
{
	Buffer *buf = view->buf;

	save_region(buf);

	if (buf->mark == buf->point)
		return;

	/* animate cursor */
	Mark point = buf->point;
	buf->point = buf->mark;
	view_render(view);

	halfdelay(5);
	int ch = getch();
	if (ch != ERR)
		ungetch(ch);
	nocbreak();  // undo halfdelay
	raw();

	buf->point = point;

	buf->last_action = ACTION_OTHER;
}

void
view_scroll(View *view, int off)
{
	size_t top = view->top;
	ssize_t lineno = text_lineno_by_pos(view->buf->text, top);

	if (lineno == 1 && off < 0) {
		size_t point = text_mark_get(view->buf->text, view->buf->point);
		if (point == 0)
			alert("Beginning of buffer");
		else {
			view->buf->point = text_mark_set(view->buf->text, 0);
			update_target_column(view->buf);
		}
		return;
	}

	lineno += off;
	if (lineno < 1)
		lineno = 1;
	view->top = text_pos_by_lineno(view->buf->text, lineno);
	if (view->top == EPOS) {
		size_t point = text_mark_get(view->buf->text, view->buf->point);
		if (point == text_size(view->buf->text)) {
			alert("End of buffer");
		} else {
			view->buf->point = text_mark_set(view->buf->text,
			    text_size(view->buf->text));
		}
		update_target_column(view->buf);
		view->top = top;   // restore
		return;
	}

	view_render(view);  // compute end

	size_t point = text_mark_get(view->buf->text, view->buf->point);

	if (off > 0 && point < view->top) {  // scrolled down too much
		view->buf->point = text_mark_set(view->buf->text, view->top);
		update_target_column(view->buf);
	} else if (off < 0 && view->end < point) {  // scrolled up too much
		view->buf->point = text_mark_set(view->buf->text,
		    text_line_start(view->buf->text, view->end));
		update_target_column(view->buf);
	} else if (view->buf->target_column) {  // keep column on same page
		point = text_line_width_set(view->buf->text, point, view->buf->target_column);
		view->buf->point = text_mark_set(view->buf->text, point);
	}
}

void
beginning_of_buffer(View *view)
{
	Buffer *buf = view->buf;

	set_mark(buf);

	buf->point = text_mark_set(buf->text, 0);
	view->top = 0;

	buf->last_action = ACTION_OTHER;
}

void
end_of_buffer(View *view)
{
	Buffer *buf = view->buf;

	set_mark(buf);

	buf->point = text_mark_set(buf->text, text_size(buf->text));

	ssize_t lineno = text_lineno_by_pos(buf->text, text_size(buf->text));

	size_t top_lineno = MAX(1, lineno - (view->lines-3));
	view->top = text_pos_by_lineno(buf->text, top_lineno);

	buf->last_action = ACTION_OTHER;
}

void
undo(Buffer *buf)
{
	static int n;

	if (buf->last_action == ACTION_UNDO)
		n++;
	else
		n = 0;

	size_t u = text_undo_emacs(buf->text, n);

	record_undo(buf);

	buf->mark = 0;
	buf->point = text_mark_set(buf->text, u == EPOS ? 0 : u);

	if (u == EPOS)
		message("No further undo information");
	else
		message("Undo");

	buf->last_action = ACTION_UNDO;
}

void
yank(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);

	/* reset killring to latest addition */
	while (text_later(killring) != EPOS)
		;

	if (text_size(killring)) {
		record_undo(buf);

		size_t len = text_size(killring);
		char *killstr = malloc(len);
		if (!killstr)
			return;
		text_bytes_get(killring, 0, len, killstr);

		text_insert(buf->text, point, killstr, len);

		buf->mark = text_mark_set(buf->text, point);
		buf->point = text_mark_set(buf->text, point + len);
	}

	buf->last_action = ACTION_YANK;
}

void
yank_pop(Buffer *buf)
{
	if (buf->last_action != ACTION_YANK) {
		alert("Previous command was not a yank");
		return;
	}

	/* ring buffer */
	if (text_earlier(killring) == EPOS)
		while (text_later(killring) != EPOS)
			;
	if (!text_modified(killring))   /* skip the empty initial buffer */
		while (text_later(killring) != EPOS)
			;

	if (!text_size(killring))
		return;

	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	/* previous insertion point */
	size_t mark = text_mark_get(buf->text, buf->mark);

	size_t len = text_size(killring);
	char *killstr = malloc(len);
	if (!killstr)
		return;
	text_bytes_get(killring, 0, len, killstr);

	text_delete(buf->text, mark, point - mark);
	point = mark;
	text_insert(buf->text, point, killstr, len);

	buf->mark = text_mark_set(buf->text, point);
	buf->point = text_mark_set(buf->text, point + len);

	buf->last_action = ACTION_YANK; /* ! */
}

void
kill_eol(Buffer *buf)
{
	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	size_t bol = text_line_start(buf->text, point);
	size_t eol = text_line_end(buf->text, point);

	if (point == bol || point == eol)
		eol = text_line_next(buf->text, point);  // kill entire line

	save_range(buf, point, eol, buf->last_action == ACTION_KILL_EOL);

	text_delete(buf->text, point, eol - point);
	buf->point = text_mark_set(buf->text, point);

	buf->last_action = ACTION_KILL_EOL;
}

void
exchange_point_mark(Buffer *buf)
{
	Mark t = buf->mark;
	buf->mark = buf->point;
	buf->point = t;

	buf->last_action = ACTION_OTHER;
}

void
save(Buffer *buf)
{
	if (!buf->file) {
		alert("Buffer has no filename.");
		return;
	}

	if (text_save_method(buf->text, buf->file, TEXT_SAVE_ATOMIC)) {
		message("Wrote %s", buf->file);
	} else {
		alert("ERROR: Saving failed! %s: %s", buf->file, strerror(errno));
	}

	buf->last_action = ACTION_OTHER;
}

void
background(View *view)
{
	view->buf->last_action = ACTION_OTHER;

	endwin();
	raise(SIGSTOP);
	view_render(view);
}

char *
minibuffer_read(View *view, const char *prompt, const char *prefill)
{
	// XXX use proper text for buffer, allow movement

	static char buf[1024];
	strncpy(buf, prefill, sizeof buf);  // want zeroing

	int done = 0;
	while (!done) {
		move(view->lines - 1, 0);
		clrtoeol();
		printw("%s %s", prompt, buf);
		refresh();

		int ch = getch();
		switch (ch) {
		case CTRL('j'):
		case CTRL('m'):
			done = 1;
			break;
		case CTRL('s'):
			done = 1;
			ungetch(CTRL('s'));  // XXX HACK
			break;
		case KEY_BACKSPACE:
		case KEY_DEL:
			{
				size_t l = strlen(buf);
				if (l)
					buf[l-1] = 0;
			}
			break;
		case CTRL('g'):
			alert("Quit");
			return 0;
		default:
			if (0x20 <= ch && ch < 0x7f) {
				size_t l = strlen(buf);
				buf[l] = ch;
				buf[l+1] = 0;
			} else {
				message("unknown key %d", ch);
				view_render(view);
				sleep(1);
			}
		}
	}

	return buf;
}

void
save_as(View *view)
{
	Buffer *buf = view->buf;
	char *new_file;
	new_file = minibuffer_read(view, "Write file:", buf->file ? buf->file : "");
	if (!new_file)
		return;

	buf->file = strdup(new_file);
	save(view->buf);
}

int
yes_or_no_p(View *view, const char *question)
{
	while (1) {
		char *answer = minibuffer_read(view, question, "");
		message("");
		if (!answer || strcasecmp(answer, "no") == 0)
			return 0;
		if (strcasecmp(answer, "yes") == 0)
			return 1;
		alert("Please answer yes or no.");
		view_render(view);
		sleep(1);
	}
}

int quit;

void
want_quit(View *view) {
	if (text_modified(view->buf->text)) {
		if (yes_or_no_p(view,
		    "Modified buffers exist; really exit? (yes or no)"))
			quit = 1;
	} else {
		quit = 1;
	}

	view->buf->last_action = ACTION_OTHER;
}

void
quoted_insert(Buffer *buf)
{
	int ch = getch();
	if (ch == ERR)
		return;

	if (0x00 <= ch && ch <= 0x7f) {
		// insert any ASCII byte raw
		insert_char(buf, ch);
	} else if (ch == KEY_BACKSPACE) {
		insert_char(buf, 0177);
	} else if (0x80 <= ch && ch <= 0xff) {
		// let default insert deal with UTF-8 etc.
		ungetch(ch);
	} else {
		alert("Not an ASCII byte");
	}

	buf->last_action = ACTION_OTHER;
}

void
transpose_chars(Buffer *buf)
{
	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);

	char b = 0;
	text_byte_get(buf->text, point, &b);
	if (b == '\n')
		point = text_char_prev(buf->text, point);

	/* [prev].[next] -> [next].[prev] */

	size_t prev = text_char_prev(buf->text, point);
	size_t next = text_char_next(buf->text, point);

	char prevbuf[4], nextbuf[4];
	size_t prevlen, nextlen;
	// XXX assert lengths
	prevlen = text_bytes_get(buf->text, prev, point - prev, prevbuf);
	nextlen = text_bytes_get(buf->text, point, next - point, nextbuf);

	text_delete(buf->text, prev, prevlen + nextlen);
	text_insert(buf->text, prev, nextbuf, nextlen);
	text_insert(buf->text, prev + nextlen, prevbuf, prevlen);

	point = prev + nextlen;
	text_byte_get(buf->text, point, &b);
	if (b == '\n')
		point = text_char_next(buf->text, point);

	buf->point = text_mark_set(buf->text, point);

	buf->last_action = ACTION_OTHER;
}

static int
isword(unsigned char c)
{
	return (
	    c == '$' ||
	    c == '%' ||
	    c == '\'' ||
	    ('0' <= c && c <= '9') ||
	    ('A' <= c && c <= 'Z') ||
	    ('a' <= c && c <= 'z') ||
	    0x80 <= c   /* simplification */
	);
}

void
backward_word(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);

	char c;
	Iterator it = text_iterator_get(buf->text, point);
	while (text_iterator_char_prev(&it, &c) && !isword((unsigned char)c))
		;
	while (text_iterator_char_prev(&it, &c) && isword((unsigned char)c))
		;
	text_iterator_char_next(&it, &c);

	buf->point = text_mark_set(buf->text, it.pos);

	buf->last_action = ACTION_OTHER;
}

void
forward_word(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);

	char c;
	Iterator it = text_iterator_get(buf->text, point);
	while (text_iterator_char_next(&it, &c) && !isword((unsigned char)c))
		;
	while (text_iterator_char_next(&it, &c) && isword((unsigned char)c))
		;

	buf->point = text_mark_set(buf->text, it.pos);

	buf->last_action = ACTION_OTHER;
}

void
kill_word(Buffer *buf)
{
	int append = buf->last_action == ACTION_KILL_WORD;

	record_undo(buf);

	size_t from = text_mark_get(buf->text, buf->point);
	forward_word(buf);
	size_t to = text_mark_get(buf->text, buf->point);

	save_range(buf, from, to, append);
	text_delete(buf->text, from, to - from);

	buf->point = text_mark_set(buf->text, from);

	buf->last_action = ACTION_KILL_WORD;
}

void
backward_kill_word(Buffer *buf)
{
	int prepend = buf->last_action == ACTION_BACKWARD_KILL_WORD;

	record_undo(buf);

	size_t to = text_mark_get(buf->text, buf->point);
	backward_word(buf);
	size_t from = text_mark_get(buf->text, buf->point);

	save_range(buf, from, to, -prepend);
	text_delete(buf->text, from, to - from);

	buf->point = text_mark_set(buf->text, from);

	buf->last_action = ACTION_BACKWARD_KILL_WORD;
}

void
capitalize_word(Buffer *buf)
{
	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);

	Iterator it = text_iterator_get(buf->text, point);
	char c;
	text_iterator_byte_get(&it, &c);
	while (!isword((unsigned char)c))
		text_iterator_char_next(&it, &c);

	if ('a' <= c && c <= 'z') {   // XXX ASCII only
		c &= ~0x20;
		text_delete(buf->text, it.pos, 1);
		text_insert(buf->text, it.pos, &c, 1);
	}

	while (text_iterator_char_next(&it, &c) && isword((unsigned char)c)) {
		if ('A' <= c && c < 'Z') {
			c |= 0x20;
			text_delete(buf->text, it.pos, 1);
			text_insert(buf->text, it.pos, &c, 1);
		}
	}

	buf->point = text_mark_set(buf->text, it.pos);

	buf->last_action = ACTION_OTHER;
}

void
just_one_space(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);

	record_undo(buf);

	char cn, cp;
        Iterator it = text_iterator_get(buf->text, point);
	text_iterator_char_prev(&it, &cp);
	text_iterator_char_next(&it, &cn);

	if (cn != ' ' && cn != '\t' &&
	    cp != ' ' && cp != '\t') { /* insert just one space */
		point = it.pos;
	} else {
		if (cp == ' ' || cp == '\t') {
			while (cp == ' ' || cp == '\t')
				text_iterator_char_prev(&it, &cp);
			text_iterator_char_next(&it, &cp);
		}

		point = it.pos;

		while (text_iterator_char_next(&it, &cn) &&
		    (cn == ' ' || cn == '\t'))
			;

		text_delete(buf->text, point, it.pos - point);
	}

	text_insert(buf->text, point, " ", 1);
	buf->point = text_mark_set(buf->text, point + 1);

	buf->last_action = ACTION_OTHER;
}

void
magic_tab(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	size_t pointbegin = text_line_begin(buf->text, point);
	size_t pointstart = text_line_start(buf->text, pointbegin);

	size_t prev = text_line_prev(buf->text, point);
	size_t prevbegin = text_line_begin(buf->text, prev);
	size_t prevend = text_line_end(buf->text, prev);

	/* skip empty lines backward */
	while (prevbegin == prevend) {
		prev = text_line_prev(buf->text, prev);
		prevbegin = text_line_begin(buf->text, prev);
		prevend = text_line_end(buf->text, prev);
	}

	size_t prevstart = text_line_start(buf->text, prevbegin);

	/* if prev line has no indent,
	   or if cursor is at beginning of indent (but not at beginning of line)
	   -> then forcibly indent
	*/
	if ((prevstart == prevbegin && point == pointbegin) ||
	    (pointbegin != pointstart && point == pointstart)) {
		if (point < pointstart)
			point = pointstart;
		buf->point = text_mark_set(buf->text, point);

		insert_char(buf, '\t');
		return;
	}

	/* else reindent the line by copying whitespace of previous line */

	if (point < pointstart)
		point = pointstart;
	buf->point = text_mark_set(buf->text, point);

	char *indent = text_bytes_alloc0(buf->text,
	    prevbegin, prevstart - prevbegin);
	char *old_indent = text_bytes_alloc0(buf->text,
	    pointbegin, pointstart - pointbegin);

	if (strcmp(indent, old_indent) != 0) {
		record_undo(buf);

		text_delete(buf->text, pointbegin, pointstart - pointbegin);
		text_insert(buf->text, pointbegin, indent, strlen(indent));

		update_target_column(buf);
	}

	free(indent);
	free(old_indent);

	buf->last_action = ACTION_OTHER;
}

void
open_line(Buffer *buf)
{
	record_undo(buf);

	size_t point = text_mark_get(buf->text, buf->point);
	size_t bol = text_line_start(buf->text, point);
	size_t sol = text_line_begin(buf->text, point);
	size_t eol = text_line_end(buf->text, point);

	text_insert(buf->text, point, "\n", 1);

	if (point != eol && sol != bol) {
		/* indent new line below as this one */
		buf->point = text_mark_set(buf->text, point + 1);
		magic_tab(buf);
	}

	buf->point = text_mark_set(buf->text, point);

	buf->last_action = ACTION_OTHER;
}

void
insert_byte(View *view)
{
	char *answer = minibuffer_read(view, "Insert byte (hex):", "");
	if (!answer || !*answer)
		return;

	char *rest;
	long byte = strtol(answer, &rest, 16);
	if (*rest != 0 || byte < 0 || byte > 0xff) {
		alert("Invalid input");
		return;
	}

	insert_char(view->buf, byte);

	view->buf->last_action = ACTION_OTHER;
}

void
goto_line(View *view)
{
	Buffer *buf = view->buf;

	char *answer = minibuffer_read(view, "Goto line:", "");
	if (!answer || !*answer)
		return;

	char *rest;
	long lineno = strtol(answer, &rest, 10);
	if (*rest != 0) {
		alert("Invalid input");
		return;
	}

	size_t point;
	if (lineno <= 0) {
		point = 0;
	} else {
		point = text_pos_by_lineno(buf->text, lineno);
		if (point == EPOS)
			point = text_size(buf->text);
	}
	view->buf->point = text_mark_set(buf->text, point);

	recenter(view);

	buf->last_action = ACTION_OTHER;
}

void
re_search_forward(View *view)
{
	Buffer *buf = view->buf;
	size_t point = text_mark_get(buf->text, buf->point);

	static char search_term[1024];

	char *answer = minibuffer_read(view, "Regexp search:", search_term);
	if (!answer)
		return;
	if (*answer)
		strcpy(search_term, answer);

	// XXX benchmark and check when partial matching becomes useful
	size_t len = text_size(buf->text);
	char *whole_buffer = malloc(len);
	if (!whole_buffer)
		return;
	text_bytes_get(buf->text, 0, len, whole_buffer);

	pcre2_code *re;
	int errornumber;
	size_t erroroffset;

	re = pcre2_compile(
	    (unsigned char *)search_term, /* the pattern */
	    PCRE2_ZERO_TERMINATED, /* indicates pattern is zero-terminated */
	    PCRE2_MULTILINE | PCRE2_UTF | PCRE2_MATCH_INVALID_UTF,  /* default options */
	    &errornumber,          /* for error number */
	    &erroroffset,          /* for error offset */
	    0);

	if (!re) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
		alert("ERROR: %d: %s\n", (int)erroroffset, buffer);
		return;
	}

	pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, 0);

	int rc = pcre2_match(
	    re,                   /* the compiled pattern */
	    (unsigned char *)whole_buffer, /* the subject string */
	    len,                  /* the length of the subject */
	    point,                /* start search at point */
	    PCRE2_NOTEMPTY,       /* default options */
	    match_data,           /* block for storing the result */
	    0);

	if (rc < 0) {
		if (rc == PCRE2_ERROR_NOMATCH) {
			buf->match_start = buf->match_end = 0;
			alert("No match found.");
		} else {
			alert("PCRE2 error %d", rc);
		}
	} else if (rc > 0) {
		size_t *ovector = pcre2_get_ovector_pointer(match_data);

		buf->match_start = ovector[0];
		buf->match_end = ovector[1];
		buf->point = text_mark_set(buf->text, buf->match_end);

		if (view->top > buf->match_end || buf->match_end > view->end)
			recenter(view); //?
	}

	pcre2_match_data_free(match_data);
	pcre2_code_free(re);
	free(whole_buffer);

	buf->last_action = ACTION_OTHER;
}

void
isearch(View *view, int dir)
{
	// XXX keep last search term

	Buffer *buf = view->buf;
	size_t point = text_mark_get(buf->text, buf->point);
	size_t initial_point = point;
	size_t search_point = point;

	static char term[1024];
	memset(term, 0, sizeof term);

	int failed = 0;
	int cur_x, cur_y;

	buf->match_start = buf->match_end = 0;

	while (1) {
		getyx(stdscr, cur_y, cur_x);

		move(view->lines - 1, 0);
		clrtoeol();
		printw("%s%s: %s",
		    failed ? "Failing I-search" : "I-search",
		    dir == +1 ? "" : " backward",
		    term);

		move(cur_y, cur_x);  /* move cursor to point */
		refresh();

		int ch = getch();
		switch (ch) {
		case CTRL('g'):
			alert("Quit");
			buf->point = text_mark_set(buf->text, point);
			buf->match_start = buf->match_end = 0;
			return;
		case CTRL('s'):
			dir = +1;
			if (buf->match_end)
				search_point = buf->match_end + 1;
			break;
		case CTRL('r'):
			dir = -1;
			if (buf->match_end)
				search_point = buf->match_end - 1;
			break;
		case CTRL('u'):
			*term = 0;
			break;
		case KEY_BACKSPACE:
		case KEY_DEL:
			{
				size_t l = strlen(term);
				if (l)
					term[l-1] = 0;
			}
			break;
		default:
			if (0x20 <= ch && ch < 0x7f) {
				size_t l = strlen(term);
				term[l] = ch;
				term[l+1] = 0;
			} else if (ch > 0) {
				buf->mark = text_mark_set(buf->text, initial_point);
				message("Mark saved where search started");
				buf->match_start = buf->match_end = 0;

				if (ch >= 0x80)
					ungetch(ch);
				return;
			}
		}

		size_t found;
again:
		if (strlen(term) > 0) {
			if (dir == +1)
				found = text_find_next(buf->text, search_point, term);
			else
				found = text_find_prev(buf->text, search_point, term);

			if (found == search_point) {
				if (!failed) {
					flash();
					failed = 1;
				} else {
					search_point = dir == +1 ? 0 :
					    text_size(buf->text);
					failed = 0;
					goto again;
				}
			} else {
				failed = 0;
				buf->match_start = found;
				buf->match_end = found + strlen(term);
				buf->point = text_mark_set(buf->text, buf->match_end);
				update_target_column(buf);
			}
		} else {
			buf->match_start = buf->match_end = 0;
			search_point = initial_point;
			buf->point = text_mark_set(buf->text, search_point);
			update_target_column(buf);
		}

		view_render(view);
	}
}

void
shell_command(View *view)
{
	Buffer *buf = view->buf;

	static char last_command[1024];
	static char command_line[1024];

	char *answer = minibuffer_read(view, "Run:", last_command);
	if (!answer)
		return;
	if (*answer)
		strcpy(last_command, answer);

	char *s = last_command;
	char *t = command_line;
	while (*s) {
		if (*s == '%') {
			s++;
			const char *f = buf->file ? buf->file : "";
			while ((*t++ = *f++))
				;
			t--;
		} else {
			*t++ = *s++;
		}
	}

	endwin();
	fprintf(stderr, "\n\n");

	int status = system(command_line);

	if (status > 0)
		fprintf(stderr, "\nshell returned %d\n", WEXITSTATUS(status));
	fprintf(stderr, "\nPress ENTER or type command to continue");

	raw();
	int ch = getch();
	message("ch=%d", ch);
	if (ch != ERR && ch != 13)
		ungetch(ch);

	view_render(view);

	buf->last_action = ACTION_OTHER;
}

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");  // XXX force UTF-8 somehow for ncurses to work
	message("");

	killring = text_load(0);

	const char *file = argc == 2 ? argv[1] : "README.md";
	Text *text = text_load(file);
	if (!text) {
		text = text_load(0);
		if (errno == ENOENT)
			message("(New file)");
		else
			alert("Error opening %s: %s", file, strerror(errno));
	}
	Buffer *buf = malloc (sizeof *buf);
	View *view = malloc (sizeof *view);

	buf->file = file;
	buf->name = strrchr(file, '/');
	if (buf->name)
		buf->name++;
	else
		buf->name = file;
	buf->text = text;
	buf->point = buf->mark = text_mark_set(text, 0);
	buf->target_column = 0;
	buf->match_start = buf->match_end = 0;

	initscr();
	raw();
	noecho();
	nonl();
	keypad(stdscr, TRUE);
	meta(stdscr, TRUE);

	window_title(buf->name);

	/* remove mapping of ^H to backspace, unless ^H is actually set
	   as erase character.  This hack is required because many
	   terminfo entries set kbs=^H even if ^? is send by the terminal. */
	if (erasechar() != CTRL('h'))
		define_key("\b", CTRL('h'));

	view->buf = buf;
	view->top = 0;
	view->end = 0;
	getmaxyx(stdscr, view->lines, view->cols);

	int ch = 0;
	while (!quit) {
		getmaxyx(stdscr, view->lines, view->cols);

		view_render(view);
		message("");
		view->buf->match_start = view->buf->match_end = 0;

		ch = getch();
		switch (ch) {
		case CTRL(' '):
			set_mark(view->buf);
			break;
		case CTRL('a'):
			move_bol(view->buf);
			break;
		case CTRL('b'):
		case KEY_LEFT:
			move_char(view->buf, -1);
			break;
		case CTRL('c'):
			quit = 1;
			break;
		case CTRL('d'):
		case KEY_DC:
			delete(view->buf);
			break;
		case CTRL('e'):
			move_eol(view->buf);
			break;
		case CTRL('f'):
		case KEY_RIGHT:
			move_char(view->buf, +1);
			break;
		case CTRL('g'):
			alert("Quit");
			break;
		case CTRL('i'):
			magic_tab(view->buf);
			break;
		case CTRL('j'):
		case CTRL('m'):
			insert_char(view->buf, '\n');
			break;
		case CTRL('k'):
			kill_eol(view->buf);
			break;
		case CTRL('l'):
			clear();
			recenter(view);
			break;
		case CTRL('n'):
		case KEY_DOWN:
			move_line(view, +1);
			break;
		case CTRL('o'):
			open_line(view->buf);
			break;
		case CTRL('p'):
		case KEY_UP:
			move_line(view, -1);
			break;
		case CTRL('q'):
			quoted_insert(view->buf);
			break;
		case CTRL('r'):
			isearch(view, -1);
			break;
		case CTRL('s'):
			isearch(view, +1);
			break;
		case CTRL('t'):
			transpose_chars(view->buf);
			break;
		case CTRL('v'):
		case KEY_NPAGE:
			view_scroll(view, view->lines-2-2);
			break;
		case KEY_PPAGE:
			view_scroll(view, -((int)view->lines-2-2));
			break;
		case CTRL('w'):
			kill_region(view->buf);
			break;
		case CTRL('y'):
			yank(view->buf);
			break;
		case CTRL('z'):
			background(view);
			break;
		case CTRL('_'):
			undo(view->buf);
			break;
		case KEY_BACKSPACE:
		case KEY_DEL:
			backspace(view->buf);
			break;
		case KEY_HOME:
			beginning_of_buffer(view);
			break;
		case KEY_END:
			end_of_buffer(view);
			break;
		case CTRL('x'):
			{
				int ch2 = getch();
				switch(ch2) {
				case '8':
					insert_byte(view);
					break;
				case 'g':
					goto_line(view);
					break;
				case 'u':
					undo(view->buf);
					break;
				case CTRL('c'):
					want_quit(view);
					break;
				case CTRL('g'):
					alert("Quit");
					break;
				case CTRL('s'):
					save(view->buf);
					break;
				case CTRL('w'):
					save_as(view);
					break;
				case CTRL('x'):
					exchange_point_mark(view->buf);
					break;
				default:
					message("unknown key C-x %d", ch2);
					break;
				}
			}
			break;
		case CTRL('['): // M-...
			{
				int ch2 = getch();

				switch (ch2) {
				case ' ':
					just_one_space(view->buf);
					break;
				case '!':
					shell_command(view);
					break;
				case '<':
					beginning_of_buffer(view);
					break;
				case '>':
					end_of_buffer(view);
					break;
				case '{':
				kUP5:
					move_paragraph(view->buf, -1);
					break;
				case '}':
				kDN5:
					move_paragraph(view->buf, +1);
					break;
				case CTRL('g'):
					alert("Quit");
					break;
				case CTRL('s'):
					re_search_forward(view);
					break;
				case 'b':
				kLFT5:
					backward_word(view->buf);
					break;
				case 'c':
					capitalize_word(view->buf);
					break;
				case 'd':
					kill_word(view->buf);
					break;
				case 'f':
				kRIT5:
					forward_word(view->buf);
					break;
				case 'g':
					goto_line(view);
					break;
				case 'v':
					view_scroll(view, -(view->lines-2-2));
					break;
				case 'w':
					kill_region_save(view);
					break;
				case 'y':
					yank_pop(buf);
					break;
				case KEY_BACKSPACE:
				case KEY_DEL:
					backward_kill_word(view->buf);
					break;
				default:
					message("unknown key M-%d %s",
					    ch2, keyname(ch2));
					break;
				}
			}
			break;
		case KEY_RESIZE:
			/* ignore */
			break;
		default:
			if (ch > KEY_MAX) {
				const char *name = keyname(ch);
				if (!name)
					goto unknown;
				if (strcmp(name, "kUP5") == 0)
					goto kUP5;
				else if (strcmp(name, "kDN5") == 0)
					goto kDN5;
				else if (strcmp(name, "kLFT5") == 0)
					goto kLFT5;
				else if (strcmp(name, "kRIT5") == 0)
					goto kRIT5;
				else
					goto unknown;
			} else if (0x20 <= ch && ch < 0x7f) {
				insert_char(view->buf, ch);
			} else if (ch >= 0x80 && ch <= 0xff && ISUTF8(ch)) {
				insert_char(view->buf, ch);
				nodelay(stdscr, TRUE);
				int ch2;
				while ((ch2 = getch()) != ERR) {
					if (!ISUTF8(ch2))
						insert_char(view->buf, ch2);
					else
						ungetch(ch2);
				}
				nodelay(stdscr, FALSE);

			} else {
			unknown:
				alert("unknown key %d %s", ch, keyname(ch));
			}
			break;
		}
	}

	endwin();
	window_title(0);

	return 0;
}
