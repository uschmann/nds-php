/* dbg.c - debugger panel, drawn into the sub engine bitmap
 *
 * Header, a status line, the code of one file (red dot = breakpoint,
 * tinted row = where the program is paused) and a row of buttons.
 * Tapping a code row toggles its breakpoint.
 */
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "dbg.h"
#include "draw.h"
#include "basic.h"
#include "lang.h"
#include "syntax.h"

extern const u16 basicPalette[16];     /* main.c: colours of the top screen */

#define HDR_H   22
#define STAT_Y  26
#define CODE_Y  38
#define ROW_H   10
#define NROWS   11
#define BTN_Y   156
#define BTN_H   32
#define BTN_W   40
#define NBTN    6

static int file, top;
static int pausedFile = -1, pausedIdx = -1;
static int lastState = -1, lastCount = -1, pressed = -1, touchHeld;
static char lastStatus[40];
static bool dirty;

static const char *btn_label(int b, BasicState st) {
	switch (b) {
	case 0: return st == BASIC_PAUSED ? "CONT" : "RUN";
	case 1: return "STEP";
	case 2: return "OVER";
	case 3: return "OUT";
	case 4: return "STOP";
	default: return "ABC";
	}
}

static bool btn_enabled(int b, BasicState st) {
	switch (b) {
	case 0: return st != BASIC_RUNNING;                  /* RUN / CONT          */
	case 1: return true;                                 /* running: pause      */
	case 2: case 3: return st != BASIC_RUNNING;
	case 4: return st != BASIC_IDLE;
	default: return true;
	}
}

static void draw_btn(int b) {
	BasicState st = basic_state();
	int x = 3 + b * (BTN_W + 2);
	bool en = btn_enabled(b, st);
	u16 face = b == 0 ? COL_ACCENT : COL_CARD;
	if (!en) face = COL_TRAY;
	draw_box(x, BTN_Y, BTN_W, BTN_H, b == pressed ? COL_TINT : face, COL_TRAY, b == pressed);
	const char *l = btn_label(b, st);
	int len = strlen(l);
	u16 tc = !en ? COL_MUTED : draw_label_color(face);
	draw_text(x + BTN_W / 2 - len * 4, BTN_Y + 11 + (b == pressed), l, tc, 1);
}

static void draw_buttons(void) {
	draw_fill(0, BTN_Y - 4, 256, 192 - BTN_Y + 4, COL_TRAY);
	draw_fill(0, BTN_Y - 4, 256, 1, COL_LINE);
	for (int b = 0; b < NBTN; b++) draw_btn(b);
}

static void draw_header(void) {
	draw_fill(0, 0, 256, HDR_H, COL_PAGE);
	draw_fill(4, 3, 16, 16, COL_ACCENT);
	draw_fill(4, 3, 1, 1, COL_PAGE); draw_fill(19, 3, 1, 1, COL_PAGE);
	draw_fill(4, 18, 1, 1, COL_PAGE); draw_fill(19, 18, 1, 1, COL_PAGE);
	draw_glyph(8, 7, '!', C(31, 31, 31), 1);
	draw_text(26, 7, "DEBUG", COL_ACCENT, 1);
	draw_text(26 + 6 * 8, 7, basic_file_name(file), COL_MUTED, 1);
	draw_fill(0, HDR_H - 1, 256, 1, COL_LINE);
}

static void status_text(char *out, int size) {
	BasicState st = basic_state();
	if (st == BASIC_PAUSED && pausedIdx >= 0)
		snprintf(out, size, L("PAUSE Z.%d %s", "PAUSED L%d %s"), pausedIdx + 1, basic_debug_function());
	else if (st == BASIC_RUNNING) snprintf(out, size, "%s", L("L\x84UFT... STEP = PAUSE", "RUNNING... STEP = PAUSE"));
	else if (st == BASIC_INPUT) snprintf(out, size, "%s", L("WARTET AUF EINGABE", "WAITING FOR INPUT"));
	else if (*basic_last_error()) snprintf(out, size, "%s", basic_last_error());
	else snprintf(out, size, "%s", L("ZEILE ANTIPPEN = HALTEPUNKT", "TAP A LINE = BREAKPOINT"));
	out[30] = 0;
}

static void draw_status(void) {
	char s[64];
	status_text(s, sizeof s);
	strcpy(lastStatus, s);
	draw_fill(0, HDR_H, 256, CODE_Y - HDR_H - 2, COL_PAGE);
	bool err = basic_state() == BASIC_IDLE && *basic_last_error();
	draw_text(6, STAT_Y, s, err || basic_state() == BASIC_PAUSED ? COL_ACCENT : COL_MUTED, 1);
}

static void draw_row(int r) {
	int idx = top + r, y = CODE_Y + r * ROW_H;
	int n = basic_file_line_count(file);
	bool here = basic_state() == BASIC_PAUSED && file == pausedFile && idx == pausedIdx;
	u16 bg = here ? COL_TINT : COL_CARD;
	draw_fill(0, y, 256, ROW_H, bg);
	if (idx >= n) return;
	if (basic_breakpoint(file, idx)) {
		draw_fill(3, y + 2, 6, 6, COL_ACCENT);
		draw_fill(3, y + 2, 1, 1, bg); draw_fill(8, y + 2, 1, 1, bg);
		draw_fill(3, y + 7, 1, 1, bg); draw_fill(8, y + 7, 1, 1, bg);
	}
	char num[16];
	snprintf(num, sizeof num, "%3d", (idx + 1) % 1000);
	draw_text(11, y + 1, num, here ? COL_ACCENT : COL_MUTED, 1);
	if (here) draw_glyph(35, y + 1, 0x1a, COL_ACCENT, 1);      /* arrow */
	/* the code with the editor's colours (palette of the top screen) */
	const char *t = basic_file_line_text(file, idx);
	int l = (int)strlen(t);
	unsigned char col[BASIC_LINE_MAX + 1];
	syntax_highlight(t, l, col);
	for (int i = 0; i < l && i < 26; i++)
		if (t[i] != ' ') draw_glyph(44 + i * 8, y + 1, (unsigned char)t[i], basicPalette[col[i] & 15] | BIT(15), 1);
}

static void draw_code(void) {
	draw_fill(0, CODE_Y - 2, 256, 2, COL_CARD);
	draw_fill(0, CODE_Y + NROWS * ROW_H, 256, BTN_Y - 4 - CODE_Y - NROWS * ROW_H, COL_CARD);
	for (int r = 0; r < NROWS; r++) draw_row(r);
}

static void draw_all(void) {
	draw_fill(0, 0, 256, 192, COL_PAGE);
	draw_header();
	draw_status();
	draw_code();
	draw_buttons();
	dirty = false;
}

static void keep_visible(int idx) {
	if (idx < top) top = idx;
	if (idx >= top + NROWS) top = idx - NROWS / 2;
	int n = basic_file_line_count(file);
	if (top > n - NROWS) top = n - NROWS;
	if (top < 0) top = 0;
}

void dbgp_open(int f) {
	file = f;
	top = 0;
	pressed = -1;
	touchHeld = 1;               /* ignore the tap that opened the panel */
	lastState = basic_state();
	lastCount = basic_file_line_count(file);
	int pf, pi;
	if (basic_debug_location(&pf, &pi)) { pausedFile = file = pf; pausedIdx = pi; keep_visible(pi); }
	else pausedFile = pausedIdx = -1;
	draw_all();
}

void dbgp_update(void) {
	BasicState st = basic_state();
	int pf = -1, pi = -1;
	basic_debug_location(&pf, &pi);
	if (st == BASIC_PAUSED && (pf != pausedFile || pi != pausedIdx || st != lastState)) {
		/* paused somewhere new: show that file and line */
		bool other = pf != file;
		pausedFile = pf;
		pausedIdx = pi;
		file = pf;
		keep_visible(pi);
		if (other) draw_header();
		dirty = true;
	} else if (st != BASIC_PAUSED && pausedIdx >= 0) {
		pausedFile = pausedIdx = -1;
		dirty = true;
	}
	if (st != lastState) { lastState = st; draw_buttons(); dirty = true; }
	if (basic_file_line_count(file) != lastCount) { lastCount = basic_file_line_count(file); dirty = true; }
	char s[64];
	status_text(s, sizeof s);
	if (strcmp(s, lastStatus)) draw_status();
	if (dirty) { draw_code(); dirty = false; }
}

static int hit_button(int px, int py) {
	if (py < BTN_Y || py >= BTN_Y + BTN_H) return -1;
	for (int b = 0; b < NBTN; b++) {
		int x = 3 + b * (BTN_W + 2);
		if (px >= x && px < x + BTN_W) return b;
	}
	return -1;
}

static const int btnAction[NBTN] = { DBA_RUN, DBA_STEP, DBA_OVER, DBA_OUT, DBA_STOP, DBA_CLOSE };

int dbgp_poll(bool touching, int px, int py, u32 down, u32 rep) {
	int act = DBA_NONE;
	BasicState st = basic_state();
	if (touching) {
		if (!touchHeld) {
			touchHeld = 1;
			int b = hit_button(px, py);
			if (b >= 0) {
				if (btn_enabled(b, st)) { pressed = b; draw_btn(b); act = btnAction[b]; }
			} else if (py >= CODE_Y && py < CODE_Y + NROWS * ROW_H) {
				int r = (py - CODE_Y) / ROW_H, idx = top + r;
				if (idx < basic_file_line_count(file)) { basic_toggle_breakpoint(file, idx); draw_row(r); }
			}
		}
	} else {
		touchHeld = 0;
		if (pressed >= 0) { int b = pressed; pressed = -1; draw_btn(b); }
	}
	int n = basic_file_line_count(file);
	if ((rep & KEY_UP) && top > 0) { top--; draw_code(); }
	if ((rep & KEY_DOWN) && top < n - NROWS) { top++; draw_code(); }
	if (act == DBA_NONE) {
		if ((down & KEY_A) && btn_enabled(1, st)) act = DBA_STEP;
		else if ((down & KEY_X) && btn_enabled(2, st)) act = DBA_OVER;
		else if ((down & KEY_Y) && btn_enabled(3, st)) act = DBA_OUT;
		else if ((down & KEY_START) && st != BASIC_RUNNING) act = DBA_RUN;
		else if ((down & KEY_START) && st == BASIC_RUNNING) act = DBA_STOP;
		else if (down & KEY_B) act = DBA_CLOSE;
	}
	return act;
}
