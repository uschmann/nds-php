/*
 * NDS PHP - main.c
 *
 * Top screen:    program listing with line numbers + input line (edit view)
 *                or the program output terminal (output view)
 * Bottom screen: retro on-screen keyboard (kbd.c)
 */
#include <nds.h>
#include <ctype.h>
#include <stdlib.h>
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
#include "dbg.h"
#include "complete.h"
#include "syntax.h"
#include "refactor.h"
#include "vers.h"

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
static int frameCount;
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
/* L held (or tapped): the code goes to the touch screen, where the stylus
 * selects words and lines; a toolbar below the code acts on them */
static bool edSwap;
static int  touchX, touchY;

static void apply_lcd(void) {
	if ((screensSwapped && basic_state() == BASIC_RUNNING) || edSwap) lcdMainOnBottom();
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
static bool inputCode;             /* REPL prompt: colour the input like code */

static void input_draw(Screen *s, int x, int y, int clearLen, int *cx, int *cy) {
	int pos = y * COLS + x;
	u8 colors[INPUT_MAX + 1];
	if (inputCode) {
		syntax_highlight(inbuf, inlen, colors);
		/* REPL commands (RUN, SAVE "X" ...) in the keyword colour */
		static const char *const cmds[] = { "RUN", "NEW", "CLR", "LIST", "SAVE", "LOAD", "DIR", "DELETE",
		                                    "EXIT", "EDIT", "FILES", "RMFILE", "RENAME", "SPRED", "SNDED",
		                                    "COMMIT", "LOG", "STATUS", "DIFF", "CHECKOUT", NULL };
		int w = 0;
		while (w < inlen && isalpha((unsigned char)inbuf[w])) w++;
		for (int k = 0; cmds[k] && w; k++)
			if ((int)strlen(cmds[k]) == w && !strncasecmp(inbuf, cmds[k], w) &&
			    (w == inlen || inbuf[w] == ' ' || inbuf[w] == '"' || inbuf[w] == '\''))
				for (int i = 0; i < w; i++) colors[i] = RED;
	}
	for (int i = 0; i < clearLen; i++) {
		int p = pos + i;
		if (p >= ROWS * COLS) break;
		u8 c = i >= inlen ? TEXTCOL : inputCode ? colors[i] : tcol;
		scr_put(s, p % COLS, p / COLS, i < inlen ? inbuf[i] : ' ', c, NOFILL);
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

enum { SEL_NONE, SEL_WORD, SEL_LINES, SEL_CHARS };
static int selKind, selLine, selA, selB;   /* word/chars: line, [A,B); lines: A..B */

static void ed_reset(void) { curLine = curCol = wantCol = topLine = topSub = 0; selKind = SEL_NONE; }

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
	selKind = SEL_NONE;
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

/* mount SD card / flashcart (DLDI) and use <device>:/nds-php for files */
static void storage_init(void) {
	static char dir[64];
	if (!fatInitDefault()) {
		set_status(L("KEINE SD-KARTE - KEIN SPEICHERN", "NO SD CARD - SAVE DISABLED"), RED);
		return;
	}
	char cwd[64] = "";
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
	char *colon = strchr(cwd, ':');
	if (colon) colon[1] = 0;
	else cwd[0] = 0;
	snprintf(dir, sizeof dir, "%s/nds-php", cwd);
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
#define TOOL_ROWS  3                 /* toolbar below the code while swapped */

static int code_rows(void) { return edSwap ? CODE_ROWS - TOOL_ROWS : CODE_ROWS; }
static int rowLine[ROWS], rowSub[ROWS];   /* what render_edit drew in each row */

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
		if (r <= code_rows()) break;
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
static void ed_page(int dir) { for (int i = 0; i < code_rows() - 1; i++) { if (dir < 0) ed_up(); else ed_down(); } }

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

/* Y: copy the line below itself, the cursor moves to the copy */
static void ed_duplicate_line(void) {
	char buf[BASIC_LINE_MAX + 1], msg[40];
	if (!basic_line_count()) return;
	if (basic_line_count() >= BASIC_MAX_LINES) { ed_full(); return; }
	snprintf(buf, sizeof buf, "%s", basic_line_text(curLine));
	if (!basic_insert_line(curLine + 1, buf)) return;
	curLine++;
	ed_clamp();
	snprintf(msg, sizeof msg, L("ZEILE %d DUPLIZIERT", "LINE %d DUPLICATED"), curLine);
	set_status(msg, GREEN);
}

static void undo_typing(void);
static void undo_typing_end(void);

static void ed_key(int k, bool shifted) {
	selKind = SEL_NONE;
	if (k == KC_RET || k == KC_DEL || k == KC_CLR || (k >= 32 && k < 127)) undo_typing();
	else undo_typing_end();
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

/* ------------------------------------------------------------------ */
/* refactoring: undo, clipboard, selection, toolbar, rename / usages    */
/* ------------------------------------------------------------------ */

/* --- undo: snapshots of whole files (small on the DS) --- */

#define UNDO_MAX 20
typedef struct { int nfiles; int file[BASIC_PROJECT_FILES]; char *text[BASIC_PROJECT_FILES]; int curFile, line, col; } UndoStep;
static UndoStep undoSteps[UNDO_MAX], redoSteps[UNDO_MAX];
static int nUndo, nRedo;
static int undoTag;                /* last action that pushed a step (to merge repeats) */

static char *file_text(int f) {
	int n = basic_file_line_count(f), len = 1;
	for (int i = 0; i < n; i++) len += (int)strlen(basic_file_line_text(f, i)) + 1;
	char *p = malloc(len), *o = p;
	if (!p) return NULL;
	for (int i = 0; i < n; i++) {
		const char *t = basic_file_line_text(f, i);
		int l = (int)strlen(t);
		memcpy(o, t, l);
		o += l;
		if (i < n - 1) *o++ = '\n';
	}
	*o = 0;
	if (!n) { free(p); p = NULL; }
	return p ? p : calloc(1, 2);
}

static void step_free(UndoStep *s) {
	for (int i = 0; i < s->nfiles; i++) free(s->text[i]);
	s->nfiles = 0;
}

/* snapshot of one file (f >= 0) or all files (f < 0) */
static void step_take(UndoStep *s, int f) {
	s->nfiles = 0;
	int nf = basic_file_count();
	for (int i = 0; i < nf; i++) {
		if (f >= 0 && i != f) continue;
		s->file[s->nfiles] = i;
		s->text[s->nfiles] = file_text(i);
		s->nfiles++;
	}
	s->curFile = basic_current_file();
	s->line = curLine;
	s->col = curCol;
}

static void stack_push(UndoStep *st, int *n, UndoStep *s) {
	if (*n == UNDO_MAX) { step_free(&st[0]); memmove(st, st + 1, (UNDO_MAX - 1) * sizeof *st); (*n)--; }
	st[(*n)++] = *s;
}

/* before a change: tag != 0 merges repeats of the same action */
static void undo_push(int f, int tag) {
	if (tag && tag == undoTag && nUndo) return;
	UndoStep s;
	step_take(&s, f);
	stack_push(undoSteps, &nUndo, &s);
	while (nRedo) step_free(&redoSteps[--nRedo]);
	undoTag = tag;
}

static void restore_step(UndoStep *s) {
	int cur = basic_current_file();
	for (int i = 0; i < s->nfiles; i++) {
		int f = s->file[i];
		if (f >= basic_file_count()) continue;
		basic_select_file(f);
		while (basic_line_count()) basic_delete_line(basic_line_count() - 1);
		const char *p = s->text[i];
		if (!*p) continue;
		for (;;) {
			const char *e = strchr(p, '\n');
			int l = e ? (int)(e - p) : (int)strlen(p);
			char buf[BASIC_LINE_MAX + 1];
			snprintf(buf, sizeof buf, "%.*s", l, p);
			basic_insert_line(basic_line_count(), buf);
			if (!e) break;
			p = e + 1;
		}
	}
	basic_select_file(s->curFile < basic_file_count() ? s->curFile : cur);
	sync_file();
	curLine = s->line;
	curCol = s->col;
	selKind = SEL_NONE;
	ed_clamp();
	ed_set_want();
	ed_scroll();
}

/* dir -1 undo, 1 redo */
static void undo_redo(int dir) {
	UndoStep *from = dir < 0 ? undoSteps : redoSteps, *to = dir < 0 ? redoSteps : undoSteps;
	int *nf = dir < 0 ? &nUndo : &nRedo, *nt = dir < 0 ? &nRedo : &nUndo;
	if (!*nf) { set_status(dir < 0 ? L("NICHTS ZUM R\x9a" "CKG\x84NGIGMACHEN", "NOTHING TO UNDO") : L("NICHTS ZUM WIEDERHOLEN", "NOTHING TO REDO"), RED); return; }
	UndoStep s = from[--*nf], back;
	step_take(&back, s.nfiles == 1 ? s.file[0] : -1);
	back.line = curLine; back.col = curCol;
	restore_step(&s);
	step_free(&s);
	stack_push(to, nt, &back);
	undoTag = 0;
	set_status(dir < 0 ? L("R\x9a" "CKG\x84NGIG GEMACHT", "UNDONE") : L("WIEDERHOLT", "REDONE"), GREEN);
}

/* typing: one snapshot per burst of typing in a line */
static bool typingBurst;
static int  burstLine;
static void undo_typing(void) {
	if (typingBurst && burstLine == curLine && undoTag == 1) return;
	undo_push(basic_current_file(), 0);
	undoTag = 1;
	typingBurst = true;
	burstLine = curLine;
}

static void undo_typing_end(void) { typingBurst = false; }

/* --- clipboard: whole lines or text inside a line --- */

static char *clip;
static bool clipLines;
static int  clipCount;

static void clip_set(const char *t, bool lines, int count) {
	free(clip);
	clip = strdup(t);
	clipLines = lines;
	clipCount = count;
}

static void sel_order(void) { if (selA > selB) { int t = selA; selA = selB; selB = t; } }

static void copy_selection(bool cut) {
	char msg[40];
	if (selKind == SEL_LINES) {
		sel_order();
		int len = 1;
		for (int i = selA; i <= selB; i++) len += (int)strlen(basic_line_text(i)) + 1;
		char *b = malloc(len), *o = b;
		if (!b) return;
		for (int i = selA; i <= selB; i++) {
			int l = (int)strlen(basic_line_text(i));
			memcpy(o, basic_line_text(i), l);
			o += l;
			if (i < selB) *o++ = '\n';
		}
		*o = 0;
		clip_set(b, true, selB - selA + 1);
		free(b);
		if (cut) {
			undo_push(basic_current_file(), 0);
			for (int i = selB; i >= selA; i--) basic_delete_line(i);
			curLine = selA;
			curCol = 0;
			selKind = SEL_NONE;
		}
		snprintf(msg, sizeof msg, cut ? L("%d ZEILEN AUSGESCHNITTEN", "%d LINES CUT") : L("%d ZEILEN KOPIERT", "%d LINES COPIED"), clipCount);
	} else if (selKind == SEL_WORD || selKind == SEL_CHARS) {
		const char *t = basic_line_text(selLine);
		char b[BASIC_LINE_MAX + 1];
		snprintf(b, sizeof b, "%.*s", selB - selA, t + selA);
		clip_set(b, false, 1);
		if (cut) {
			undo_push(basic_current_file(), 0);
			char nb[BASIC_LINE_MAX + 1];
			snprintf(nb, sizeof nb, "%.*s%s", selA, t, t + selB);
			basic_set_line(selLine, nb);
			curLine = selLine;
			curCol = selA;
			selKind = SEL_NONE;
		}
		snprintf(msg, sizeof msg, cut ? L("\"%.18s\" AUSGESCHNITTEN", "\"%.18s\" CUT") : L("\"%.18s\" KOPIERT", "\"%.18s\" COPIED"), clip);
	} else return;
	ed_clamp();
	ed_set_want();
	set_status(msg, GREEN);
}

static void paste_clip(void) {
	if (!clip) { set_status(L("ZWISCHENABLAGE LEER", "CLIPBOARD EMPTY"), RED); return; }
	undo_push(basic_current_file(), 0);
	if (clipLines) {
		int at = basic_line_count() ? curLine : 0, n = 0;
		const char *p = clip;
		for (;;) {
			const char *e = strchr(p, '\n');
			char buf[BASIC_LINE_MAX + 1];
			snprintf(buf, sizeof buf, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
			if (!basic_insert_line(at + n, buf)) { ed_full(); break; }
			n++;
			if (!e) break;
			p = e + 1;
		}
		selKind = SEL_LINES;
		selA = at;
		selB = at + n - 1;
		curLine = at;
		curCol = 0;
	} else {
		if (!ed_ensure_line()) return;
		const char *t = basic_line_text(curLine);
		int l = (int)strlen(t), cl = (int)strlen(clip);
		if (l + cl > BASIC_LINE_MAX) { ed_full(); return; }
		char buf[BASIC_LINE_MAX + 2];
		memcpy(buf, t, curCol);
		memcpy(buf + curCol, clip, cl);
		strcpy(buf + curCol + cl, t + curCol);
		basic_set_line(curLine, buf);
		curCol += cl;
		selKind = SEL_NONE;
	}
	ed_clamp();
	ed_set_want();
	ed_scroll();
	set_status(L("EINGEF\x9a" "GT", "PASTED"), GREEN);
}

/* --- line block actions --- */

static void lines_indent(int dir) {
	sel_order();
	undo_push(basic_current_file(), dir < 0 ? 2 : 3);
	for (int i = selA; i <= selB; i++) {
		const char *t = basic_line_text(i);
		char buf[BASIC_LINE_MAX + 1];
		if (dir > 0) {
			if ((int)strlen(t) + 2 > BASIC_LINE_MAX) continue;
			snprintf(buf, sizeof buf, "  %s", t);
		} else {
			int sp = (int)strspn(t, " ");
			snprintf(buf, sizeof buf, "%s", t + (sp > 2 ? 2 : sp));
		}
		basic_set_line(i, buf);
	}
}

static void lines_move(int dir) {
	sel_order();
	int n = basic_line_count();
	if ((dir < 0 && selA == 0) || (dir > 0 && selB >= n - 1)) return;
	undo_push(basic_current_file(), dir < 0 ? 4 : 5);
	/* the neighbouring line jumps to the other side of the block */
	int from = dir < 0 ? selA - 1 : selB + 1, to = dir < 0 ? selB : selA;
	char buf[BASIC_LINE_MAX + 1];
	snprintf(buf, sizeof buf, "%s", basic_line_text(from));
	basic_delete_line(from);
	basic_insert_line(to, buf);
	selA += dir;
	selB += dir;
	if (curLine >= selA - dir && curLine <= selB - dir) curLine += dir;
	else curLine = selA;
	ed_scroll();
}

static void lines_duplicate(void) {
	sel_order();
	int cnt = selB - selA + 1;
	if (basic_line_count() + cnt > BASIC_MAX_LINES) { ed_full(); return; }
	undo_push(basic_current_file(), 0);
	for (int i = 0; i < cnt; i++) {
		char buf[BASIC_LINE_MAX + 1];
		snprintf(buf, sizeof buf, "%s", basic_line_text(selA + i));
		basic_insert_line(selB + 1 + i, buf);
	}
	selA += cnt;
	selB += cnt;
	curLine = selA;
	curCol = 0;
	ed_scroll();
}

/* --- rename / usages: list of all places on the top screen --- */

enum { LIST_NONE, LIST_RENAME, LIST_USES };
static int listMode;
static RefTarget listTarget;
static RefOcc listOcc[REF_MAX];
static int listCount, listSel, listTop;
static char renBuf[32];
static int  renLen;
static char renError[33];
static Screen listScr;
#define LIST_FIRST 3                 /* first list row on the screen */

/* rows of the list: file headers (occ = -1 - file) and lines (first occurrence) */
static short listRow[REF_MAX + BASIC_PROJECT_FILES];
static int   nListRows;

static void list_build(void) {
	nListRows = 0;
	int lastFile = -1;
	for (int i = 0; i < listCount; i++) {
		RefOcc *o = &listOcc[i];
		if (o->file != lastFile) { listRow[nListRows++] = (short)(-1 - o->file); lastFile = o->file; }
		if (i && o->file == listOcc[i - 1].file && o->line == listOcc[i - 1].line) continue;
		listRow[nListRows++] = (short)i;
	}
}

static int list_files(void) {
	int n = 0;
	for (int i = 0; i < nListRows; i++) if (listRow[i] < 0) n++;
	return n;
}

static int list_code_count(void) {
	int n = 0;
	for (int i = 0; i < listCount; i++) n += listOcc[i].code;
	return n;
}

static const char *kind_label(int k) {
	return k == RK_VAR ? L("VARIABLE", "VARIABLE") : k == RK_MEMBER ? L("MEMBER", "MEMBER") :
	       k == RK_CLASS ? L("KLASSE", "CLASS") : L("FUNKTION/KONST.", "FUNCTION/CONST");
}

/* the word under the selection / cursor as a refactoring target */
static bool target_here(RefTarget *t) {
	int line = selKind == SEL_WORD ? selLine : curLine;
	int col = selKind == SEL_WORD ? selA : curCol;
	if (line >= basic_line_count()) return false;
	const char *s = basic_line_text(line);
	if (s[col] == '$') col++;
	if (ref_target(basic_current_file(), line, col, t) != RK_NONE) return true;
	set_status(L("KEIN NAME AUS DEM PROGRAMM", "NOT A NAME OF THE PROGRAM"), RED);
	return false;
}

static void open_list(int mode) {
	RefTarget t;
	if (!target_here(&t)) return;
	listTarget = t;
	listCount = ref_find(&t, listOcc, REF_MAX);
	list_build();
	listMode = mode;
	listSel = 0;
	listTop = 0;
	renError[0] = 0;
	snprintf(renBuf, sizeof renBuf, "%s", t.name);
	renLen = (int)strlen(renBuf);
	/* usages start at the place of the cursor */
	for (int i = 0; i < nListRows; i++) {
		if (listRow[i] < 0) continue;
		RefOcc *o = &listOcc[listRow[i]];
		if (o->file == basic_current_file() && o->line == (selKind == SEL_WORD ? selLine : curLine)) { listSel = i; break; }
	}
	if (listRow[listSel] < 0 && nListRows > 1) listSel = 1;
}

static void close_list(void) { listMode = LIST_NONE; }

static void select_word_at(int f, int line, int col) {
	if (f != basic_current_file()) select_file(f);
	curLine = line;
	curCol = col;
	int s, e;
	if (line < basic_line_count() && ref_word(basic_line_text(line), col, &s, &e)) {
		selKind = SEL_WORD; selLine = line; selA = s; selB = e;
	} else selKind = SEL_NONE;
	ed_clamp();
	ed_set_want();
	ed_scroll();
}

static void go_definition(void) {
	RefTarget t;
	if (!target_here(&t)) return;
	int f, l, c;
	if (!ref_definition(&t, &f, &l, &c)) { set_status(L("KEINE DEFINITION GEFUNDEN", "NO DEFINITION FOUND"), RED); return; }
	select_word_at(f, l, c);
	char msg[40];
	snprintf(msg, sizeof msg, "%s:%d", basic_file_name(f), l + 1);
	set_status(msg, GREEN);
}

static void list_jump(void) {
	if (listSel >= nListRows || listRow[listSel] < 0) return;
	RefOcc *o = &listOcc[listRow[listSel]];
	close_list();
	select_word_at(o->file, o->line, o->col);
}

static void apply_rename(void) {
	renBuf[renLen] = 0;
	if (!strcmp(renBuf, listTarget.name)) { close_list(); return; }
	char err[64];
	undo_push(-1, 0);
	int n = ref_rename(&listTarget, renBuf, err, sizeof err);
	if (n < 0) {
		step_free(&undoSteps[--nUndo]);
		snprintf(renError, sizeof renError, "%.32s", err);
		return;
	}
	char msg[40];
	snprintf(msg, sizeof msg, L("%d STELLEN GE\x84NDERT", "%d PLACES CHANGED"), n);
	set_status(msg, GREEN);
	close_list();
	selKind = SEL_NONE;
	ed_clamp();
}

static void list_move(int d) {
	int i = listSel;
	do { i += d; } while (i >= 0 && i < nListRows && listRow[i] < 0);
	if (i >= 0 && i < nListRows) listSel = i;
}

static void list_key(int k) {
	if (listMode == LIST_RENAME) {
		if (k == KC_RET) apply_rename();
		else if (k == KC_SCRN || k == KC_RUN) close_list();
		else if (k == KC_DEL) { if (renLen) renBuf[--renLen] = 0; renError[0] = 0; }
		else if (k == KC_UP) list_move(-1);
		else if (k == KC_DOWN) list_move(1);
		else if (k > 32 && k < 127 && (isalnum(k) || k == '_' || k >= 128) && renLen < 31) {
			if (renLen == 0 && isdigit(k)) return;
			renBuf[renLen++] = (char)k;
			renBuf[renLen] = 0;
			renError[0] = 0;
		}
	} else {
		if (k == KC_RET) list_jump();
		else if (k == KC_DEL || k == KC_SCRN || k == KC_RUN) close_list();
		else if (k == KC_UP || k == KC_LEFT) list_move(-1);
		else if (k == KC_DOWN || k == KC_RIGHT) list_move(1);
	}
}

static void render_list(int *cx, int *cy) {
	char buf[64];
	u8 colors[BASIC_LINE_MAX + 1];
	Screen *s = &listScr;
	scr_clear(s, TEXTCOL);
	for (int x = 0; x < COLS; x++) scr_put(s, x, 0, ' ', BAR_FG, BAR_BG);
	snprintf(buf, sizeof buf, " %s: %s", listMode == LIST_RENAME ? L("UMBENENNEN", "RENAME") : L("VERWENDUNG", "USAGES"), kind_label(listTarget.kind));
	scr_text(s, 0, 0, buf, BAR_FG, BAR_BG);
	bool dollar = listTarget.kind == RK_VAR;
	int nfiles = list_files(), ncode = list_code_count();
	*cx = *cy = -1;
	if (listMode == LIST_RENAME) {
		snprintf(buf, sizeof buf, "%s%.12s \x1a %s", dollar ? "$" : "", listTarget.name, dollar ? "$" : "");
		scr_text(s, 0, 1, buf, GREY, NOFILL);
		int x = (int)strlen(buf);
		scr_text(s, x, 1, renBuf, BLUE, NOFILL);
		*cx = x + renLen;
		*cy = 1;
		if (renError[0]) scr_text(s, 0, 2, renError, RED, NOFILL);
		else {
			snprintf(buf, sizeof buf, L("%d STELLEN IN %d DATEI%s", "%d PLACES IN %d FILE%s"), ncode, nfiles,
			         nfiles == 1 ? "" : L("EN", "S"));
			scr_text(s, 0, 2, buf, GREEN, NOFILL);
		}
	} else {
		snprintf(buf, sizeof buf, "%s%.20s: %d", dollar ? "$" : "", listTarget.name, ncode);
		scr_text(s, 0, 1, buf, BLUE, NOFILL);
	}
	/* keep the selected row visible */
	int rows = STATUS_ROW - LIST_FIRST;
	if (listSel < listTop) listTop = listSel > 0 && listRow[listSel - 1] < 0 ? listSel - 1 : listSel;
	if (listSel >= listTop + rows) listTop = listSel - rows + 1;
	for (int r = 0; r < rows && listTop + r < nListRows; r++) {
		int y = LIST_FIRST + r, idx = listRow[listTop + r];
		if (idx < 0) { scr_text(s, 0, y, basic_file_name(-1 - idx), PURPLE, NOFILL); continue; }
		RefOcc *o = &listOcc[idx];
		const char *t = basic_file_line_text(o->file, o->line);
		/* the line as it would look after the rename */
		char line[BASIC_LINE_MAX * 2 + 1];
		unsigned char mark[BASIC_LINE_MAX * 2 + 1];
		int n = 0, p = 0;
		for (int k = idx; k < listCount && listOcc[k].file == o->file && listOcc[k].line == o->line; k++) {
			RefOcc *q = &listOcc[k];
			while (p < q->col && n < BASIC_LINE_MAX) { mark[n] = 0; line[n++] = t[p++]; }
			const char *w = listMode == LIST_RENAME && q->code ? renBuf : t + q->col;
			int wl = listMode == LIST_RENAME && q->code ? renLen : q->len;
			for (int j = 0; j < wl && n < BASIC_LINE_MAX * 2; j++) { mark[n] = q->code ? 1 : 2; line[n++] = w[j]; }
			p = q->col + q->len;
		}
		while (t[p] && n < BASIC_LINE_MAX * 2) { mark[n] = 0; line[n++] = t[p++]; }
		line[n] = 0;
		syntax_highlight(line, n, colors);
		int sel = listTop + r == listSel;
		snprintf(buf, sizeof buf, "%4d ", o->line + 1);
		scr_text(s, 0, y, buf, sel ? WHITE : GREY, sel ? BLUE : NOFILL);
		int ind = (int)strspn(line, " "), x = 5;
		for (int j = ind; j < n && x < COLS; j++, x++) {
			if (mark[j] == 1) scr_put(s, x, y, line[j], WHITE, GREEN);
			else if (mark[j] == 2) scr_put(s, x, y, line[j], WHITE, GREY);
			else scr_put(s, x, y, line[j], colors[j], NOFILL);
		}
	}
	for (int x = 0; x < COLS; x++) scr_put(s, x, STATUS_ROW, ' ', FOOT_FG, FOOT_BG);
	scr_text(s, 0, STATUS_ROW, listMode == LIST_RENAME ? L("RET:UMBENENNEN  SCRN:ABBRUCH", "RET:RENAME  SCRN:CANCEL")
	                                                   : L("A:SPRINGEN \x18\x19:WAHL B:ZUR\x9a" "CK", "A:JUMP \x18\x19:SELECT B:BACK"),
	         FOOT_FG, FOOT_BG);
}

/* a tap on the list (screens swapped): pick the line, usages jump there */
static void list_touch(int row) {
	int i = listTop + row - LIST_FIRST;
	if (row < LIST_FIRST || i < 0 || i >= nListRows || listRow[i] < 0) return;
	listSel = i;
	if (listMode == LIST_USES) list_jump();
}

/* --- toolbar below the code while the screens are swapped --- */

enum { TA_RENAME = 1, TA_DEF, TA_USES, TA_COPY, TA_CUT, TA_DUP, TA_OUTDENT, TA_INDENT, TA_UP, TA_DOWN,
       TA_PASTE, TA_UNDO, TA_REDO, TA_DONE };
typedef struct { const char *label; int action, x, w; } ToolBtn;
static ToolBtn tools[8];
static int ntools;
static bool swapStick;             /* L was tapped: stays swapped */
static bool swapBlock;             /* rename started: back until L is released */

static void tool(const char *label, int action) {
	if (ntools < 8) { tools[ntools].label = label; tools[ntools].action = action; ntools++; }
}

static void build_toolbar(void) {
	ntools = 0;
	if (selKind == SEL_WORD) {
		tool(L("Umben.", "Rename"), TA_RENAME);
		tool(L("Def.", "Def"), TA_DEF);
		tool(L("Verw.", "Uses"), TA_USES);
		tool(L("Kopie", "Copy"), TA_COPY);
		tool(L("Aus", "Cut"), TA_CUT);
	} else if (selKind == SEL_CHARS) {
		tool(L("Aus", "Cut"), TA_CUT);
		tool(L("Kopie", "Copy"), TA_COPY);
		tool(L("R\x81" "ckg.", "Undo"), TA_UNDO);
	} else if (selKind == SEL_LINES) {
		tool(L("Aus", "Cut"), TA_CUT);
		tool(L("Kopie", "Copy"), TA_COPY);
		tool(L("Dupl", "Dup"), TA_DUP);
		tool("\xae", TA_OUTDENT);
		tool("\xaf", TA_INDENT);
		tool("\x18", TA_UP);
		tool("\x19", TA_DOWN);
	} else {
		if (clip) tool(L("Einf.", "Paste"), TA_PASTE);
		tool(L("R\x81" "ckg.", "Undo"), TA_UNDO);
		tool(L("Wdh.", "Redo"), TA_REDO);
		if (swapStick) tool(L("Fertig", "Done"), TA_DONE);
	}
	/* widths: label + 2, with gaps when there is room */
	int total = 0, pad = 2;
	for (int i = 0; i < ntools; i++) total += (int)strlen(tools[i].label) + 2;
	if (total > COLS) pad = 1;                   /* tight: one column of padding */
	total = 0;
	for (int i = 0; i < ntools; i++) { tools[i].w = (int)strlen(tools[i].label) + pad; total += tools[i].w; }
	int extra = COLS - total, gap = ntools > 1 && extra >= ntools - 1 ? 1 : 0;
	extra -= gap * (ntools - 1);
	int x = 0;
	for (int i = 0; i < ntools; i++) {
		int add = ntools ? extra / ntools + (i < extra % ntools ? 1 : 0) : 0;
		if (add > 4) add = 4;
		tools[i].w += add;
		tools[i].x = x;
		x += tools[i].w + gap;
	}
}

static void render_toolbar(void) {
	build_toolbar();
	int top = CODE_TOP + code_rows();
	for (int y = top; y < top + TOOL_ROWS; y++)
		for (int x = 0; x < COLS; x++) scr_put(&edit, x, y, ' ', TEXTCOL, NOFILL);
	for (int i = 0; i < ntools; i++) {
		u8 bg = (i & 1) ? LBLUE : BLUE;
		for (int y = top; y < top + TOOL_ROWS; y++)
			for (int x = tools[i].x; x < tools[i].x + tools[i].w && x < COLS; x++) scr_put(&edit, x, y, ' ', WHITE, bg);
		int l = (int)strlen(tools[i].label);
		scr_text(&edit, tools[i].x + (tools[i].w - l) / 2, top + 1, tools[i].label, WHITE, bg);
	}
}

static void tool_action(int a) {
	switch (a) {
	case TA_RENAME: open_list(LIST_RENAME); swapStick = false; swapBlock = listMode != LIST_NONE; break;
	case TA_USES: open_list(LIST_USES); break;
	case TA_DEF: go_definition(); break;
	case TA_COPY: copy_selection(false); break;
	case TA_CUT: copy_selection(true); break;
	case TA_DUP: lines_duplicate(); break;
	case TA_OUTDENT: lines_indent(-1); break;
	case TA_INDENT: lines_indent(1); break;
	case TA_UP: lines_move(-1); break;
	case TA_DOWN: lines_move(1); break;
	case TA_PASTE: paste_clip(); break;
	case TA_UNDO: undo_redo(-1); break;
	case TA_REDO: undo_redo(1); break;
	case TA_DONE: swapStick = false; break;
	}
	if (a != TA_UNDO && a != TA_REDO && a != TA_OUTDENT && a != TA_INDENT && a != TA_UP && a != TA_DOWN) undoTag = 0;
}

/* --- stylus on the code --- */

/* screen cell -> line and column (rows behind the text: end of the last line) */
static bool cell_pos(int row, int col, int *line, int *idx) {
	int n = basic_line_count();
	if (!n) return false;
	int gw = gutter_w(), tw = COLS - gw;
	if (row < CODE_TOP) row = CODE_TOP;
	if (row >= CODE_TOP + code_rows()) row = CODE_TOP + code_rows() - 1;
	if (rowLine[row] < 0) { *line = n - 1; *idx = line_len(n - 1); return true; }
	*line = rowLine[row];
	int c = col - gw;
	if (c < 0) c = 0;
	*idx = rowSub[row] * tw + c;
	if (*idx > line_len(*line)) *idx = line_len(*line);
	return true;
}

static void scroll_rows(int d) {
	for (; d > 0; d--) {                         /* down */
		if (topSub + 1 < line_rows(topLine)) topSub++;
		else if (topLine < basic_line_count() - 1) { topLine++; topSub = 0; }
	}
	for (; d < 0; d++) {
		if (topSub > 0) topSub--;
		else if (topLine > 0) { topLine--; topSub = line_rows(topLine) - 1; }
	}
}

static int  penState;              /* 0 up, 1 on code, 2 scrolling the gutter, 3 toolbar / list */
static int  penX, penY, penLine, penIdx, penRow, penMoved;

static void edit_touch(bool touching, int px, int py) {
	int row = py / 8, col = px / 8;
	int toolTop = CODE_TOP + code_rows();
	if (touching && !penState) {
		penX = px; penY = py; penRow = row; penMoved = 0;
		if (listMode) { penState = 3; list_touch(row); return; }
		if (row >= toolTop && row < toolTop + TOOL_ROWS) {
			penState = 3;
			build_toolbar();
			for (int i = 0; i < ntools; i++)
				if (col >= tools[i].x && col < tools[i].x + tools[i].w) { tool_action(tools[i].action); break; }
			return;
		}
		if (row < CODE_TOP || row >= toolTop) { penState = 3; return; }
		if (col < gutter_w()) { penState = 2; return; }
		penState = cell_pos(row, col, &penLine, &penIdx) ? 1 : 3;
		return;
	}
	if (touching && penState == 2) {
		if (row != penRow) { scroll_rows(penRow - row); penRow = row; }
		return;
	}
	if (touching && penState == 1) {
		int line, idx;
		/* at the top / bottom edge: scroll while selecting */
		if ((row <= CODE_TOP || row >= toolTop - 1) && (frameCount & 3) == 0) scroll_rows(row <= CODE_TOP ? -1 : 1);
		if (!cell_pos(row, col, &line, &idx)) return;
		if (line != penLine || (penMoved == 2 && abs(py - penY) > 4)) {
			penMoved = 2;
			selKind = SEL_LINES;
			selA = penLine;
			selB = line;
			curLine = line;
			curCol = 0;
		} else if (penMoved != 2 && (abs(px - penX) >= 6 || penMoved == 1)) {
			penMoved = 1;
			selKind = idx == penIdx ? SEL_NONE : SEL_CHARS;
			selLine = line;
			selA = penIdx < idx ? penIdx : idx;
			selB = penIdx < idx ? idx : penIdx;
			curLine = line;
			curCol = idx;
		}
		return;
	}
	if (!touching && penState) {
		if (penState == 1 && !penMoved) {          /* tap: word or cursor */
			curLine = penLine;
			curCol = penIdx;
			int s, e;
			const char *t = basic_line_text(penLine);
			if (ref_word(t, penIdx, &s, &e) && penIdx >= s && penIdx < e) { selKind = SEL_WORD; selLine = penLine; selA = s; selB = e; }
			else selKind = SEL_NONE;
		}
		if (selKind == SEL_LINES) sel_order();
		ed_clamp();
		ed_set_want();
		penState = 0;
	}
}

/* status line while code and selection are on the touch screen */
static void selection_status(char *l1, int size) {
	if (selKind == SEL_LINES) snprintf(l1, size, L("%d ZEILEN MARKIERT", "%d LINES SELECTED"), (selA > selB ? selA - selB : selB - selA) + 1);
	else if (selKind == SEL_WORD || selKind == SEL_CHARS) snprintf(l1, size, "\"%.*s\"", selB - selA > 24 ? 24 : selB - selA, basic_line_text(selLine) + selA);
	else snprintf(l1, size, "%s", L("TIPPEN:WORT  ZIEHEN:ZEILEN", "TAP:WORD  DRAG:LINES"));
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
	int row = CODE_TOP, line = topLine, sub = topSub, end = CODE_TOP + code_rows();
	for (int r = 0; r < ROWS; r++) { rowLine[r] = -1; rowSub[r] = 0; }
	while (row < end && line < n) {
		const char *t = basic_line_text(line);
		int l = (int)strlen(t), rows = l / tw + 1;
		bool inLines = selKind == SEL_LINES && line >= selA && line <= selB;
		syntax_highlight(t, l, colors);
		for (; sub < rows && row < end; sub++, row++) {
			rowLine[row] = line;
			rowSub[row] = sub;
			if (inLines) for (int c = gw; c < COLS; c++) scr_put(&edit, c, row, ' ', TEXTCOL, LGREY);
			if (sub == 0) {
				snprintf(buf, sizeof buf, "%*d", gw - 1, line + 1);
				if (inLines) scr_text(&edit, 0, row, buf, WHITE, BLUE);
				else if (basic_breakpoint(basic_current_file(), line)) scr_text(&edit, 0, row, buf, WHITE, RED);
				else scr_text(&edit, 0, row, buf, line == curLine ? RED : GREY, NOFILL);
			}
			for (int c = 0; c < tw; c++) {
				int idx = sub * tw + c;
				if (idx >= l) break;
				if ((selKind == SEL_WORD || selKind == SEL_CHARS) && line == selLine && idx >= selA && idx < selB)
					scr_put(&edit, gw + c, row, t[idx], WHITE, BLUE);
				else scr_put(&edit, gw + c, row, t[idx], colors[idx], inLines ? LGREY : NOFILL);
			}
			if (line == curLine && curCol / tw == sub) { *cx = gw + curCol % tw; *cy = row; }
		}
		line++;
		sub = 0;
	}

	/* status bar */
	for (int x = 0; x < COLS; x++) scr_put(&edit, x, STATUS_ROW, ' ', FOOT_FG, FOOT_BG);
	scr_text(&edit, 0, STATUS_ROW, statusMsg, statusCol, FOOT_BG);
	if (edSwap) render_toolbar();
}

/* ------------------------------------------------------------------ */
/* REPL console: the output terminal with a "> " prompt                */
/* ------------------------------------------------------------------ */

static bool replActive;               /* prompt shown, input goes there  */
#define HISTORY 8
static char history[HISTORY][INPUT_MAX + 1];
static int  histCount, histPos;

static void open_sprite_editor(int slot);
static void open_sound_editor(int slot);

/* place the line input at the end of the terminal output */
static void begin_line_input(const char *prompt) {
	inputCode = prompt[0] == '>';          /* REPL: code; input(): plain text */
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
static char browserFile[BASIC_NAME_BUF];
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
		syntax_highlight(t, l, colors);
		for (int c = 0; c < l && c < COLS; c++) scr_put(&brScr, c, 1 + i, t[c], colors[c], NOFILL);
	}
	scr_text(&brScr, 0, ROWS - 1, L("A:\x99" "FFNEN/LADEN  B:ZUR\x9a" "CK", "A:OPEN/LOAD  B:BACK"), FOOT_FG, FOOT_BG);
	scr_upload(&brScr, -1, -1);
	bgHide(gfxBg);
}

/* ------------------------------------------------------------------ */
/* versions (menu): COMMIT / DIFF / CHECKOUT, diff preview on top       */
/* ------------------------------------------------------------------ */

static bool versOpen, versChecking;
static Screen vrScr;

static void open_versions(void) {
	versOpen = true;
	versChecking = false;
	vers_open();
}

static void close_versions(void) {
	versOpen = false;
	kbd_redraw();
}

static void versions_frame(u32 down, u32 rep, bool touching, int px, int py) {
	if (basic_state() != BASIC_IDLE) return;         /* CHECKOUT running */
	int r = vers_poll(touching, px, py, down, rep);
	char cmd[40];
	if (r == VR_EXIT) close_versions();
	else if (r == VR_DIFF) {
		/* the whole diff in the output view */
		close_versions();
		view = VIEW_OUTPUT;
		snprintf(cmd, sizeof cmd, "DIFF %d", vers_target());
		exec_line(cmd);
	} else if (r == VR_CHECKOUT) {
		snprintf(cmd, sizeof cmd, "CHECKOUT %d%s", vers_target(), vers_force() ? " FORCE" : "");
		versChecking = true;
		exec_line(cmd);
	} else if (r == VR_COMMIT) {
		/* ask for the message in the REPL */
		close_versions();
		view = VIEW_OUTPUT;
		if (!replActive) repl_prompt();
		input_set("COMMIT \"");
	}
}

/* CHECKOUT started from the version list has finished */
static void versions_exec_done(void) {
	const char *e = basic_last_error();
	versChecking = false;
	vers_refresh();                                 /* the status card drew over it */
	if (strstr(e, "Uncommitted")) { vers_ask_force(); return; }
	if (*e) { vers_set_message(e); return; }
	close_versions();
	select_file(0);
	ed_reset();
	view = VIEW_EDIT;
}

static void render_versions(void) {
	char buf[40];
	u8 colors[BASIC_LINE_MAX + 1];
	scr_clear(&vrScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) {
		scr_put(&vrScr, x, 0, ' ', BAR_FG, BAR_BG);
		scr_put(&vrScr, x, ROWS - 1, ' ', FOOT_FG, FOOT_BG);
	}
	snprintf(buf, sizeof buf, "%.30s", vers_preview_title());
	scr_text(&vrScr, 1, 0, buf, BAR_FG, BAR_BG);
	int n = vers_preview_count();
	for (int i = 0; i < n && i < ROWS - 2; i++) {
		const char *t = vers_preview_line(i);
		int l = (int)strlen(t), code = 0;
		u8 pc = GREY;
		/* "+12 code" green, "-12 code" red, "== file" cyan, the code highlighted */
		if ((t[0] == '+' || t[0] == '-') && isdigit((unsigned char)t[1])) {
			pc = t[0] == '+' ? GREEN : RED;
			while (code < l && t[code] != ' ') code++;
			if (code < l) code++;
		} else if (t[0] == '=' && t[1] == '=') { pc = CYAN; code = l; }
		else code = l;
		for (int c = 0; c < code && c < COLS; c++) scr_put(&vrScr, c, 1 + i, t[c], pc, NOFILL);
		syntax_highlight(t + code, l - code, colors);
		for (int c = code; c < l && c < COLS; c++) scr_put(&vrScr, c, 1 + i, t[c], colors[c - code], NOFILL);
	}
	scr_text(&vrScr, 0, ROWS - 1, L("A:DIFF  B:ZUR\x9a" "CK", "A:DIFF  B:BACK"), FOOT_FG, FOOT_BG);
	scr_upload(&vrScr, -1, -1);
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
	if (versOpen) {
		messageShown = false;
		versions_exec_done();
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

/* ------------------------------------------------------------------ */
/* debugger: panel on the bottom screen, variables on the top screen   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* autocompletion: suggestion chips above the keyboard                 */
/* ------------------------------------------------------------------ */

static Completion comp[COMPLETE_MAX];
static int  ncomp, compWord, compPage, compShown, compCtx = -1;
static char compBefore[BASIC_LINE_MAX + 1];

/* where typing goes: 1 editor, 2 REPL prompt, 0 nowhere */
static int typing_context(void) {
	if (basic_state() != BASIC_IDLE || helpOpen || browserOpen || versOpen || spredOpen || sndedOpen || listMode) return 0;
	if (view == VIEW_EDIT) return 1;
	if (view == VIEW_OUTPUT && replActive) return 2;
	return 0;
}

static void show_suggestions(void) {
	const char *labels[COMPLETE_MAX];
	for (int i = compPage; i < ncomp; i++) labels[i - compPage] = comp[i].label;
	compShown = kbd_set_suggestions(labels, ncomp - compPage, comp[compPage].hint, compPage > 0);
}

static void update_completion(bool otherScreen) {
	int ctx = otherScreen ? 0 : typing_context();
	char before[BASIC_LINE_MAX + 1] = "";
	if (ctx == 1 && curLine < basic_line_count()) {
		const char *t = basic_line_text(curLine);
		snprintf(before, sizeof before, "%.*s", curCol, t);
	} else if (ctx == 2) snprintf(before, sizeof before, "%.*s", incur, inbuf);
	if (ctx == compCtx && !strcmp(before, compBefore)) return;
	compCtx = ctx;
	strcpy(compBefore, before);
	ncomp = ctx ? complete_find(before, comp, COMPLETE_MAX, &compWord) : 0;
	compPage = 0;
	if (otherScreen) kbd_drop_suggestions();
	else if (ncomp) show_suggestions();
	else kbd_set_suggestions(NULL, 0, NULL, false);
}

static void type_into(int ctx, int k) {
	if (ctx == 1) ed_key(k, false);
	else input_key(k);
}

static void accept_completion(int i) {
	int ctx = typing_context();
	if (!ctx || i < 0 || i >= ncomp) return;
	Completion *c = &comp[i];
	for (int n = 0; n < compWord; n++) type_into(ctx, KC_DEL);
	for (const char *p = c->insert; *p; p++) type_into(ctx, (unsigned char)*p);
	for (int n = 0; n < c->back; n++) type_into(ctx, KC_LEFT);
	complete_used(c->label);
	compCtx = -1;                         /* look again */
}

static void completion_key(int k) {
	if (k == KC_SUGG_MORE) {
		compPage += compShown;
		if (compPage >= ncomp) compPage = 0;
		show_suggestions();
	} else accept_completion(compPage + k - KC_SUGG);
}

#define MAX_VARS 96
static bool dbgOpen, dbgReopen, varShowOutput;
static int  varTop;
static Screen varScr;
static BasicVar vars[MAX_VARS];

static void open_debugger(void) {
	dbgOpen = true;
	helpOpen = false;
	if (basic_state() == BASIC_IDLE) view = VIEW_EDIT;
	varTop = 0;
	varShowOutput = false;
	dbgp_open(basic_current_file());
}

static void close_debugger(void) {
	dbgOpen = false;
	kbd_redraw();
}

/* variables of the paused program: locals of the running function first */
static void render_vars(void) {
	char buf[64];
	scr_clear(&varScr, TEXTCOL);
	for (int x = 0; x < COLS; x++) scr_put(&varScr, x, 0, ' ', BAR_FG, BAR_BG);
	int f = 0, i = 0;
	basic_debug_location(&f, &i);
	snprintf(buf, sizeof buf, L(" PAUSE %s:%d", " PAUSED %s:%d"), basic_file_name(f), i + 1);
	scr_text(&varScr, 0, 0, buf, BAR_FG, BAR_BG);
	const char *fn = basic_debug_function();
	scr_text(&varScr, COLS - 1 - (int)strlen(fn), 0, fn, BAR_FG, BAR_BG);

	int n = basic_debug_vars(vars, MAX_VARS);
	/* lines: section titles and variables */
	int total = 0, row = 1 - varTop, prevGlobal = -1;
	for (int k = 0; k < n; k++) {
		if (vars[k].global != prevGlobal) {
			prevGlobal = vars[k].global;
			if (row >= 1 && row < ROWS - 1)
				scr_text(&varScr, 0, row, vars[k].global ? L("GLOBAL", "GLOBALS") : L("LOKAL", "LOCALS"), GREY, NOFILL);
			row++;
			total++;
		}
		if (row >= 1 && row < ROWS - 1) {
			snprintf(buf, sizeof buf, "$%.23s", vars[k].name);
			int nl = (int)strlen(buf);
			scr_text(&varScr, 1, row, buf, BLUE, NOFILL);
			scr_text(&varScr, 1 + nl, row, " =", GREY, NOFILL);
			char val[40];
			int room = COLS - 4 - nl;
			if (room < 4) room = 4;
			snprintf(val, sizeof val, "%.*s", room, vars[k].value);
			scr_text(&varScr, 4 + nl, row, val, TEXTCOL, NOFILL);
		}
		row++;
		total++;
	}
	if (n == 0) scr_text(&varScr, 1, 2, L("KEINE VARIABLEN", "NO VARIABLES"), GREY, NOFILL);
	if (varTop > 0 && varTop > total - (ROWS - 2)) varTop = total > ROWS - 2 ? total - (ROWS - 2) : 0;
	for (int x = 0; x < COLS; x++) scr_put(&varScr, x, STATUS_ROW, ' ', FOOT_FG, FOOT_BG);
	scr_text(&varScr, 0, STATUS_ROW, L("A:SCHRITT L/R:BL\x84TTERN SEL:AUSG", "A:STEP L/R:SCROLL SEL:OUTPUT"), FOOT_FG, FOOT_BG);
}

static void run_program(void);

static void debugger_frame(u32 down, u32 rep, bool touching, int px, int py) {
	BasicState st = basic_state();
	int act = dbgp_poll(touching, px, py, down, rep);
	switch (act) {
	case DBA_CLOSE: close_debugger(); return;
	case DBA_RUN:
		if (st == BASIC_PAUSED) { basic_debug_resume(BASIC_DBG_CONTINUE); varShowOutput = false; }
		else if (st == BASIC_IDLE) run_program();
		break;
	case DBA_STEP: case DBA_OVER: case DBA_OUT: {
		int mode = act == DBA_STEP ? BASIC_DBG_STEP : act == DBA_OVER ? BASIC_DBG_OVER : BASIC_DBG_OUT;
		if (st == BASIC_PAUSED) basic_debug_resume(mode);
		else if (st == BASIC_IDLE) { basic_debug_step_next(); run_program(); }
		else basic_debug_step_next();        /* running: pause at the next line */
		break;
	}
	case DBA_STOP:
		if (st != BASIC_IDLE) { basic_break(); snd_stop(); }
		break;
	}
	if (rep & KEY_L) varTop = varTop > 10 ? varTop - 10 : 0;
	if (rep & KEY_R) varTop += 10;
	if ((down & KEY_SELECT) && st == BASIC_PAUSED) varShowOutput = !varShowOutput;
	if (dbgOpen) dbgp_update();
}

/* entries of the "three dots" menu */
static void menu_action(int item) {
	BasicState st = basic_state();
	/* 0 open (file browser), 1 save, 2 versions, 3 debugger, 4 sprites, 5 sounds, 6 exit */
	if (item == 3) { open_debugger(); return; }
	if (st != BASIC_IDLE) { set_status(L("ERST DAS PROGRAMM BEENDEN", "STOP THE PROGRAM FIRST"), RED); return; }
	helpOpen = false;
	if (item == 0) open_browser();
	else if (item == 2) open_versions();
	else if (item == 4) open_sprite_editor(spred_slot());
	else if (item == 5) open_sound_editor(snded_slot());
	else if (item == 1) {
		/* save under the last name, or ask for one in the REPL */
		if (basic_program_name()[0]) exec_line("SAVE");
		else {
			view = VIEW_OUTPUT;
			if (!replActive) repl_prompt();
			input_set("SAVE \"");
		}
	} else if (item == 6) exec_line("EXIT");      /* back to the title screen, the program stays */
}

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
	} else if (listMode) {
		kbd_set_status(listMode == LIST_RENAME ? L("NEUER NAME + RET", "NEW NAME + RET") : L("VERWENDUNGEN", "USAGES"),
		               listMode == LIST_RENAME ? L("SCRN: ABBRUCH", "SCRN: CANCEL") : L("A:SPRINGEN B:ZUR\x9a" "CK", "A:JUMP B:BACK"));
	} else if (edSwap) {
		selection_status(l1, sizeof l1);
		kbd_set_status(l1, swapStick ? L("L: ZUR\x9a" "CK ZUR TASTATUR", "L: BACK TO THE KEYBOARD")
		                             : L("L LOSLASSEN: ZUR\x9a" "CK", "RELEASE L: BACK"));
	} else if (selKind != SEL_NONE) {
		selection_status(l1, sizeof l1);
		kbd_set_status(l1, L("L:AKTIONEN X:AUS Y:DUPL", "L:ACTIONS X:CUT Y:DUP"));
	} else {
		kbd_set_status(statusMsg, L("START:RUN SCRN:REPL SEL:HILFE", "START:RUN SCRN:REPL SEL:HELP"));
	}
}

/* title screen with program list; afterwards set up the 2D screens again
 * and load the program that was tapped (if any) */
static void enter_title(void) {
	char file[BASIC_NAME_BUF];
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
	complete_set_help(helpText, helpLineCount);
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
		/* debugger: opens by itself at a breakpoint; input() needs the keyboard */
		if (st == BASIC_PAUSED && !dbgOpen && !spredOpen && !sndedOpen && !browserOpen && !versOpen) open_debugger();
		if (st == BASIC_INPUT && dbgOpen) { close_debugger(); dbgReopen = true; }
		if (dbgReopen && (st == BASIC_RUNNING || st == BASIC_PAUSED || st == BASIC_IDLE)) {
			dbgReopen = false;
			if (st != BASIC_IDLE && !dbgOpen) open_debugger();
		}
		if (!dbgOpen) kbd_set_running(st != BASIC_IDLE);
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
		if (dbgOpen) {
			debugger_frame(down, rep, touching, tp.px, tp.py);
		} else if (browserOpen) {
			browser_frame(down, rep, touching, tp.px, tp.py);
		} else if (versOpen) {
			versions_frame(down, rep, touching, tp.px, tp.py);
		} else {
			{
				const char *names[BASIC_PROJECT_FILES];
				int nf = basic_file_count();
				for (int i = 0; i < nf; i++) names[i] = basic_file_name(i);
				kbd_set_tabs(names, nf, basic_current_file());
			}
			/* code editor: L held or tapped puts the code on the touch screen */
			bool edCtx = st == BASIC_IDLE && view == VIEW_EDIT && !helpOpen;
			static int lDownAt;
			static bool lActed;
			if (down & KEY_L) { lDownAt = frameCount; lActed = false; }
			if ((held & KEY_L) && (touching || (down & ~(u32)KEY_L))) lActed = true;
			if ((keysUp() & KEY_L) && edCtx && !lActed && !swapBlock && frameCount - lDownAt < 15) swapStick = !swapStick;
			if ((keysUp() & KEY_L) || (!listMode && !(held & KEY_L))) swapBlock = false;
			if (!edCtx) swapStick = false;
			bool wasSwap = edSwap;
			edSwap = edCtx && !swapBlock && ((held & KEY_L) || swapStick);
			/* a stylus that is down while the screens change belongs to neither */
			static bool penHold;
			if (edSwap != wasSwap) { penState = 0; apply_lcd(); if (touching) penHold = true; }
			if (!touching) penHold = false;
			bool shoulder = (held & (edCtx ? KEY_R : (KEY_L | KEY_R))) != 0;
			/* swapped screens: the touch screen belongs to the program / the code */
		bool kbdTouch = touching && !(screensSwapped && st == BASIC_RUNNING) && !edSwap && !penHold;
		int k = kbd_poll(kbdTouch, tp.px, tp.py, shoulder);
			if (edSwap) edit_touch(touching && !penHold, tp.px, tp.py);

			/* suggestion chips; R released on its own takes the first one */
			static bool rUsed;
			if (down & KEY_R) rUsed = false;
			if (k && (held & KEY_R)) rUsed = true;
			if (k >= KC_SUGG && k <= KC_SUGG_MORE) { completion_key(k); k = KC_NONE; }
			else if ((keysUp() & KEY_R) && !rUsed && ncomp && typing_context()) accept_completion(compPage);
			static bool selUsed;
			if (down & KEY_SELECT) selUsed = helpOpen;
			if (down & KEY_START) k = KC_RUN;
			if (rep & KEY_LEFT) k = KC_LEFT;
			if (rep & KEY_RIGHT) k = KC_RIGHT;
			if (rep & KEY_UP) k = KC_UP;
			if (rep & KEY_DOWN) k = KC_DOWN;
			if (rep & KEY_B) k = KC_DEL;
			if (down & KEY_A) k = KC_RET;
			if ((held & KEY_SELECT) && edCtx && (down & (KEY_A | KEY_X | KEY_Y))) k = KC_NONE;

			if (helpOpen) {
				if (k == KC_UP) help_scroll(-1);
				else if (k == KC_DOWN) help_scroll(1);
				else if (k == KC_LEFT) help_scroll(-HELP_ROWS);
				else if (k == KC_RIGHT) help_scroll(HELP_ROWS);
				else if (k || (down & (KEY_SELECT | KEY_X | KEY_Y))) {
					helpOpen = false;
					if (k == KC_RUN) run_program();
				}
			} else if ((held & KEY_SELECT) && edCtx && (down & (KEY_Y | KEY_A | KEY_X))) {
				/* SELECT+Y undo, SELECT+A redo, SELECT+X paste */
				selUsed = true;
				if (down & KEY_Y) undo_redo(-1);
				else if (down & KEY_A) undo_redo(1);
				else paste_clip();
			} else if (listMode && edCtx) {
				if (k) list_key(k);
			} else if ((keysUp() & KEY_SELECT) && !selUsed && st == BASIC_IDLE) {
				helpOpen = true;
			} else if (k >= KC_TAB && k < KC_TAB + BASIC_PROJECT_FILES) {
				/* file tab: open that file in the editor */
				if (st == BASIC_IDLE) { select_file(k - KC_TAB); view = VIEW_EDIT; }
			} else if (k == KC_MENU) {
				const char *items[7] = { L("\x99" "ffnen", "Open"), L("Speichern", "Save"), L("Versionen", "Versions"),
				                         "Debugger", L("Sprite-Editor", "Sprite editor"), L("Sound-Editor", "Sound editor"), "Exit" };
				static const unsigned char ic[7] = { KI_OPEN, KI_SAVE, KI_VERSIONS, KI_DEBUG, KI_SPRITE, KI_SOUND, KI_EXIT };
				kbd_open_menu(items, ic, 7, 6);
			} else if (k >= KC_MENU_ITEM && k < KC_MENU_ITEM + 7) {
				menu_action(k - KC_MENU_ITEM);
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
			} else if ((down & KEY_Y) && view != VIEW_EDIT) {
				open_sound_editor(snded_slot());
			} else if ((down & KEY_X) && view != VIEW_EDIT) {
				open_sprite_editor(spred_slot());
			} else if (view == VIEW_OUTPUT) {
				/* REPL console */
				if (k == KC_RUN) run_program();
				else if (k == KC_SCRN) view = VIEW_EDIT;
				else if (k == KC_UP) repl_history(-1);
				else if (k == KC_DOWN) repl_history(1);
				else if (k && input_key(k)) repl_submit();
			} else {
				/* code editor: Y duplicates the line, X deletes it */
				if (down & KEY_Y) {
					if (selKind == SEL_LINES) lines_duplicate();
					else { undo_push(basic_current_file(), 0); ed_duplicate_line(); }
					ed_scroll();
				} else if (down & KEY_X) {
					if (selKind != SEL_NONE) copy_selection(true);
					else if (basic_line_count()) {
						clip_set(basic_line_text(curLine), true, 1);
						undo_push(basic_current_file(), 0);
						ed_delete_line();
					}
					ed_scroll();
				}
				else if (k == KC_RUN) run_program();
				else if (k == KC_SCRN) view = VIEW_OUTPUT;
				else if (k) ed_key(k, shoulder);
			}
		}
		update_completion(dbgOpen || browserOpen || versOpen || spredOpen || sndedOpen || edSwap);
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
		bool varsShown = dbgOpen && basic_state() == BASIC_PAUSED && !varShowOutput;
		if (varsShown) {
			bgHide(gfxBg);
			render_vars();
			scr_upload(&varScr, -1, -1);
		} else if (browserOpen) {
			render_browser();
		} else if (versOpen) {
			render_versions();
		} else if (helpOpen) {
			render_help();
			scr_upload(&helpScr, -1, -1);
			bgHide(gfxBg);
		} else if (view == VIEW_EDIT && listMode) {
			bgHide(gfxBg);
			render_list(&cx, &cy);
			if (!blink) cx = -1;
			scr_upload(&listScr, cx, cy);
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
		hwspr_update(!helpOpen && !browserOpen && !varsShown && view == VIEW_OUTPUT);
		if (!browserOpen && !dbgOpen) update_status_panel();
	}
	return 0;
}
