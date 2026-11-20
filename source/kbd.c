/* kbd.c - retro on-screen keyboard, drawn into the sub engine bitmap */
#include <nds.h>
#include <string.h>
#include "kbd.h"
#include "draw.h"


typedef enum { KT_CHAR, KT_SHIFT, KT_SPECIAL } KeyType;
typedef enum { ST_NORMAL, ST_FUNC, ST_RED, ST_GREY } KeyStyle;

typedef struct {
	const char *label;   /* label for special keys          */
	char ch, sch;        /* normal / shifted char           */
	u8 type, style, units, code;
	/* computed */
	short x, y, w, h;
} Key;

#define K(c, s)            { NULL, c, s, KT_CHAR, ST_NORMAL, 1, 0, 0, 0, 0, 0 }
#define S(l, st, u, code)  { l, 0, 0, KT_SPECIAL, st, u, code, 0, 0, 0, 0 }

static Key keys[] = {
	/* row 0 */
	K('1','!'), K('2','"'), K('3','\\'), K('4','$'), K('5','%'), K('6','&'),
	K('7','\''), K('8','('), K('9',')'), K('0','|'), K('-','_'),
	S("DEL", ST_FUNC, 2, KC_DEL),
	/* row 1 */
	K('q','Q'), K('w','W'), K('e','E'), K('r','R'), K('t','T'), K('y','Y'),
	K('u','U'), K('i','I'), K('o','O'), K('p','P'), K('+','*'), K('=','{'),
	K('/','}'),
	/* row 2 */
	K('a','A'), K('s','S'), K('d','D'), K('f','F'), K('g','G'), K('h','H'),
	K('j','J'), K('k','K'), K('l','L'), K(';',':'), K(',','['),
	S("RET", ST_FUNC, 2, KC_RET),
	/* row 3 */
	{ "SHFT", 0, 0, KT_SHIFT, ST_FUNC, 2, 0, 0, 0, 0, 0 },
	K('z','Z'), K('x','X'), K('c','C'), K('v','V'), K('b','B'), K('n','N'),
	K('m','M'), K('<','>'), K('.',']'),
	S("\x1b", ST_GREY, 1, KC_LEFT), S("\x1a", ST_GREY, 1, KC_RIGHT),
	/* row 4 */
	S("RUN", ST_RED, 3, KC_RUN), S("SCRN", ST_GREY, 2, KC_SCRN),
	S("", ST_NORMAL, 4, ' '), S("CLR", ST_GREY, 2, KC_CLR),
	S("\x18", ST_GREY, 1, KC_UP), S("\x19", ST_GREY, 1, KC_DOWN),
};
#define NKEYS ((int)(sizeof(keys) / sizeof(keys[0])))
static const u8 rowLen[] = { 12, 13, 12, 12, 6 };

#define KEY_PITCH 19
#define ROW_PITCH 26
#define KEY_H     24
#define KB_X      4
#define KB_Y      62

static bool shift, shiftLock, running, extHeld;
static int pressed = -1, holdFrames, lastShiftTap = -100, frame;

#define fill  draw_fill
#define glyph draw_glyph
#define text  draw_text

/* ---------------- keys ---------------- */

static u16 key_face(const Key *k) {
	if (k->type == KT_SHIFT && shift) return COL_TINT;
	if (k->code == KC_RUN) return running ? COL_ACCENT_DK : COL_ACCENT;
	switch (k->style) {
	case ST_FUNC: case ST_GREY: return COL_KEY_FN;
	case ST_RED: return COL_ACCENT;
	default: return COL_CARD;
	}
}

static void draw_key(int i) {
	const Key *k = &keys[i];
	bool down = (i == pressed);
	u16 face = down ? COL_TINT : key_face(k);
	draw_box(k->x, k->y, k->w, k->h, face, COL_TRAY, down);

	int cx = k->x + k->w / 2, ty = k->y + (down ? 1 : 0);
	u16 label = draw_label_color(face);
	if (k->type == KT_CHAR) {
		bool letter = (k->ch >= 'a' && k->ch <= 'z');
		if (letter) {
			char c = shift ? k->sch : k->ch;
			glyph(cx - 4, ty + 6, (unsigned char)c, label, 1);
		} else {
			glyph(cx - 4, ty + 1, (unsigned char)k->sch, shift ? COL_ACCENT : COL_MUTED, 1);
			glyph(cx - 4, ty + 11, (unsigned char)k->ch, shift ? COL_MUTED : label, 1);
		}
	} else {
		const char *l = k->label;
		if (k->code == KC_RUN && running) l = "STOP";
		int len = strlen(l);
		text(cx - len * 4, ty + 6, l, (k->type == KT_SHIFT && shift) ? COL_ACCENT : label, 1);
	}
}

static void draw_all_keys(void) {
	for (int i = 0; i < NKEYS; i++) draw_key(i);
}

static void layout(void) {
	int i = 0;
	for (int r = 0; r < 5; r++) {
		int x = KB_X;
		for (int n = 0; n < rowLen[r]; n++, i++) {
			keys[i].x = x;
			keys[i].y = KB_Y + r * ROW_PITCH;
			keys[i].w = keys[i].units * KEY_PITCH - 2;
			keys[i].h = KEY_H;
			x += keys[i].units * KEY_PITCH;
		}
	}
}

/* ---------------- file tabs ---------------- */

#define TAB_MAX 16
static char tabNames[TAB_MAX][13] = { "main.php" };
static int  tabCount = 1, tabCur, tabX[TAB_MAX + 1], tabW[TAB_MAX + 1], tabIdx[TAB_MAX + 1], tabShown;

#define DBG_X 224

static void draw_tabs(void) {
	fill(0, 0, 256, 22, COL_PAGE);
	/* small accent tile with "<>" */
	fill(4, 3, 16, 16, COL_ACCENT);
	fill(4, 3, 1, 1, COL_PAGE); fill(19, 3, 1, 1, COL_PAGE);
	fill(4, 18, 1, 1, COL_PAGE); fill(19, 18, 1, 1, COL_PAGE);
	glyph(5, 7, '<', C(31, 31, 31), 1);
	glyph(11, 7, '>', C(31, 31, 31), 1);
	/* "DBG" tile on the right: debugger */
	fill(DBG_X, 3, 28, 16, COL_KEY_FN);
	fill(DBG_X, 3, 1, 1, COL_PAGE); fill(DBG_X + 27, 3, 1, 1, COL_PAGE);
	fill(DBG_X, 18, 1, 1, COL_PAGE); fill(DBG_X + 27, 18, 1, 1, COL_PAGE);
	text(DBG_X + 2, 7, "DBG", COL_ACCENT, 1);
	fill(0, 21, 256, 1, COL_LINE);

	const int x0 = 24, plusW = 18, maxX = DBG_X - 4 - plusW;
	int w[TAB_MAX];
	for (int i = 0; i < tabCount; i++) {
		int l = 0;
		while (tabNames[i][l] && tabNames[i][l] != '.') l++;
		w[i] = l * 8 + 10;
	}
	/* first visible tab: keep the current one on screen */
	int first = 0;
	for (;;) {
		int sum = 0;
		for (int i = first; i <= tabCur; i++) sum += w[i] + 2;
		if (x0 + sum <= maxX || first >= tabCur) break;
		first++;
	}
	int x = x0;
	tabShown = 0;
	for (int i = first; i < tabCount && x + w[i] <= maxX; i++) {
		char label[13];
		int l = 0;
		while (tabNames[i][l] && tabNames[i][l] != '.') { label[l] = tabNames[i][l]; l++; }
		label[l] = 0;
		bool cur = i == tabCur;
		text(x + 5, 7, label, cur ? COL_ACCENT : COL_MUTED, 1);
		if (cur) fill(x, 19, w[i], 2, COL_ACCENT);
		tabX[tabShown] = x; tabW[tabShown] = w[i]; tabIdx[tabShown] = i; tabShown++;
		x += w[i] + 2;
	}
	/* "+" for a new file */
	text(x + 5, 7, "+", COL_MUTED, 1);
	tabX[tabShown] = x; tabW[tabShown] = plusW; tabIdx[tabShown] = -1;
}

void kbd_set_tabs(const char *const *names, int count, int current) {
	if (count > TAB_MAX) count = TAB_MAX;
	bool changed = count != tabCount || current != tabCur;
	for (int i = 0; i < count && !changed; i++) changed = strncmp(tabNames[i], names[i], 12) != 0;
	if (!changed) return;
	for (int i = 0; i < count; i++) { strncpy(tabNames[i], names[i], 12); tabNames[i][12] = 0; }
	tabCount = count;
	tabCur = current;
	draw_tabs();
}

static int hit_tab(int px, int py) {
	if (py >= 22) return KC_NONE;
	if (px < 22) return KC_BROWSER;
	if (px >= DBG_X - 2) return KC_DEBUG;
	for (int i = 0; i <= tabShown; i++)
		if (px >= tabX[i] && px < tabX[i] + tabW[i]) return tabIdx[i] < 0 ? KC_TABNEW : KC_TAB + tabIdx[i];
	return KC_NONE;
}

static void draw_frame(void) {
	fill(0, 0, 256, 192, COL_PAGE);
	draw_tabs();
	/* tray behind the keys */
	fill(0, KB_Y - 4, 256, 192 - KB_Y + 4, COL_TRAY);
	fill(0, KB_Y - 4, 256, 1, COL_LINE);
}

static char status1[33] = "READY.", status2[33] = "";


/* status card with a red accent bar */
static void draw_status(void) {
	fill(4, 26, 248, 30, COL_LINE);
	fill(5, 27, 246, 28, COL_CARD);
	fill(4, 26, 1, 1, COL_PAGE); fill(251, 26, 1, 1, COL_PAGE);
	fill(4, 55, 1, 1, COL_PAGE); fill(251, 55, 1, 1, COL_PAGE);
	fill(5, 27, 3, 28, COL_ACCENT);
	text(13, 30, status1, COL_TEXT, 1);
	text(13, 42, status2, COL_MUTED, 1);
}

void kbd_set_status(const char *l1, const char *l2) {
	if (!strncmp(l1, status1, 29) && !strncmp(l2, status2, 29)) return;
	strncpy(status1, l1, 29); status1[29] = 0;
	strncpy(status2, l2, 29); status2[29] = 0;
	draw_status();
}

void kbd_set_running(bool r) {
	if (r == running) return;
	running = r;
	for (int i = 0; i < NKEYS; i++)
		if (keys[i].code == KC_RUN) draw_key(i);
}

void kbd_redraw(void) {
	pressed = -2;   /* ignore a stylus that is still down */
	draw_frame();
	draw_status();
	draw_all_keys();
}

void kbd_init(void) {
	videoSetModeSub(MODE_5_2D);
	vramSetBankC(VRAM_C_SUB_BG);
	int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	draw_init(bgGetGfxPtr(bg));
	layout();
	draw_frame();
	draw_status();
	draw_all_keys();
}

static int hit(int px, int py) {
	for (int i = 0; i < NKEYS; i++) {
		const Key *k = &keys[i];
		if (px >= k->x - 1 && px <= k->x + k->w && py >= k->y - 1 && py <= k->y + k->h) return i;
	}
	return -1;
}

static void set_shift(bool s) {
	if (s == shift) return;
	shift = s;
	draw_all_keys();
}

static int emit(int i) {
	const Key *k = &keys[i];
	if (k->type == KT_SHIFT) {
		/* tap = one shot shift, double tap = shift lock */
		if (shift && frame - lastShiftTap < 20) { shiftLock = true; }
		else if (shift) { shiftLock = false; set_shift(false); }
		else set_shift(true);
		lastShiftTap = frame;
		draw_key(i);
		return KC_NONE;
	}
	if (k->type == KT_CHAR) {
		int c = shift ? k->sch : k->ch;
		if (!shiftLock && !extHeld) set_shift(false);
		return c;
	}
	return k->code;
}

static bool repeatable(int i) {
	const Key *k = &keys[i];
	return k->type == KT_CHAR || k->code == KC_DEL || k->code == KC_LEFT ||
	       k->code == KC_RIGHT || k->code == KC_UP || k->code == KC_DOWN || k->code == ' ';
}

int kbd_poll(bool touching, int px, int py, bool extShift) {
	frame++;
	if (extShift != extHeld) {
		extHeld = extShift;
		if (!shiftLock) set_shift(extShift);
	}
	int result = KC_NONE;
	if (touching) {
		int k = hit(px, py);
		if (pressed == -1) {
			if (k >= 0) {
				pressed = k;
				holdFrames = 0;
				draw_key(k);
				result = emit(k);
			} else {
				result = hit_tab(px, py);
				pressed = -2;    /* touched outside: ignore until release */
			}
		} else if (pressed >= 0) {
			if (k == pressed) {
				holdFrames++;
				if (repeatable(k) && holdFrames > 24 && (holdFrames - 24) % 4 == 0) result = emit(k);
			} else {
				int old = pressed;
				pressed = -2;
				draw_key(old);
			}
		}
	} else if (pressed != -1) {
		int old = pressed;
		pressed = -1;
		if (old >= 0) draw_key(old);
	}
	return result;
}
