/* te - tiny emacs */

/*
todo:
- saving
- minibuffer
- save as
- backup files  (at before first save of session)
- autosave  (alert 30?)
- isearch
- prefix commands
- pipe region
- search and replace with pcre2
- xterm title
- movement by paragraphs (can we bind C-up, C-down?)
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <locale.h>

#include <ncurses.h>

#include "vis/array.h"
#include "vis/text.h"
#include "vis/text-motions.h"

#define MIN(a, b)  ((a) > (b) ? (b) : (a))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define CTRL(x) ((x) & 0x1f)
#define ISUTF8(c)   (((c)&0xC0)!=0x80)
#define ISASCII(ch) ((unsigned char)ch < 0x80)

typedef struct {
	const char *file;
	Text *text;
	Mark point;
	Mark mark;
	size_t target_column;
	Array point_history;
} Buffer;

typedef struct {
	Buffer *buf;
	Mark top;
	size_t end;
	int lines, cols;
} View;

char message_buf[128];
char *killring;  // XXX make actual ring


void
view_render(View *view)
{
	int lines = view->lines;
	int cols = view->cols;

	erase();

	size_t point = text_mark_get(view->buf->text, view->buf->point);
	size_t lineno = text_lineno_by_pos(view->buf->text, point);
	size_t bol_point = text_pos_by_lineno(view->buf->text, lineno);

	char buf[lines*cols*4];
	size_t top = view->top;
	size_t len = text_bytes_get(view->buf->text, top, sizeof buf - 1, buf);
	buf[len] = 0;

	int line = 0;
	int col = 0;
	move(0, 0);
	int cur_y = lines, cur_x = cols;
	size_t i;
	for (i = 0; i < len; i++) {
		if (i == point - view->top) {
			getyx(stdscr, cur_y, cur_x);
		}
		if (buf[i] == '\n') {
			getyx(stdscr, line, col);
			move(line + 1, 0);
			if (line == lines - 3)
				break;
		} else {
			getyx(stdscr, line, col);
			if (col == cols - 1) {
				addch('\\');
				if (i == point - view->top) {
					getyx(stdscr, cur_y, cur_x);
				}
			}
			addch((unsigned char)buf[i]);
		}
	}
	view->end = top + i;

	if (point == text_size(view->buf->text)) {
		getyx(stdscr, cur_y, cur_x);
		if (buf[i-1] == '\n') {
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

	mvprintw(lines - 2, 0, "-- %s -- L%ld C%ld B%ld/%ld",
	    view->buf->file,
	    lineno,
	    point - bol_point + 1,
	    point,
	    text_size(view->buf->text)
	);
	mvchgat(lines - 2, 0, view->cols, A_REVERSE, 0, 0);
	mvprintw(lines - 1, 0, "%s", message_buf);

	move(cur_y, cur_x);

	refresh();
}

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

static size_t
mark_column(Buffer *buf, Mark mark)
{
	size_t pos = text_mark_get(buf->text, mark);
	if (pos == EPOS)
		return 0;
	size_t bol = text_line_begin(buf->text, pos);

	return pos - bol;
}

static void
update_target_column(Buffer *buf)
{
	buf->target_column = mark_column(buf, buf->point);
}

void
recenter(View *view)
{
	size_t point = text_mark_get(view->buf->text, view->buf->point);
	ssize_t lineno = text_lineno_by_pos(view->buf->text, point);

	size_t top_lineno = MAX(1, lineno - (view->lines-2)/2);
	view->top = text_pos_by_lineno(view->buf->text, top_lineno);
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
		point = text_line_offset(buf->text, point, buf->target_column);

	buf->point = text_mark_set(buf->text, point);

	if (point < view->top || point > view->end) {
		recenter(view);
	}
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
}

static void
record_undo(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	array_push(&buf->point_history, &point);

	size_t mark = text_mark_get(buf->text, buf->mark);
	array_push(&buf->point_history, &mark);

	text_snapshot(buf->text);
}

void
insert_char(Buffer *buf, int ch)
{
	record_undo(buf);  // TODO debounce

	size_t point = text_mark_get(buf->text, buf->point);
	const char c = {ch};
	text_insert(buf->text, point, &c, 1);

	update_target_column(buf);
}

void
backspace(Buffer *buf)
{
	record_undo(buf);  // TODO debounce

	size_t point = text_mark_get(buf->text, buf->point);
	size_t prev = text_char_prev(buf->text, point);
	if (point == prev) {
		flash();
		return;
	}

	text_delete(buf->text, prev, point - prev);
	buf->point = text_mark_set(buf->text, prev);

	update_target_column(buf);
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
}

void
move_bol(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	point = text_line_begin(buf->text, point);
	buf->point = text_mark_set(buf->text, point);
	update_target_column(buf);
}

void
move_eol(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	point = text_line_end(buf->text, point);
	buf->point = text_mark_set(buf->text, point);
	update_target_column(buf);
}

void
set_mark(Buffer *buf)
{
	// XXX implement mark ring
	buf->mark = buf->point;
	message("Mark set");
}

static void
save_range(Buffer *buf, size_t from, size_t to)
{
	free(killring);
	killring = text_bytes_alloc0(buf->text, from, to - from);
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
	cbreak();  // undo halfdelay

	buf->point = point;
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
		point = text_line_offset(view->buf->text, point, view->buf->target_column);
		view->buf->point = text_mark_set(view->buf->text, point);
	}
}

void
beginning_of_buffer(View *view)
{
	view->buf->point = text_mark_set(view->buf->text, 0);
	view->top = 0;
}

void
end_of_buffer(View *view)
{
	view->buf->point = text_mark_set(view->buf->text,
	    text_size(view->buf->text));
	recenter(view);   // XXX improve
}

void
undo(Buffer *buf)
{
	size_t u = text_undo(buf->text);
	if (u == EPOS) {
		alert("No further undo information");
		return;
	}

	size_t mark = *(size_t *)array_pop(&buf->point_history);
	buf->mark = text_mark_set(buf->text, mark);

	size_t point = *(size_t *)array_pop(&buf->point_history);
	buf->point = text_mark_set(buf->text, point);

	// XXX for redo, need to preserve cursors somewhere.

	// XXX how to do emacs-style undo tree? text_earlier?

	message("Undo");
}

void
yank(Buffer *buf)
{
	size_t point = text_mark_get(buf->text, buf->point);
	if (killring) {
		// XXX NUL-byte safety.
		size_t len = strlen(killring);
		text_insert(buf->text, point, killring, len);

		buf->point = text_mark_set(buf->text, point + len);
	}
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

	save_range(buf, point, eol);

	text_delete(buf->text, point, eol - point);
	buf->point = text_mark_set(buf->text, point);
}

void
exchange_point_mark(Buffer *buf)
{
	Mark t = buf->mark;
	buf->mark = buf->point;
	buf->point = t;
}

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");  // XXX force UTF-8 somehow for ncurses to work

	Text *text = text_load(argc == 2 ? argv[1] : "README.md");
	Buffer *buf = malloc (sizeof *buf);
	View *view = malloc (sizeof *view);

	buf->file = "README.md";
	buf->text = text;
	buf->point = buf->mark = text_mark_set(text, 0);
	buf->target_column = 0;
	array_init_sized(&buf->point_history, sizeof (Mark));

	view->buf = buf;
	view->top = 0;
	view->end = 0;
	getmaxyx(stdscr, view->lines, view->cols);
	message("");

	initscr();
	raw();
	noecho();
	nonl();
        cbreak();
	keypad(stdscr, TRUE);
	meta(stdscr, TRUE);

	int quit = 0;
	while (!quit) {
		getmaxyx(stdscr, view->lines, view->cols);

		view_render(view);
		message("");

		int ch = getch();
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
			delete(view->buf);
			break;
		case CTRL('e'):
			move_eol(view->buf);
			break;
		case CTRL('f'):
		case KEY_RIGHT:
			move_char(view->buf, +1);
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
		case CTRL('p'):
		case KEY_UP:
			move_line(view, -1);
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
		case CTRL('_'):
			undo(view->buf);
			break;
		case KEY_BACKSPACE:
			backspace(view->buf);
			break;
		case CTRL('x'):
			{
				int ch2 = getch();
				switch(ch2) {
				case CTRL('c'):
					quit = 1;
					break;
				case CTRL('s'):
					message("NYI");
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
				nodelay(stdscr, TRUE);
				int ch2 = getch();
				nodelay(stdscr, FALSE);

				switch (ch2) {
				case '<':
					beginning_of_buffer(view);
					break;
				case '>':
					end_of_buffer(view);
					break;
				case 'v':
					view_scroll(view, -(view->lines-2-2));
					break;
				case 'w':
					kill_region_save(view);
					break;
				default:
					message("unknown key M-%d", ch2);
					break;
				}
			}
			break;
		default:
			if (0x20 <= ch && ch <= 0x7f) {
				insert_char(view->buf, ch);
			} else if (ch <= 0xff && ISUTF8(ch)) {
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
				alert("unknown key %d", ch);
			}
			break;
		}
	}

	endwin();

	return 0;
}
