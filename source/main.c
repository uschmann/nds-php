/*
 * NDS PHP - main.c
 *
 * Top screen:    program listing with line numbers + input line (edit view)
 *                or the program output terminal (output view)
 * Bottom screen: retro on-screen keyboard (kbd.c)
 */
#include <nds.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fat.h>

#include "basic.h"
#include "kbd.h"
#include "help.h"
#include "spred.h"
#include "sound.h"
#include "snded.h"
#include "title.h"
#include "hwspr.h"
#include "browser.h"
#include "lang.h"

#define COLS 32
#define ROWS 24

/* light, friendly 16 colour palette (5 bit per channel); the names keep
 * the classic meaning, so color(2) is still red */
const u16 basicPalette[16] = {
	RGB15(3, 3, 3),    RGB15(31, 31, 31), RGB15(30, 6, 0),   RGB15(1, 18, 22),
	RGB15(15, 7, 29),  RGB15(2, 20, 9),   RGB15(4, 12, 29),  RGB15(29, 22, 1),
	RGB15(31, 14, 2),  RGB15(18, 8, 1),   RGB15(31, 14, 14), RGB15(7, 7, 7),
	RGB15(14, 13, 13), RGB15(9, 27, 16),  RGB15(12, 20, 31), RGB15(28, 28, 27)
};
enum { BLACK, WHITE, RED, CYAN, PURPLE, GREEN, BLUE, YELLOW, ORANGE, BROWN,
       LRED, DGREY, GREY, LGREEN, LBLUE, LGREY };
#define NOFILL 0xFF
#define BACKDROP RGB15(31, 31, 30)   /* page colour behind everything      */
#define TEXTCOL  BLACK                /* body text                          */
#define BAR_FG   WHITE                /* title bar: white on accent red     */
#define BAR_BG   RED
#define FOOT_FG  DGREY                /* footer / status bar: dark on grey  */
#define FOOT_BG  LGREY

typedef struct {
	u8 ch[ROWS][COLS];
	u8 fg[ROWS][COLS];
	u8 bg[ROWS][COLS];
} Screen;

static Screen term, edit;
static u16 *map0, *map1;
static u16 *gfx;          /* 8 bit bitmap layer behind the text (256x256) */
static int gfxBg;

/* ------------------------------------------------------------------ */
/* screens                                                             */
/* ------------------------------------------------------------------ */

static void scr_clear(Screen *s, u8 fg) {
	memset(s->ch, ' ', sizeof s->ch);
	memset(s->fg, fg, sizeof s->fg);
	memset(s->bg, NOFILL, sizeof s->bg);
}

static void scr_put(Screen *s, int x, int y, char c, u8 fg, u8 bg) {
	if (x < 0 || x >= COLS || y < 0 || y >= ROWS) return;
	s->ch[y][x] = (u8)c;
	s->fg[y][x] = fg;
	s->bg[y][x] = bg;
}

static void scr_text(Screen *s, int x, int y, const char *t, u8 fg, u8 bg) {
	for (; *t && x < COLS; t++, x++) scr_put(s, x, y, *t, fg, bg);
}

static void scr_upload(const Screen *s, int curX, int curY) {
	for (int y = 0; y < ROWS; y++)
		for (int x = 0; x < COLS; x++) {
			u8 fg = s->fg[y][x], bg = s->bg[y][x];
			if (x == curX && y == curY) { bg = RED; fg = WHITE; }
			map0[y * 32 + x] = s->ch[y][x] | (fg << 12);
			map1[y * 32 + x] = bg == NOFILL ? 0 : (1 | (bg << 12));
		}
}

/* ---------------- output terminal ---------------- */

static int tx, ty;
static u8 tcol = TEXTCOL;
static bool outputSeen;

static void term_scroll(void) {
	memmove(term.ch[0], term.ch[1], (ROWS - 1) * COLS);
	memmove(term.fg[0], term.fg[1], (ROWS - 1) * COLS);
	memmove(term.bg[0], term.bg[1], (ROWS - 1) * COLS);
	memset(term.ch[ROWS - 1], ' ', COLS);
	memset(term.fg[ROWS - 1], tcol, COLS);
	memset(term.bg[ROWS - 1], NOFILL, COLS);
}

static void term_newline(void) {
	tx = 0;
	if (++ty >= ROWS) { term_scroll(); ty = ROWS - 1; }
}

static void term_putc(char c) {
	if (c == '\n') { term_newline(); return; }
	if (tx >= COLS) term_newline();
	scr_put(&term, tx++, ty, c, tcol, NOFILL);
}

static void term_puts(const char *s) { while (*s) term_putc(*s++); }

/* ------------------------------------------------------------------ */
/* interpreter I/O callbacks                                           */
/* ------------------------------------------------------------------ */

enum { VIEW_EDIT, VIEW_OUTPUT };
static int view = VIEW_EDIT;

static char keyq[16];
static int keyqHead, keyqTail;

static void io_putch(char c) { term_putc(c); outputSeen = true; view = VIEW_OUTPUT; }
static void io_cls(void) { scr_clear(&term, tcol); tx = ty = 0; view = VIEW_OUTPUT; }
static int io_inkey(void) {
	if (keyqHead == keyqTail) return 0;
	int c = (unsigned char)keyq[keyqTail];
	keyqTail = (keyqTail + 1) % sizeof keyq;
	return c;
}
static void io_locate(int c, int r) {
	tx = c < COLS ? c : COLS - 1;
	ty = r < ROWS ? r : ROWS - 1;
}
static void io_color(int c) { tcol = (u8)c; }

/* graphics: palette index for colour c is c*16+1, 0 = transparent.
 * VRAM needs 16 bit writes, so pixels are written in pairs. */
static void gfx_px(int x, int y, int c) {
	/* volatile: force real 16 bit accesses, VRAM ignores/duplicates byte stores */
	volatile u16 *p = &gfx[(y * 256 + x) >> 1];
	u16 v = c < 0 ? 0 : (u16)(c * 16 + 1);
	*p = (x & 1) ? ((*p & 0x00FF) | (v << 8)) : ((*p & 0xFF00) | v);
}
static void io_pset(int x, int y, int c) {
	gfx_px(x, y, c);
	view = VIEW_OUTPUT;
}
static int io_pget(int x, int y) {
	u16 w = gfx[(y * 256 + x) >> 1];
	int v = (x & 1) ? (w >> 8) : (w & 0xFF);
	return v ? (v - 1) / 16 : -1;
}
static void io_gcls(void) {
	u32 *p = (u32 *)gfx;
	for (int i = 0; i < 256 * 192 / 4; i++) p[i] = 0;
	view = VIEW_OUTPUT;
}

static void io_message(const char *m);
static void io_sound(int n) { if (n < 0) snd_stop(); else snd_play(basic_sound(n)); }
static int io_sound_busy(void) { return snd_busy(); }
static bool exitRequested;
static void io_exit(void) { exitRequested = true; }
/* JOY bits: 1 up, 2 down, 4 left, 8 right, 16 A, 32 B, 64 X, 128 Y, 256 L, 512 R */
static int io_joy(void) {
	u32 h = keysHeld();
	return ((h & KEY_UP) ? 1 : 0) | ((h & KEY_DOWN) ? 2 : 0) | ((h & KEY_LEFT) ? 4 : 0) |
	       ((h & KEY_RIGHT) ? 8 : 0) | ((h & KEY_A) ? 16 : 0) | ((h & KEY_B) ? 32 : 0) |
	       ((h & KEY_X) ? 64 : 0) | ((h & KEY_Y) ? 128 : 0) | ((h & KEY_L) ? 256 : 0) |
	       ((h & KEY_R) ? 512 : 0);
}
/* swap_screens(): program output (main engine) on the bottom LCD while the
 * program runs; during input() the keyboard comes back to the bottom */
static bool screensSwapped, touchDown;
static int  touchX, touchY;

static void apply_lcd(void) {
	if (screensSwapped && basic_state() == BASIC_RUNNING) lcdMainOnBottom();
	else lcdMainOnTop();
}

static int io_swap(int mode) {
	screensSwapped = mode < 0 ? !screensSwapped : mode != 0;
	apply_lcd();
	return screensSwapped;
}

static int io_touch(int *x, int *y) {
	if (!touchDown) return 0;
	*x = touchX;
	*y = touchY;
	return 1;
}

static const BasicIO bio = { io_putch, io_cls, io_inkey, io_locate, io_color, COLS, io_message,
                              io_pset, io_pget, io_gcls, 256, 192,
                              io_sound, io_sound_busy, io_exit, io_joy, io_swap, io_touch,
                              snd_tone, snd_tone_queued };

static void keyq_push(int c) {
	int next = (keyqHead + 1) % sizeof keyq;
	if (next == keyqTail) return;
	keyq[keyqHead] = (char)c;
	keyqHead = next;
}

/* ------------------------------------------------------------------ */
/* line input (shared by editor and INPUT statement)                   */
/* ------------------------------------------------------------------ */

#define INPUT_MAX 94
static char inbuf[INPUT_MAX + 1];
static int inlen, incur, inmax = INPUT_MAX;
static int inStartX, inStartY;   /* INPUT statement: start in terminal */

static void input_clear(void) { inlen = incur = 0; inbuf[0] = 0; }

static void input_set(const char *s) {
	strncpy(inbuf, s, inmax);
	inbuf[inmax] = 0;
	inlen = incur = strlen(inbuf);
}

/* returns true if RETURN was pressed */
static bool input_key(int k) {
	switch (k) {
	case KC_RET: return true;
	case KC_DEL:
		if (incur > 0) {
			memmove(inbuf + incur - 1, inbuf + incur, inlen - incur + 1);
			incur--; inlen--;
		}
		break;
	case KC_LEFT:  if (incur > 0) incur--; break;
	case KC_RIGHT: if (incur < inlen) incur++; break;
	case KC_CLR:   input_clear(); break;
	default:
		if (k >= 32 && k < 127 && inlen < inmax) {
			memmove(inbuf + incur + 1, inbuf + incur, inlen - incur + 1);
			inbuf[incur++] = (char)k;
			inlen++;
		}
	}
	return false;
}

/* draw the input buffer into a screen starting at (x,y); returns cursor pos */
static void input_draw(Screen *s, int x, int y, int clearLen, int *cx, int *cy) {
	int pos = y * COLS + x;
	for (int i = 0; i < clearLen; i++) {
		int p = pos + i;
		if (p >= ROWS * COLS) break;
		scr_put(s, p % COLS, p / COLS, i < inlen ? inbuf[i] : ' ', i < inlen ? tcol : TEXTCOL, NOFILL);
	}
	int p = pos + incur;
	*cx = p % COLS;
	*cy = p / COLS;
}

/* ------------------------------------------------------------------ */
/* status line and storage                                             */
/* ------------------------------------------------------------------ */

static char statusMsg[33] = "READY.";
static u8 statusCol = GREEN;

static void set_status(const char *msg, u8 colr) {
	strncpy(statusMsg, msg, 32);
	statusMsg[32] = 0;
	statusCol = colr;
}

/* code editor cursor: line index and column; wantCol = column inside a
 * screen row that up/down try to keep; topLine/topSub = first visible
 * screen row (line and wrapped row of that line) */
static int curLine, curCol, wantCol, topLine, topSub;

static void ed_reset(void) { curLine = curCol = wantCol = topLine = topSub = 0; }

/* every file of the program keeps its own cursor position */
typedef struct { int line, col, want, top, sub; } EdPos;
static EdPos edPos[BASIC_PROJECT_FILES];
static int   edFile, edVersion = -1;

/* follow file switches made by the interpreter (EDIT, LOAD, RMFILE ...) */
static void sync_file(void) {
	int f = basic_current_file();
	if (basic_files_version() != edVersion) {
		edVersion = basic_files_version();
		memset(edPos, 0, sizeof edPos);
		ed_reset();
		edFile = f;
		return;
	}
	if (f == edFile) return;
	edPos[edFile] = (EdPos){ curLine, curCol, wantCol, topLine, topSub };
	curLine = edPos[f].line; curCol = edPos[f].col; wantCol = edPos[f].want;
	topLine = edPos[f].top; topSub = edPos[f].sub;
	edFile = f;
}

static void select_file(int f) {
	basic_select_file(f);
	sync_file();
}

/* SAVE / LOAD / DELETE report: status line + terminal, no view switch */
static bool messageShown;

static void io_message(const char *m) {
	set_status(m, GREEN);
	messageShown = true;
	if (tx) term_newline();
	term_puts(m);
	term_newline();
	if (!strncmp(m, "LOADED", 6)) ed_reset();
}

/* mount SD card / flashcart (DLDI) and use <device>:/nds-basic for files */
static void storage_init(void) {
	static char dir[64];
	if (!fatInitDefault()) {
		set_status(L("KEINE SD-KARTE - KEIN SPEICHERN", "NO SD CARD - SAVE DISABLED"), RED);
		return;
	}
	char cwd[64] = "";
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
	char *colon = strchr(cwd, ':');
	if (colon) { colon[1] = 0; snprintf(dir, sizeof dir, "%s/nds-basic", cwd); }
	else strcpy(dir, "/nds-basic");
	mkdir(dir, 0777);
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		set_status(L("SD-FEHLER - KEIN SPEICHERN", "SD ERROR - SAVE DISABLED"), RED);
		return;
	}
	basic_set_storage(dir);
	char msg[40];
	snprintf(msg, sizeof msg, L("READY. DATEIEN: %s", "READY. FILES: %s"), dir);
	set_status(msg, GREEN);
}

/* ------------------------------------------------------------------ */
/* code editor (top screen): line numbers in the gutter, soft wrapped   */
/* lines, cursor moved with the d-pad or the arrow keys                */
/* ------------------------------------------------------------------ */

#define CODE_TOP   1
#define CODE_ROWS  22
#define STATUS_ROW 23

static int gutter_w(void) {
	int n = basic_line_count();
	return (n >= 1000 ? 4 : n >= 100 ? 3 : 2) + 1;
}
static int text_w(void) { return COLS - gutter_w(); }
static int line_len(int i) { return i < basic_line_count() ? (int)strlen(basic_line_text(i)) : 0; }
static int line_rows(int i) { return line_len(i) / text_w() + 1; }

static void ed_clamp(void) {
	int n = basic_line_count();
	if (curLine >= n) curLine = n ? n - 1 : 0;
	if (curLine < 0) curLine = 0;
	int l = line_len(curLine);
	if (curCol > l) curCol = l;
	if (curCol < 0) curCol = 0;
	if (topLine >= n) { topLine = n ? n - 1 : 0; topSub = 0; }
	if (topSub >= line_rows(topLine)) topSub = 0;
}

/* scroll so that the cursor is visible */
static void ed_scroll(void) {
	ed_clamp();
	int tw = text_w(), csub = curCol / tw;
	if (curLine < topLine || (curLine == topLine && csub < topSub)) { topLine = curLine; topSub = csub; return; }
	for (;;) {
		int r = -topSub;
		for (int i = topLine; i < curLine; i++) r += line_rows(i);
		r += csub + 1;
		if (r <= CODE_ROWS) break;
		if (++topSub >= line_rows(topLine)) { topLine++; topSub = 0; }
	}
}

static void ed_goto_line(int line) {
	curLine = line;
	curCol = (int)strspn(line < basic_line_count() ? basic_line_text(line) : "", " ");
	wantCol = curCol % text_w();
	ed_scroll();
}

static void ed_set_want(void) { wantCol = curCol % text_w(); }

static void ed_left(void) {
	if (curCol > 0) curCol--;
	else if (curLine > 0) { curLine--; curCol = line_len(curLine); }
	ed_set_want();
}

static void ed_right(void) {
	if (curCol < line_len(curLine)) curCol++;
	else if (curLine < basic_line_count() - 1) { curLine++; curCol = 0; }
	ed_set_want();
}

static void ed_up(void) {
	int tw = text_w();
	if (curCol >= tw) curCol = (curCol / tw - 1) * tw + wantCol;
	else if (curLine > 0) {
		int l = line_len(--curLine);
		curCol = (l / tw) * tw + wantCol;
		if (curCol > l) curCol = l;
	}
}

static void ed_down(void) {
	int tw = text_w(), l = line_len(curLine);
	if (curCol / tw < l / tw) {
		curCol = (curCol / tw + 1) * tw + wantCol;
		if (curCol > l) curCol = l;
	} else if (curLine < basic_line_count() - 1) {
		l = line_len(++curLine);
		curCol = wantCol < l ? wantCol : l;
	}
}

/* home: first non-blank character, pressed again: column 0 */
static void ed_home(void) {
	int ind = (int)strspn(curLine < basic_line_count() ? basic_line_text(curLine) : "", " ");
	curCol = curCol == ind ? 0 : ind;
	ed_set_want();
}
static void ed_end(void) { curCol = line_len(curLine); ed_set_want(); }
static void ed_page(int dir) { for (int i = 0; i < CODE_ROWS - 1; i++) { if (dir < 0) ed_up(); else ed_down(); } }

static void ed_full(void) {
	set_status(L("ZEILE ZU LANG/PROGRAMM VOLL", "LINE TOO LONG / PROGRAM FULL"), RED);
}

static bool ed_ensure_line(void) {
	if (basic_line_count() == 0 && !basic_insert_line(0, "")) { ed_full(); return false; }
	return true;
}

static void ed_insert(char c) {
	char buf[BASIC_LINE_MAX + 2];
	if (!ed_ensure_line()) return;
	const char *t = basic_line_text(curLine);
	int l = (int)strlen(t);
	if (c == '}' && curCol == l && l >= 2 && (int)strspn(t, " ") == l) {
		/* closing brace on an empty, indented line: one level back */
		memset(buf, ' ', l - 2);
		buf[l - 2] = '}';
		buf[l - 1] = 0;
		curCol = l - 1;
	} else {
		if (l >= BASIC_LINE_MAX) { ed_full(); return; }
		memcpy(buf, t, curCol);
		buf[curCol] = c;
		memcpy(buf + curCol + 1, t + curCol, l - curCol + 1);
		curCol++;
	}
	basic_set_line(curLine, buf);
	ed_set_want();
}

/* RET: split the line, the new line keeps the indentation (+2 after '{') */
static void ed_newline(void) {
	char left[BASIC_LINE_MAX + 1], right[BASIC_LINE_MAX + 1];
	if (!ed_ensure_line()) return;
	if (basic_line_count() >= BASIC_MAX_LINES) { ed_full(); return; }
	const char *t = basic_line_text(curLine);
	int l = (int)strlen(t), ind = (int)strspn(t, " ");
	if (ind > curCol) ind = curCol;
	memcpy(left, t, curCol);
	left[curCol] = 0;
	int e = curCol;
	while (e > 0 && left[e - 1] == ' ') e--;
	int ni = ind + ((e > 0 && left[e - 1] == '{') ? 2 : 0);
	const char *rp = t + curCol;
	while (*rp == ' ') rp++;
	int rest = (int)strlen(rp);
	if (ni + rest > BASIC_LINE_MAX) ni = 0;
	if (rest > BASIC_LINE_MAX) rest = BASIC_LINE_MAX;
	memset(right, ' ', ni);
	memcpy(right + ni, rp, rest);
	right[ni + rest] = 0;
	(void)l;
	basic_set_line(curLine, left);
	basic_insert_line(curLine + 1, right);
	curLine++;
	curCol = ni;
	ed_set_want();
}

/* DEL: delete left of the cursor, at the line start join with the line above */
static void ed_backspace(void) {
	char buf[2 * BASIC_LINE_MAX + 2];
	if (basic_line_count() == 0) return;
	const char *t = basic_line_text(curLine);
	int l = (int)strlen(t);
	if (curCol > 0) {
		memcpy(buf, t, curCol - 1);
		memcpy(buf + curCol - 1, t + curCol, l - curCol + 1);
		basic_set_line(curLine, buf);
		curCol--;
	} else if (curLine > 0) {
		const char *p = basic_line_text(curLine - 1);
		int pl = (int)strlen(p);
		if (pl + l > BASIC_LINE_MAX) { ed_full(); return; }
		memcpy(buf, p, pl);
		memcpy(buf + pl, t, l + 1);
		basic_set_line(curLine - 1, buf);
		basic_delete_line(curLine);
		curLine--;
		curCol = pl;
	} else if (l == 0 && basic_line_count() == 1) basic_delete_line(0);
	ed_set_want();
}

/* CLR: delete the whole line */
static void ed_delete_line(void) {
	if (!basic_line_count()) return;
	char msg[40];
	snprintf(msg, sizeof msg, L("ZEILE %d GEL\x99SCHT", "LINE %d DELETED"), curLine + 1);
	basic_delete_line(curLine);
	curCol = 0;
	ed_clamp();
	ed_set_want();
	set_status(msg, GREEN);
}

static void ed_key(int k, bool shifted) {
	switch (k) {
	case KC_LEFT:  if (shifted) ed_home(); else ed_left(); break;
	case KC_RIGHT: if (shifted) ed_end(); else ed_right(); break;
	case KC_UP:    if (shifted) ed_page(-1); else ed_up(); break;
	case KC_DOWN:  if (shifted) ed_page(1); else ed_down(); break;
	case KC_RET:   ed_newline(); break;
	case KC_DEL:   ed_backspace(); break;
	case KC_CLR:   ed_delete_line(); break;
	default:
		if (k >= 32 && k < 127) ed_insert((char)k);
	}
	ed_scroll();
}

/* --- syntax colours --- */

static bool is_keyword(const char *w, int n) {
	static const char *const kw[] = {
		"echo", "print", "if", "else", "elseif", "while", "do", "for", "foreach", "as", "switch",
		"case", "default", "break", "continue", "function", "return", "global", "true", "false",
		"null", "and", "or", "xor", "isset", "unset", "empty", "exit", "die", "array",
		"require", "require_once", "include", "include_once", "class", "interface", "trait",
		"extends", "implements", "new", "clone", "instanceof", "public", "protected", "private",
		"static", "abstract", "final", "const", "use", "self", "parent", "readonly", NULL
	};
	for (int i = 0; kw[i]; i++)
		if ((int)strlen(kw[i]) == n && !strncasecmp(kw[i], w, n)) return true;
	return false;
}

static bool ident_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void highlight(const char *t, int l, u8 *col) {
	int i = 0;
	while (i < l) {
		char c = t[i];
		if (c == '#' || (c == '/' && t[i + 1] == '/')) { while (i < l) col[i++] = GREY; break; }
		if (c == '/' && t[i + 1] == '*') {
			int j = i + 2;
			while (j < l && !(t[j] == '*' && t[j + 1] == '/')) j++;
			j = j < l ? j + 2 : l;
			while (i < j) col[i++] = GREY;
			continue;
		}
		if (c == '"' || c == '\'') {
			col[i++] = GREEN;
			while (i < l && t[i] != c) {
				if (t[i] == '\\' && i + 1 < l) col[i++] = GREEN;
				col[i++] = GREEN;
			}
			if (i < l) col[i++] = GREEN;
			continue;
		}
		if (c == '$' && i + 1 < l && (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_')) {
			col[i++] = PURPLE;
			while (i < l && ident_ch(t[i])) col[i++] = PURPLE;
			continue;
		}
		if (isalpha((unsigned char)c) || c == '_') {
			int j = i;
			while (j < l && ident_ch(t[j])) j++;
			u8 k = is_keyword(t + i, j - i) ? RED : TEXTCOL;
			while (i < j) col[i++] = k;
			continue;
		}
		if (isdigit((unsigned char)c)) {
			while (i < l && (isalnum((unsigned char)t[i]) || t[i] == '.')) col[i++] = ORANGE;
			continue;
		}
		col[i++] = TEXTCOL;
	}
}

static void render_edit(int *cx, int *cy) {
	char buf[40];
	u8 colors[BASIC_LINE_MAX + 1];
	scr_clear(&edit, TEXTCOL);
	ed_clamp();

	/* title bar with cursor position */
	for (int x = 0; x < COLS; x++) scr_put(&edit, x, 0, ' ', BAR_FG, BAR_BG);
	scr_text(&edit, 1, 0, basic_file_name(basic_current_file()), BAR_FG, BAR_BG);
	int n = basic_line_count();
	snprintf(buf, sizeof buf, L("Z %d/%d S %d", "L %d/%d C %d"), curLine + 1, n ? n : 1, curCol + 1);
	scr_text(&edit, COLS - 1 - strlen(buf), 0, buf, BAR_FG, BAR_BG);

	int gw = gutter_w(), tw = COLS - gw;
	*cx = gw;
	*cy = CODE_TOP;
	if (n == 0 && basic_current_file() > 0) {
		scr_text(&edit, 0, CODE_TOP, " 1", RED, NOFILL);
		scr_text(&edit, gw, CODE_TOP + 2, L("Z.B. FUNKTIONEN, EINBINDEN", "E.G. FUNCTIONS; USE IT IN"), GREY, NOFILL);
		scr_text(&edit, gw, CODE_TOP + 3, L("IN MAIN.PHP MIT:", "MAIN.PHP WITH:"), GREY, NOFILL);
		char req[40];
		snprintf(req, sizeof req, "require \"%s\";", basic_file_name(basic_current_file()));
		scr_text(&edit, gw + 2, CODE_TOP + 5, req, TEXTCOL, NOFILL);
	} else if (n == 0) {
		scr_text(&edit, 0, CODE_TOP, " 1", RED, NOFILL);
		scr_text(&edit, gw, CODE_TOP + 2, L("EINFACH LOSTIPPEN, Z.B.", "JUST START TYPING, E.G."), GREY, NOFILL);
		scr_text(&edit, gw + 2, CODE_TOP + 4, "$n = \"DS\";", TEXTCOL, NOFILL);
		scr_text(&edit, gw + 2, CODE_TOP + 5, L("echo \"HALLO $n!\";", "echo \"HELLO $n!\";"), TEXTCOL, NOFILL);
		scr_text(&edit, gw, CODE_TOP + 7, L("START/RUN: AUSF\x9a" "HREN", "START/RUN: RUN IT"), GREY, NOFILL);
		scr_text(&edit, gw, CODE_TOP + 8, L("SCRN: REPL-KONSOLE", "SCRN: REPL CONSOLE"), GREY, NOFILL);
	}
	int row = CODE_TOP, line = topLine, sub = topSub;
	while (row < CODE_TOP + CODE_ROWS && line < n) {
		const char *t = basic_line_text(line);
		int l = (int)strlen(t), rows = l / tw + 1;
		highlight(t, l, colors);
		for (; sub < rows && row < CODE_TOP + CODE_ROWS; sub++, row++) {
			if (sub == 0) {
				snprintf(buf, sizeof buf, "%*d", gw - 1, line + 1);
				scr_text(&edit, 0, row, buf, line == curLine ? RED : GREY, NOFILL);
			}
			for (int c = 0; c < tw; c++) {
				int idx = sub * tw + c;
				if (idx >= l) break;
				scr_put(&edit, gw + c, row, t[idx], colors[idx], NOFILL);
			}
			if (line == curLine && curCol / tw == sub) { *cx = gw + curCol % tw; *cy = row; }
		}
		line++;
		sub = 0;
	}

	/* status bar */
	for (int x = 0; x < COLS; x++) scr_put(&edit, x, STATUS_ROW, ' ', FOOT_FG, FOOT_BG);
	scr_text(&edit, 0, STATUS_ROW, statusMsg, statusCol, FOOT_BG);
}

/* ------------------------------------------------------------------ */
/* REPL console: the output terminal with a "> " prompt                */
/* ------------------------------------------------------------------ */

static int  frameCount;
static bool replActive;               /* prompt shown, input goes there  */
#define HISTORY 8
static char history[HISTORY][INPUT_MAX + 1];
static int  histCount, histPos;

static void open_sprite_editor(int slot);
static void open_sound_editor(int slot);

/* place the line input at the end of the terminal output */
static void begin_line_input(const char *prompt) {
	if (tx) term_newline();
	u8 saved = tcol;
	tcol = RED;
	term_puts(prompt);
	tcol = saved;
	if (tx >= COLS) term_newline();
	while (ty > ROWS - 3) { term_scroll(); ty--; }
	inStartX = tx;
	inStartY = ty;
	inmax = ROWS * COLS - (ty * COLS + tx) - 1;
	if (inmax > INPUT_MAX) inmax = INPUT_MAX;
	input_clear();
}

/* write the typed line into the terminal and return it */
static void end_line_input(char *out) {
	int cx, cy;
	input_draw(&term, inStartX, inStartY, inlen, &cx, &cy);
	int end = inStartY * COLS + inStartX + inlen;
	tx = end % COLS;
	ty = end / COLS;
	if (ty >= ROWS) { ty = ROWS - 1; tx = COLS; }
	term_newline();
	strcpy(out, inbuf);
	inmax = INPUT_MAX;
	input_clear();
}

static void repl_prompt(void) {
	begin_line_input("> ");
	replActive = true;
	histPos = histCount;
}

static void exec_line(const char *cmd) {
	outputSeen = false;
	basic_seed(frameCount * 2654435761u + 1);
	basic_enter(cmd, NULL);
}

static void repl_submit(void) {
	char cmd[INPUT_MAX + 1];
	end_line_input(cmd);
	replActive = false;
	const char *p = cmd;
	while (*p == ' ') p++;
	if (!*p) return;
	if (histCount == 0 || strcmp(history[(histCount - 1) % HISTORY], p)) {
		strcpy(history[histCount % HISTORY], p);
		histCount++;
	}
	/* SPRED [n] / SNDED [n] open the editors */
	if (!strncasecmp(p, "SNDED", 5) && (p[5] == 0 || p[5] == ' ')) {
		int sl = 0;
		sscanf(p + 5, "%d", &sl);
		open_sound_editor(sl);
		return;
	}
	if (!strncasecmp(p, "SPRED", 5) && (p[5] == 0 || p[5] == ' ')) {
		int sl = 0;
		sscanf(p + 5, "%d", &sl);
		open_sprite_editor(sl);
		return;
	}
	exec_line(p);
}

static void repl_history(int dir) {
	int oldest = histCount > HISTORY ? histCount - HISTORY : 0;
	int np = histPos + dir;
	if (np < oldest || np > histCount) return;
	histPos = np;
	input_set(np == histCount ? "" : history[np % HISTORY]);
}

/* RUN from the editor: shows "> RUN" in the console and starts the program */
static void run_program(void) {
	view = VIEW_OUTPUT;
	if (!replActive) repl_prompt();
	input_set("RUN");
	repl_submit();
}

/* ------------------------------------------------------------------ */
/* file browser (tap on "<>"): programs on the card, preview on top    */
/* ------------------------------------------------------------------ */

static bool browserOpen, browserLoading, browserDeleting;
static char browserFile[16];
static Screen brScr;

static void open_browser(void) {
	browserOpen = true;
	browserLoading = browserDeleting = false;
	browser_open();
}

static void close_browser(void) {
	browserOpen = false;
	kbd_redraw();
}

static void browser_frame(u32 down, u32 rep, bool touching, int px, int py) {
	if (basic_state() != BASIC_IDLE) return;         /* LOAD / DELETE running */
	int r = browser_poll(touching, px, py, down, rep);
	char cmd[40];
	if (r == BR_EXIT) close_browser();
	else if (r == BR_LOAD) {
		snprintf(cmd, sizeof cmd, "LOAD \"%s\"", browser_target());
		strcpy(browserFile, browser_target_file());
		browserLoading = true;
		exec_line(cmd);
	} else if (r == BR_DELETE) {
		snprintf(cmd, sizeof cmd, "DELETE \"%s\"", browser_target());
		browserDeleting = true;
		exec_line(cmd);
	}
}

/* LOAD / DELETE started from the browser has finished */
static void browser_exec_done(void) {
	const char *e = basic_last_error();
	if (browserLoading) {
		browserLoading = false;
		if (*e) { browser_set_message(e); return; }
		int f = 0;
		for (int i = 0; i < basic_file_count(); i++)
			if (browserFile[0] && !strcasecmp(basic_file_name(i), browserFile)) f = i;
		close_browser();
		select_file(f);
		ed_reset();
		view = VIEW_EDIT;
	} else if (browserDeleting) {
		browserDeleting = false;
		browser_refresh();
		browser_set_message(*e ? e : statusMsg);
	}
}

static void render_browser(void) {
	char buf[40];
	u8 colors[BASIC_LINE_MAX + 1];
	scr_clear(&brScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) {
		scr_put(&brScr, x, 0, ' ', BAR_FG, BAR_BG);
		scr_put(&brScr, x, ROWS - 1, ' ', FOOT_FG, FOOT_BG);
	}
	snprintf(buf, sizeof buf, "%.30s", browser_preview_title());
	scr_text(&brScr, 1, 0, buf, BAR_FG, BAR_BG);
	int n = browser_preview_count();
	for (int i = 0; i < n && i < ROWS - 2; i++) {
		const char *t = browser_preview_line(i);
		int l = (int)strlen(t);
		highlight(t, l, colors);
		for (int c = 0; c < l && c < COLS; c++) scr_put(&brScr, c, 1 + i, t[c], colors[c], NOFILL);
	}
	scr_text(&brScr, 0, ROWS - 1, L("A:\x99" "FFNEN/LADEN  B:ZUR\x9a" "CK", "A:OPEN/LOAD  B:BACK"), FOOT_FG, FOOT_BG);
	scr_upload(&brScr, -1, -1);
	bgHide(gfxBg);
}

static void finish_exec(void) {
	const char *e = basic_last_error();
	/* the program is over: screens back to normal */
	screensSwapped = false;
	apply_lcd();
	if (browserOpen) {
		messageShown = false;
		browser_exec_done();
		return;
	}
	if (*e) {
		set_status(e, RED);
		int ln = basic_error_line();
		if (ln > 0) { select_file(basic_error_file()); ed_goto_line(ln - 1); }
	} else if (!messageShown) set_status("READY.", GREEN);
	sync_file();
	if (basic_edit_request() >= 0) {
		/* EDIT "name": show that file in the editor */
		char msg[40];
		snprintf(msg, sizeof msg, L("DATEI %s", "FILE %s"), basic_file_name(basic_current_file()));
		set_status(msg, GREEN);
		view = VIEW_EDIT;
	}
	messageShown = false;
	ed_clamp();
}

static void begin_input_statement(void) {
	begin_line_input("");
	view = VIEW_OUTPUT;
}

static void end_input_statement(void) {
	char answer[INPUT_MAX + 1];
	end_line_input(answer);
	basic_input(answer);
}

/* ------------------------------------------------------------------ */
/* help page (SELECT)                                                  */
/* ------------------------------------------------------------------ */

#define HELP_ROWS (ROWS - 2)
static Screen helpScr;
static bool helpOpen;
static int helpTop;

/* draw UTF-8 text, mapping German umlauts to the CP437 font */
static int help_text(int x, int y, const char *t, u8 fg, u8 bg) {
	while (*t && x < COLS) {
		unsigned char c = (unsigned char)*t++;
		if (c == 0xC3 && *t) {
			switch ((unsigned char)*t++) {
			case 0xA4: c = 0x84; break;  /* ä */
			case 0xB6: c = 0x94; break;  /* ö */
			case 0xBC: c = 0x81; break;  /* ü */
			case 0x84: c = 0x8E; break;  /* Ä */
			case 0x96: c = 0x99; break;  /* Ö */
			case 0x9C: c = 0x9A; break;  /* Ü */
			case 0x9F: c = 0xE1; break;  /* ß */
			default:   c = '?';
			}
		}
		scr_put(&helpScr, x++, y, (char)c, fg, bg);
	}
	return x;
}

static int help_max_top(void) {
	int m = helpLineCount - HELP_ROWS;
	return m > 0 ? m : 0;
}

static void help_scroll(int d) {
	helpTop += d;
	if (helpTop > help_max_top()) helpTop = help_max_top();
	if (helpTop < 0) helpTop = 0;
}

static void render_help(void) {
	char buf[40];
	scr_clear(&helpScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) {
		scr_put(&helpScr, x, 0, ' ', BAR_FG, BAR_BG);
		scr_put(&helpScr, x, ROWS - 1, ' ', FOOT_FG, FOOT_BG);
	}
	help_text(1, 0, L("HILFE - BEFEHLE", "HELP - COMMANDS"), BAR_FG, BAR_BG);
	int pages = (helpLineCount + HELP_ROWS - 1) / HELP_ROWS;
	int page = (helpTop + HELP_ROWS - 1) / HELP_ROWS + 1;
	if (helpTop >= help_max_top()) page = pages;
	sprintf(buf, "%d/%d", page, pages);
	help_text(COLS - 1 - strlen(buf), 0, buf, BAR_FG, BAR_BG);
	help_text(0, ROWS - 1, L("\x18\x19\x1b\x1a" "BL\xc3\xa4TTERN  SELECT/B:ZUR\xc3\x9c" "CK",
	                          "\x18\x19\x1b\x1a" "SCROLL  SELECT/B:BACK"), FOOT_FG, FOOT_BG);

	for (int r = 0; r < HELP_ROWS; r++) {
		int i = helpTop + r;
		if (i >= helpLineCount) break;
		const char *t = helpText[i];
		int y = 1 + r;
		if (t[0] == '#') {
			for (int x = 0; x < COLS; x++) scr_put(&helpScr, x, y, ' ', RED, LGREY);
			help_text(1, y, t + 1, RED, LGREY);
		} else if (!strncmp(t, "@COLORS", 7)) {
			int first = t[7] == '8' ? 8 : 0;
			for (int c = first; c < first + 8; c++) {
				int x = (c - first) * 4;
				scr_put(&helpScr, x, y, (char)0xDB, c, NOFILL);
				scr_put(&helpScr, x + 1, y, (char)0xDB, c, NOFILL);
				sprintf(buf, "%-2d", c);
				help_text(x + 2, y, buf, GREY, NOFILL);
			}
		} else {
			const char *bar = strchr(t, '`');
			if (!bar) { help_text(0, y, t, TEXTCOL, NOFILL); continue; }
			int len = bar - t;
			if (len > 31) len = 31;
			memcpy(buf, t, len);
			buf[len] = 0;
			int x = len ? help_text(0, y, buf, TEXTCOL, NOFILL) + 1 : 2;
			help_text(x, y, bar + 1, GREY, NOFILL);
		}
	}
}

/* ------------------------------------------------------------------ */
/* sprite editor (X / SPRED n): editor on the bottom screen,           */
/* overview of all 256 slots on the top screen's graphics layer        */
/* ------------------------------------------------------------------ */

static bool spredOpen;
static u32 gfxBackup[256 * 192 / 4];
static Screen sprScr;

static void ov_cell(int i) {
	int cx = (i % 16) * 16 + 3, cy = 8 + (i / 16) * 11 + 1;
	u8 frame = (i == spred_slot()) ? RED : LGREY;
	for (int k = 0; k < 10; k++) {
		gfx_px(cx + k, cy, frame);
		gfx_px(cx + k, cy + 9, frame);
		gfx_px(cx, cy + k, frame);
		gfx_px(cx + 9, cy + k, frame);
	}
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) gfx_px(cx + 1 + x, cy + 1 + y, basic_sprite_get(i, x, y));
}

static void open_sprite_editor(int slot) {
	u32 *g = (u32 *)gfx;
	for (int i = 0; i < 256 * 192 / 4; i++) { gfxBackup[i] = g[i]; g[i] = 0; }
	spredOpen = true;
	spred_open(slot);
	for (int i = 0; i < BASIC_SPRITES; i++) ov_cell(i);
}

static void close_sprite_editor(void) {
	u32 *g = (u32 *)gfx;
	for (int i = 0; i < 256 * 192 / 4; i++) g[i] = gfxBackup[i];
	spredOpen = false;
	kbd_redraw();
	set_status("READY.", GREEN);
}

static void sprite_editor_frame(u32 down, u32 rep, bool touching, int px, int py) {
	int old = spred_slot();
	int r = spred_poll(touching, px, py);
	if (rep & KEY_LEFT)  spred_set_slot(spred_slot() - 1);
	if (rep & KEY_RIGHT) spred_set_slot(spred_slot() + 1);
	if (rep & KEY_UP)    spred_set_slot(spred_slot() - 16);
	if (rep & KEY_DOWN)  spred_set_slot(spred_slot() + 16);
	if (down & (KEY_B | KEY_X | KEY_SELECT | KEY_START)) r = SPRED_EXIT;
	if (r == SPRED_CHANGED) ov_cell(spred_slot());
	if (spred_slot() != old) { ov_cell(old); ov_cell(spred_slot()); }
	if (r == SPRED_EXIT) { close_sprite_editor(); return; }

	char buf[40];
	scr_clear(&sprScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) {
		scr_put(&sprScr, x, 0, ' ', BAR_FG, BAR_BG);
		scr_put(&sprScr, x, ROWS - 1, ' ', FOOT_FG, FOOT_BG);
	}
	scr_text(&sprScr, 1, 0, "SPRITES", BAR_FG, BAR_BG);
	sprintf(buf, "SLOT %03d", spred_slot());
	scr_text(&sprScr, COLS - 1 - strlen(buf), 0, buf, BAR_FG, BAR_BG);
	scr_text(&sprScr, 0, ROWS - 1, L("\x18\x19\x1b\x1a:SLOT  B/X/OK:FERTIG", "\x18\x19\x1b\x1a:SLOT  B/X/OK:DONE"), FOOT_FG, FOOT_BG);
	scr_upload(&sprScr, -1, -1);
	hwspr_update(false);
	bgShow(gfxBg);
}

/* ------------------------------------------------------------------ */
/* sound editor (Y / SNDED n): piano on the bottom screen,             */
/* note list and slot overview on the top screen                       */
/* ------------------------------------------------------------------ */

static bool sndedOpen;
static Screen sndScr;

static void open_sound_editor(int slot) {
	sndedOpen = true;
	snded_open(slot);
}

static void close_sound_editor(void) {
	sndedOpen = false;
	snd_stop();
	kbd_redraw();
	set_status("READY.", GREEN);
}

static void render_sound_screen(void) {
	char buf[40];
	int slot = snded_slot();
	const BasicSound *sd = basic_sound(slot);
	int playing = snd_position();

	scr_clear(&sndScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) {
		scr_put(&sndScr, x, 0, ' ', BAR_FG, BAR_BG);
		scr_put(&sndScr, x, ROWS - 1, ' ', FOOT_FG, FOOT_BG);
	}
	sprintf(buf, "SOUNDS  SLOT %d", slot);
	scr_text(&sndScr, 1, 0, buf, BAR_FG, BAR_BG);
	sprintf(buf, "%s %d BPM", snd_inst_name(sd->inst), sd->tempo);
	scr_text(&sndScr, COLS - 1 - strlen(buf), 0, buf, BAR_FG, BAR_BG);

	/* notes: 4 columns x 16 rows, "C#4 4" = note + length (1/n) */
	for (int k = 0; k <= sd->count && k < BASIC_SOUND_LEN; k++) {
		int x = (k / 16) * 8, y = 1 + k % 16;
		if (k == sd->count) { scr_put(&sndScr, x, y, '_', RED, NOFILL); break; }
		int len = sd->len[k];
		if (16 % len == 0) sprintf(buf, "%s%3d", snd_note_name(sd->note[k]), 16 / len);
		else sprintf(buf, "%s %2d*", snd_note_name(sd->note[k]), len);
		u8 fill = (k == playing) ? LGREY : NOFILL;
		scr_text(&sndScr, x, y, buf, sd->note[k] ? TEXTCOL : GREY, fill);
	}
	if (!sd->count) scr_text(&sndScr, 2, 3, L("LEER - TASTEN UNTEN ANTIPPEN", "EMPTY - TAP THE KEYS BELOW"), GREY, NOFILL);

	/* slot overview */
	for (int i = 0; i < BASIC_SOUNDS; i++) {
		const BasicSound *o = basic_sound(i);
		sprintf(buf, "%d:%3d %-5s", i, o->count, o->count ? snd_inst_name(o->inst) : "");
		scr_text(&sndScr, (i / 5) * 16, 18 + i % 5, buf, i == slot ? RED : TEXTCOL, NOFILL);
	}
	scr_text(&sndScr, 0, ROWS - 1, L("\x1b\x1a:SLOT \x18\x19:OKTAVE B/Y:FERTIG", "\x1b\x1a:SLOT \x18\x19:OCTAVE B/Y:DONE"), FOOT_FG, FOOT_BG);
	scr_upload(&sndScr, -1, -1);
	bgHide(gfxBg);
}

static void sound_editor_frame(u32 down, u32 rep, bool touching, int px, int py) {
	int r = snded_poll(touching, px, py);
	if (rep & KEY_LEFT)  snded_set_slot(snded_slot() - 1);
	if (rep & KEY_RIGHT) snded_set_slot(snded_slot() + 1);
	if (rep & KEY_UP)    snded_change_octave(1);
	if (rep & KEY_DOWN)  snded_change_octave(-1);
	if (down & (KEY_B | KEY_Y | KEY_SELECT | KEY_START)) r = SNDED_EXIT;
	if (r == SNDED_EXIT) { close_sound_editor(); return; }
	render_sound_screen();
	hwspr_update(false);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void video_init(void) {
	videoSetMode(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	lcdMainOnTop();

	static PrintConsole con;
	consoleInit(&con, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
	int bg1 = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 30, 1);
	bgSetPriority(con.bgId, 0);
	bgSetPriority(bg1, 1);
	bgShow(con.bgId);
	bgShow(bg1);
	map0 = bgGetMapPtr(con.bgId);
	map1 = bgGetMapPtr(bg1);

	/* graphics layer: 8 bit bitmap at 64 KiB, behind text and fills */
	gfxBg = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
	bgSetPriority(gfxBg, 3);
	gfx = bgGetGfxPtr(gfxBg);
	for (int i = 0; i < 256 * 256 / 2; i++) gfx[i] = 0;
	bgHide(gfxBg);
	bgUpdate();

	/* hardware sprites for SPRITE objects (VRAM bank B) */
	hwspr_init();

	/* tile 0 transparent, tile 1 solid colour index 1 */
	u32 *tiles = (u32 *)bgGetGfxPtr(bg1);
	for (int i = 0; i < 8; i++) { tiles[i] = 0; tiles[8 + i] = 0x11111111; }

	for (int p = 0; p < 16; p++) {
		BG_PALETTE[p * 16 + 1] = basicPalette[p];
		BG_PALETTE[p * 16 + 15] = basicPalette[p];
	}
	BG_PALETTE[0] = BACKDROP;
}

static void update_status_panel(void) {
	BasicState st = basic_state();
	char l1[40];
	if (helpOpen) {
		kbd_set_status(L("HILFE", "HELP"), L("\x18\x19\x1b\x1a:BL\x84TTERN SEL:ZUR\x9a" "CK",
		                                    "\x18\x19\x1b\x1a:SCROLL SEL:BACK"));
	} else if (st == BASIC_RUNNING) {
		int ln = basic_current_line();
		if (ln >= 0) sprintf(l1, L("L\x84UFT... ZEILE %d", "RUNNING... LINE %d"), ln);
		else strcpy(l1, L("L\x84UFT...", "RUNNING..."));
		kbd_set_status(l1, L("STOP/START = ABBRUCH", "STOP/START = BREAK"));
	} else if (st == BASIC_INPUT) {
		kbd_set_status(L("EINGABE - ANTWORT + RET", "INPUT - TYPE ANSWER + RET"),
		               L("STOP/START = ABBRUCH", "STOP/START = BREAK"));
	} else if (view == VIEW_OUTPUT) {
		kbd_set_status(*basic_last_error() ? basic_last_error() : L("REPL: PHP ODER BEFEHL + RET", "REPL: PHP OR COMMAND + RET"),
		               L("SCRN:EDITOR \x18\x19:VERLAUF", "SCRN:EDITOR \x18\x19:HISTORY"));
	} else {
		kbd_set_status(statusMsg, L("START:RUN SCRN:REPL SEL:HILFE", "START:RUN SCRN:REPL SEL:HELP"));
	}
}

/* title screen with program list; afterwards set up the 2D screens again
 * and load the program that was tapped (if any) */
static void enter_title(void) {
	char file[16];
	screensSwapped = false;
	bool chosen = title_screen(file, basic_line_count() > 0);
	video_init();
	kbd_init();
	view = VIEW_EDIT;
	replActive = false;
	if (chosen) {
		char cmd[40];
		snprintf(cmd, sizeof cmd, "LOAD \"%s\"", file);
		exec_line(cmd);
	}
}

int main(int argc, char *argv[]) {
	(void)argc; (void)argv;
	lang_init();
	help_init(langDE);
	keysSetRepeat(20, 4);
	scr_clear(&term, TEXTCOL);
	term_puts("**** NDS PHP V1.0 ****");
	term_newline();
	basic_init(&bio);
	snd_init();
	storage_init();       /* before the title screen: it lists the programs */
	enter_title();

	while (pmMainLoop()) {
		/* --- execute the program, using the whole frame --- */
		if (basic_state() == BASIC_RUNNING) {
			while ((REG_DISPSTAT & DISP_IN_VBLANK) && basic_state() == BASIC_RUNNING)
				basic_run_slice(8);
			while (!(REG_DISPSTAT & DISP_IN_VBLANK) && basic_state() == BASIC_RUNNING)
				basic_run_slice(8);
			if (basic_state() != BASIC_RUNNING) swiWaitForVBlank();
		} else {
			swiWaitForVBlank();
		}
		frameCount++;
		basic_frame();
		snd_frame();

		/* program reached an input() call? */
		static bool inInput;
		if (basic_state() == BASIC_INPUT) {
			if (!inInput) { begin_input_statement(); inInput = true; }
		} else inInput = false;

		BasicState st = basic_state();
		kbd_set_running(st != BASIC_IDLE);
		sync_file();

		/* console idle: show the REPL prompt */
		if (st == BASIC_IDLE && view == VIEW_OUTPUT && !replActive && !helpOpen) repl_prompt();

		/* --- input --- */
		scanKeys();
		u32 down = keysDown(), rep = keysDownRepeat(), held = keysHeld();
		touchPosition tp = { 0 };
		bool touching = (held & KEY_TOUCH) && touchRead(&tp);
		touchDown = touching;
		touchX = tp.px;
		touchY = tp.py;
		apply_lcd();
		if (spredOpen) {
			sprite_editor_frame(down, rep, touching, tp.px, tp.py);
			continue;
		}
		if (sndedOpen) {
			sound_editor_frame(down, rep, touching, tp.px, tp.py);
			continue;
		}
		if (browserOpen) {
			browser_frame(down, rep, touching, tp.px, tp.py);
		} else {
			{
				const char *names[BASIC_PROJECT_FILES];
				int nf = basic_file_count();
				for (int i = 0; i < nf; i++) names[i] = basic_file_name(i);
				kbd_set_tabs(names, nf, basic_current_file());
			}
			bool shoulder = (held & (KEY_L | KEY_R)) != 0;
			/* swapped screens: the touch screen belongs to the program */
		bool kbdTouch = touching && !(screensSwapped && st == BASIC_RUNNING);
		int k = kbd_poll(kbdTouch, tp.px, tp.py, shoulder);

			if (down & KEY_START) k = KC_RUN;
			if (rep & KEY_LEFT) k = KC_LEFT;
			if (rep & KEY_RIGHT) k = KC_RIGHT;
			if (rep & KEY_UP) k = KC_UP;
			if (rep & KEY_DOWN) k = KC_DOWN;
			if (rep & KEY_B) k = KC_DEL;
			if (down & KEY_A) k = KC_RET;

			if (helpOpen) {
				if (k == KC_UP) help_scroll(-1);
				else if (k == KC_DOWN) help_scroll(1);
				else if (k == KC_LEFT) help_scroll(-HELP_ROWS);
				else if (k == KC_RIGHT) help_scroll(HELP_ROWS);
				else if (k || (down & (KEY_SELECT | KEY_X | KEY_Y))) {
					helpOpen = false;
					if (k == KC_RUN) run_program();
				}
			} else if ((down & KEY_SELECT) && st == BASIC_IDLE) {
				helpOpen = true;
			} else if (k >= KC_TAB && k < KC_TAB + BASIC_PROJECT_FILES) {
				/* file tab: open that file in the editor */
				if (st == BASIC_IDLE) { select_file(k - KC_TAB); view = VIEW_EDIT; }
			} else if (k == KC_BROWSER) {
			if (st == BASIC_IDLE) { helpOpen = false; open_browser(); }
		} else if (k == KC_TABNEW) {
				/* "+": ask for the name in the REPL */
				if (st == BASIC_IDLE) {
					view = VIEW_OUTPUT;
					if (!replActive) repl_prompt();
					input_set("EDIT \"");
				}
			} else if (st != BASIC_IDLE) {
				if (k == KC_RUN) {
					basic_break();
					snd_stop();
				} else if (st == BASIC_INPUT) {
					if (k && input_key(k)) { inInput = false; end_input_statement(); }
				} else if (k >= 32 && k < 127) {
					keyq_push(k);
				} else if (k == KC_RET) {
					keyq_push(13);
				}
			} else if (down & KEY_Y) {
				open_sound_editor(snded_slot());
			} else if (down & KEY_X) {
				open_sprite_editor(spred_slot());
			} else if (view == VIEW_OUTPUT) {
				/* REPL console */
				if (k == KC_RUN) run_program();
				else if (k == KC_SCRN) view = VIEW_EDIT;
				else if (k == KC_UP) repl_history(-1);
				else if (k == KC_DOWN) repl_history(1);
				else if (k && input_key(k)) repl_submit();
			} else {
				/* code editor */
				if (k == KC_RUN) run_program();
				else if (k == KC_SCRN) view = VIEW_OUTPUT;
				else if (k) ed_key(k, shoulder);
			}
		}
		/* program just finished? */
		static BasicState prevState;
		BasicState now = basic_state();
		if (now == BASIC_IDLE && prevState != BASIC_IDLE) finish_exec();
		prevState = now;

		/* EXIT: back to the title screen, program and data stay in memory */
		if (exitRequested) {
			exitRequested = false;
			snd_stop();
			set_status("READY.", GREEN);
			enter_title();
			continue;
		}

		/* --- render top screen --- */
		bool blink = (frameCount & 32) == 0;
		int cx = -1, cy = -1;
		if (browserOpen) {
			render_browser();
		} else if (helpOpen) {
			render_help();
			scr_upload(&helpScr, -1, -1);
			bgHide(gfxBg);
		} else if (view == VIEW_EDIT) {
			bgHide(gfxBg);
			render_edit(&cx, &cy);
			if (!blink) cx = -1;
			scr_upload(&edit, cx, cy);
		} else {
			bgShow(gfxBg);
			if (basic_state() == BASIC_INPUT || (basic_state() == BASIC_IDLE && replActive)) {
				input_draw(&term, inStartX, inStartY, inmax, &cx, &cy);
				if (!blink) cx = -1;
			}
			scr_upload(&term, cx, cy);
		}
		hwspr_update(!helpOpen && !browserOpen && view == VIEW_OUTPUT);
		if (!browserOpen) update_status_panel();
	}
	return 0;
}
