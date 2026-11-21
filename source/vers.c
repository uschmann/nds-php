/*
 * vers.c - version list of the current program (bottom screen)
 *
 * Shows the versions saved with COMMIT, newest first. The top screen
 * (rendered by main.c from vers_preview_*()) shows what changed between the
 * selected version and the program in the editor. DIFF, CHECKOUT and COMMIT
 * themselves are run by main.c as REPL commands.
 */
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "vers.h"
#include "draw.h"
#include "basic.h"
#include "lang.h"

#define LIST_Y    26
#define ROW_H     17
#define ROWS      7
#define MSG_Y     148
#define BTN_Y     164
#define BTN_H     22
#define MAX_VERS  200
#define PREVIEW_LINES 22
#define PREVIEW_LEN   64

static BasicVersion vers[MAX_VERS];
static int  nvers, sel, top, base;
static bool noProgram;
static char message[33];
static int  confirm = -1, pressed = -1;
static int  target;
static bool force;

static char previewTitle[40];
static char preview[PREVIEW_LINES][PREVIEW_LEN + 1];
static int  npreview;

/* ---------------- buttons ---------------- */

enum { B_UP, B_DOWN, B_DIFF, B_LOAD, B_COMMIT, B_CLOSE, NBTN };
typedef struct { short x, w; } BtnPos;
static const BtnPos btnPos[NBTN] = {
	{ 6, 22 }, { 30, 22 }, { 54, 44 }, { 100, 52 }, { 154, 70 }, { 228, 22 }
};

static const char *btn_label(int b) {
	if (confirm == b) return L("JA?", "YES?");
	switch (b) {
	case B_UP:     return "\x18";
	case B_DOWN:   return "\x19";
	case B_DIFF:   return "DIFF";
	case B_LOAD:   return L("LADEN", "LOAD");
	case B_COMMIT: return "COMMIT";
	default:       return "X";
	}
}

static void draw_btn(int b) {
	const BtnPos *p = &btnPos[b];
	u16 face = b == B_COMMIT ? COL_ACCENT : COL_KEY_FN;
	if (confirm == b) face = COL_ACCENT_DK;
	if ((b == B_DIFF || b == B_LOAD) && nvers == 0) face = COL_TRAY;
	if (b == B_COMMIT && noProgram) face = COL_TRAY;
	draw_button(p->x, BTN_Y, p->w, BTN_H, btn_label(b), face, b == pressed);
}

static void draw_buttons(void) {
	draw_fill(0, BTN_Y - 2, 256, BTN_H + 4, COL_PAGE);
	for (int b = 0; b < NBTN; b++) draw_btn(b);
}

/* ---------------- list ---------------- */

static void draw_header(void) {
	draw_fill(0, 0, 256, 22, COL_PAGE);
	draw_fill(4, 3, 16, 16, COL_ACCENT);
	draw_fill(4, 3, 1, 1, COL_PAGE); draw_fill(19, 3, 1, 1, COL_PAGE);
	draw_fill(4, 18, 1, 1, COL_PAGE); draw_fill(19, 18, 1, 1, COL_PAGE);
	/* clock face: history */
	static const u8 clock[8] = { 0x3C, 0x42, 0x91, 0x91, 0x9D, 0x81, 0x42, 0x3C };
	for (int r = 0; r < 8; r++)
		for (int b = 0; b < 8; b++)
			if (clock[r] & (0x80 >> b)) draw_fill(8 + b, 7 + r, 1, 1, C(31, 31, 31));
	const char *root = L("VERSIONEN", "VERSIONS");
	draw_text(26, 7, root, COL_ACCENT, 1);
	const char *name = basic_program_name();
	if (name[0]) {
		int x = 26 + 8 * (int)strlen(root);
		draw_text(x, 7, " / ", COL_MUTED, 1);
		draw_text(x + 24, 7, name, COL_TEXT, 1);
	}
	draw_fill(0, 21, 256, 1, COL_LINE);
}

static void draw_row(int r) {
	int i = top + r, y = LIST_Y + r * ROW_H;
	draw_fill(4, y, 248, ROW_H - 1, COL_PAGE);
	if (i >= nvers) return;
	bool s = i == sel;
	if (s) {
		draw_fill(4, y, 248, ROW_H - 1, COL_TINT);
		draw_fill(4, y, 3, ROW_H - 1, COL_ACCENT);
	}
	/* the version the program is based on: a dot */
	bool cur = vers[i].num == base || (!base && i == 0);
	if (cur) draw_fill(10, y + 6, 4, 4, COL_ACCENT);
	char num[8], date[12];
	snprintf(num, sizeof num, "%3d", vers[i].num);
	draw_text(16, y + 4, num, s ? COL_ACCENT : COL_TEXT, 1);
	/* "MM-DD" of "YYYY-MM-DD HH:MM", right aligned */
	snprintf(date, sizeof date, "%.5s", strlen(vers[i].date) >= 10 ? vers[i].date + 5 : vers[i].date);
	draw_text(248 - 8 * (int)strlen(date), y + 4, date, COL_MUTED, 1);
	char msg[BASIC_VC_MSG + 1];
	int room = (248 - 8 * (int)strlen(date) - 8 - 48) / 8;
	snprintf(msg, sizeof msg, "%.*s", room, vers[i].msg);
	draw_text(48, y + 4, msg, s ? COL_ACCENT : COL_TEXT, 1);
}

static void draw_list(void) {
	draw_fill(0, 22, 256, MSG_Y - 22, COL_PAGE);
	for (int r = 0; r < ROWS; r++) draw_row(r);
	if (nvers == 0) {
		const char *t1 = !basic_has_storage() ? L("KEINE SD-KARTE", "NO SD CARD")
		               : noProgram ? L("ERST SPEICHERN", "SAVE THE PROGRAM FIRST")
		               : L("NOCH KEINE VERSIONEN", "NO VERSIONS YET");
		draw_text(128 - 4 * (int)strlen(t1), 64, t1, COL_MUTED, 1);
		if (basic_has_storage() && !noProgram) {
			const char *t2 = L("COMMIT SPEICHERT DEN STAND", "COMMIT KEEPS THE CURRENT STATE");
			draw_text(128 - 4 * (int)strlen(t2), 80, t2, COL_MUTED, 1);
		}
	}
	if (top > 0) draw_text(240, 22, "\x1e", COL_MUTED, 1);
	if (top + ROWS < nvers) draw_text(240, MSG_Y - 10, "\x1f", COL_MUTED, 1);
}

static void draw_message(void) {
	draw_fill(0, MSG_Y, 256, BTN_Y - 2 - MSG_Y, COL_PAGE);
	draw_text(8, MSG_Y + 2, message, confirm >= 0 ? COL_ACCENT : COL_MUTED, 1);
}

static void draw_all(void) {
	draw_fill(0, 0, 256, 192, COL_PAGE);
	draw_header();
	draw_list();
	draw_message();
	draw_buttons();
}

static void set_message(const char *m) {
	strncpy(message, m, 31);
	message[31] = 0;
	draw_message();
}

/* ---------------- preview: the diff ---------------- */

static void update_preview(void) {
	static char text[8192];
	npreview = 0;
	if (nvers == 0) {
		snprintf(previewTitle, sizeof previewTitle, "%s", L("VERSIONEN", "VERSIONS"));
		return;
	}
	snprintf(previewTitle, sizeof previewTitle, L("VERSION %d -> JETZT", "VERSION %d -> NOW"), vers[sel].num);
	basic_vc_diff(vers[sel].num, text, sizeof text);
	for (char *ln = text; *ln && npreview < PREVIEW_LINES; ) {
		char *nl = strchr(ln, '\n');
		int l = nl ? (int)(nl - ln) : (int)strlen(ln);
		snprintf(preview[npreview++], PREVIEW_LEN + 1, "%.*s", l < PREVIEW_LEN ? l : PREVIEW_LEN, ln);
		if (!nl) break;
		ln = nl + 1;
	}
}

/* ---------------- list handling ---------------- */

static void read_list(void) {
	int n = basic_vc_list(vers, MAX_VERS);
	noProgram = n < 0;
	nvers = n < 0 ? 0 : n;
	base = basic_vc_base();
	if (sel >= nvers) sel = nvers ? nvers - 1 : 0;
	if (sel < 0) sel = 0;
}

static void keep_visible(void) {
	if (sel < top) top = sel;
	if (sel >= top + ROWS) top = sel - ROWS + 1;
	if (top < 0) top = 0;
}

static void select_entry(int i) {
	if (i < 0 || i >= nvers || i == sel) return;
	sel = i;
	confirm = -1;
	message[0] = 0;
	keep_visible();
	draw_list();
	draw_message();
	draw_buttons();
	update_preview();
}

/* ---------------- public ---------------- */

void vers_open(void) {
	sel = top = 0;
	confirm = -1;
	pressed = -2;            /* the tap that opened the panel is still down */
	message[0] = 0;
	read_list();
	/* start on the version the program is based on */
	for (int i = 0; i < nvers; i++) if (base && vers[i].num == base) sel = i;
	keep_visible();
	draw_all();
	update_preview();
}

void vers_refresh(void) {
	read_list();
	keep_visible();
	confirm = -1;
	draw_all();
	update_preview();
}

void vers_set_message(const char *m) { set_message(m); }

void vers_ask_force(void) {
	confirm = B_LOAD;
	set_message(L("\x8eNDERUNGEN VERWERFEN?", "DISCARD THE CHANGES?"));
	draw_buttons();
}

int vers_target(void) { return target; }
bool vers_force(void) { return force; }
const char *vers_preview_title(void) { return previewTitle; }
int vers_preview_count(void) { return npreview; }
const char *vers_preview_line(int i) { return (i >= 0 && i < npreview) ? preview[i] : ""; }

static int press(int b) {
	switch (b) {
	case B_UP:   select_entry(sel - 1); return VR_NONE;
	case B_DOWN: select_entry(sel + 1); return VR_NONE;
	case B_CLOSE: return VR_EXIT;
	case B_DIFF:
		if (!nvers) return VR_NONE;
		target = vers[sel].num;
		return VR_DIFF;
	case B_LOAD:
		if (!nvers) return VR_NONE;
		force = confirm == B_LOAD;
		confirm = -1;
		target = vers[sel].num;
		return VR_CHECKOUT;
	case B_COMMIT:
		if (noProgram) { set_message(L("ERST SPEICHERN", "SAVE THE PROGRAM FIRST")); return VR_NONE; }
		return VR_COMMIT;
	}
	return VR_NONE;
}

static int hit_button(int px, int py) {
	if (py < BTN_Y || py >= BTN_Y + BTN_H) return -1;
	for (int b = 0; b < NBTN; b++)
		if (px >= btnPos[b].x && px < btnPos[b].x + btnPos[b].w) return b;
	return -1;
}

int vers_poll(bool touching, int px, int py, u32 down, u32 rep) {
	int result = VR_NONE;
	if (rep & KEY_UP) select_entry(sel - 1);
	if (rep & KEY_DOWN) select_entry(sel + 1);
	if (rep & KEY_LEFT) select_entry(sel - ROWS > 0 ? sel - ROWS : 0);
	if (rep & KEY_RIGHT) select_entry(sel + ROWS < nvers ? sel + ROWS : nvers - 1);
	if (down & KEY_A) result = press(B_DIFF);
	if (down & KEY_B) result = press(B_CLOSE);

	if (touching) {
		if (pressed == -1) {
			int b = hit_button(px, py);
			if (b >= 0) {
				pressed = b;
				draw_btn(b);
			} else if (py >= LIST_Y && py < LIST_Y + ROWS * ROW_H) {
				int i = top + (py - LIST_Y) / ROW_H;
				if (i < nvers) select_entry(i);
				pressed = -2;
			} else if (px < 22 && py < 22) {
				pressed = -2;
				result = VR_EXIT;
			} else pressed = -2;
		}
	} else if (pressed != -1) {
		int b = pressed;
		pressed = -1;
		if (b >= 0) {
			draw_btn(b);
			result = press(b);
		}
	}
	return result;
}
