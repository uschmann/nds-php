/*
 * title.c - title screen: the words "NDS PHP" as 3D voxel lettering rendered
 * with the DS 3D hardware on the top screen; the list of saved programs on the
 * bottom screen.
 *
 * At the start every voxel drops in from above the screen and the logo
 * builds up. Lighting is done in software per face (flat shading); every
 * voxel has its own polygon id, so edge marking outlines the single cubes.
 */
#include <nds.h>
#include <math.h>
#include <string.h>
#include "draw.h"
#include "title.h"
#include "lang.h"
#include "basic.h"
#include <stdio.h>

typedef struct { float x, y, z; } V3;

static V3 lightDir;          /* light direction in model space */
static int polyId;

static V3 v3(float x, float y, float z) { V3 v = { x, y, z }; return v; }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) {
	float l = sqrtf(dot(a, a));
	return l > 0 ? v3(a.x / l, a.y / l, a.z / l) : a;
}

/* start a new part: own polygon id so edge marking outlines it */
static void part(void) {
	polyId = (polyId % 62) + 1;
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(polyId));
}

/* flat shaded quad; 'inside' is a point inside the part (fixes normal direction) */
static void quad(V3 a, V3 b, V3 c, V3 d, int r, int g, int bl, V3 inside) {
	V3 n = norm(cross(sub(b, a), sub(c, a)));
	V3 mid = v3((a.x + c.x) / 2, (a.y + c.y) / 2, (a.z + c.z) / 2);
	if (dot(n, sub(mid, inside)) < 0) n = v3(-n.x, -n.y, -n.z);
	float lit = dot(n, lightDir);
	float k = 0.45f + 0.65f * (lit > 0 ? lit : 0);
	int R = (int)(r * k), G = (int)(g * k), B = (int)(bl * k);
	glColor(RGB15(R > 31 ? 31 : R, G > 31 ? 31 : G, B > 31 ? 31 : B));
	glBegin(GL_QUADS);
	glVertex3f(a.x, a.y, a.z);
	glVertex3f(b.x, b.y, b.z);
	glVertex3f(c.x, c.y, c.z);
	glVertex3f(d.x, d.y, d.z);
	glEnd();
}

/* flat shaded triangle (lit like quad) */
static void tri(V3 a, V3 b, V3 c, int r, int g, int bl, V3 inside) {
	V3 n = norm(cross(sub(b, a), sub(c, a)));
	V3 mid = v3((a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3);
	if (dot(n, sub(mid, inside)) < 0) n = v3(-n.x, -n.y, -n.z);
	float lit = dot(n, lightDir);
	float k = 0.45f + 0.65f * (lit > 0 ? lit : 0);
	int R = (int)(r * k), G = (int)(g * k), B = (int)(bl * k);
	glColor(RGB15(R > 31 ? 31 : R, G > 31 ? 31 : G, B > 31 ? 31 : B));
	glBegin(GL_TRIANGLES);
	glVertex3f(a.x, a.y, a.z);
	glVertex3f(b.x, b.y, b.z);
	glVertex3f(c.x, c.y, c.z);
	glEnd();
}

/* --- voxel lettering: 5x7 pixel glyphs built from small cubes --- */

#define VOX     0.15f             /* edge length of one voxel          */
#define VDEPTH  0.30f             /* depth of the letters              */

static const char *const glyphs[][7] = {
	{ "X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X", "X...X" },   /* N */
	{ "XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX." },   /* D */
	{ ".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX." },   /* S */
	{ "XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...." },   /* P */
	{ "X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X" },   /* H */
};

static bool vox_at(int g, int x, int y) {
	return x >= 0 && x < 5 && y >= 0 && y < 7 && glyphs[g][y][x] == 'X';
}

/* one cube; faces hidden by a neighbour in the same letter are skipped */
static void voxel(int g, int gx, int gy, float x0, float y1, int r, int gr, int b) {
	float x1 = x0 + VOX, y0 = y1 - VOX, z0 = -VDEPTH / 2, z1 = VDEPTH / 2;
	V3 in = v3((x0 + x1) / 2, (y0 + y1) / 2, 0);
	part();
	quad(v3(x0, y0, z1), v3(x1, y0, z1), v3(x1, y1, z1), v3(x0, y1, z1), r, gr, b, in);   /* front */
	quad(v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y1, z0), v3(x0, y1, z0), r, gr, b, in);   /* back  */
	if (!vox_at(g, gx, gy - 1)) quad(v3(x0, y1, z0), v3(x1, y1, z0), v3(x1, y1, z1), v3(x0, y1, z1), r, gr, b, in);
	if (!vox_at(g, gx, gy + 1)) quad(v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y0, z1), v3(x0, y0, z1), r, gr, b, in);
	if (!vox_at(g, gx - 1, gy)) quad(v3(x0, y0, z0), v3(x0, y0, z1), v3(x0, y1, z1), v3(x0, y1, z0), r, gr, b, in);
	if (!vox_at(g, gx + 1, gy)) quad(v3(x1, y0, z0), v3(x1, y0, z1), v3(x1, y1, z1), v3(x1, y1, z0), r, gr, b, in);
}

/* intro: every voxel drops in from above the screen, lands with a small
 * bounce; the lower word builds up first, row by row from the bottom */
#define DROP_FROM   2.8f          /* start height (above the visible top) */
#define GRAVITY     9.0f
#define ASSEMBLED   3.1f          /* all voxels have landed by then        */

static float jitter(int a, int b, int c) {
	unsigned h = (unsigned)(a * 73856093) ^ (unsigned)(b * 19349663) ^ (unsigned)(c * 83492791);
	h *= 2654435761u;
	return (float)((h >> 16) & 255) / 255.0f;
}

/* height above the resting place, or < 0 while the voxel has not started */
static float drop(float t, float delay, float height) {
	float e = t - delay;
	if (e < 0) return -1;
	float fallTime = sqrtf(2 * height / GRAVITY);
	if (e < fallTime) return height - 0.5f * GRAVITY * e * e;
	e -= fallTime;
	return 0.10f * fabsf(sinf(e * 11.0f)) * expf(-e * 7.0f);
}

/* a word centred at x = 0 with its top at y; once assembled the letters
 * hop in a little wave */
static void word(const int *g, int n, float top, int r, int gr, int b, float t, int phase0, float start) {
	float w = (n * 6 - 1) * VOX;
	float waveIn = (t - ASSEMBLED) / 0.6f;
	waveIn = waveIn < 0 ? 0 : waveIn > 1 ? 1 : waveIn;
	for (int i = 0; i < n; i++) {
		float hop = sinf(t * 2.6f - (i + phase0) * 0.8f);
		float dy = hop > 0 ? hop * 0.10f * waveIn : 0;
		float lx = -w / 2 + i * 6 * VOX;
		for (int y = 0; y < 7; y++)
			for (int x = 0; x < 5; x++) {
				if (!vox_at(g[i], x, y)) continue;
				float rest = top + dy - y * VOX;
				float delay = start + (6 - y) * 0.11f + i * 0.06f + jitter(phase0 + i, x, y) * 0.18f;
				float up = drop(t, delay, DROP_FROM - rest + jitter(x, y, i) * 0.6f);
				if (up < 0) continue;
				voxel(g[i], x, y, lx + x * VOX, rest + up, r, gr, b);
			}
	}
}

static void lettering(float t) {
	static const int nds[3] = { 0, 1, 2 }, php[3] = { 3, 4, 3 };
	word(php, 3, -1 * VOX, 30, 6, 0, t, 3, 0.2f);
	word(nds, 3, 8 * VOX, 7, 7, 8, t, 0, 1.15f);
}

/* soft shadow on the ground below the letters */
static void shadow(void) {
	const float y = -9 * VOX;
	part();
	V3 below = v3(0, y - 5, 0);              /* normal points up: lit */
	for (int i = 0; i < 12; i++) {
		float a0 = i * 6.2831853f / 12, a1 = (i + 1) * 6.2831853f / 12;
		tri(v3(0, y, 0), v3(1.45f * cosf(a0), y, 0.40f * sinf(a0)),
		    v3(1.45f * cosf(a1), y, 0.40f * sinf(a1)), 26, 26, 25, below);
	}
}

static void render_scene(float angle, float t) {
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(0.0f, 1.10f, 5.40f, 0.0f, -0.05f, 0.0f, 0.0f, 1.0f, 0.0f);
	float bob = sinf(t * 1.3f) * 0.04f;
	glTranslatef(0.0f, bob, 0.0f);
	glRotateY(angle);

	/* light from upper left front, transformed into model space */
	float a = -angle * 3.14159265f / 180.0f;
	V3 L = norm(v3(-0.5f, 0.8f, 0.6f));
	lightDir = v3(L.x * cosf(a) + L.z * sinf(a), L.y, -L.x * sinf(a) + L.z * cosf(a));

	polyId = 0;
	shadow();
	lettering(t);
	glFlush(0);
}

/* ---------------- bottom screen: program list ---------------- */

#define LIST_X0   6
#define LIST_X1   130
#define LIST_Y    52
#define ITEM_W    120
#define ITEM_H    17
#define ITEM_DY   19
#define PER_PAGE  12
#define BTN_Y     170

static char files[BASIC_MAX_FILES][BASIC_NAME_BUF];
static int nfiles, page, pressed = -1;
static bool hasProgram;

enum { T_NONE = -1, T_PREV = -2, T_NEXT = -3, T_CONTINUE = -4 };   /* >= 0: file index */

static int pages(void) { return nfiles ? (nfiles + PER_PAGE - 1) / PER_PAGE : 1; }

static void item_rect(int i, int *x, int *y) {
	int k = i % PER_PAGE;
	*x = (k / 6) ? LIST_X1 : LIST_X0;
	*y = LIST_Y + (k % 6) * ITEM_DY;
}

static void draw_item(int i) {
	int x, y;
	char name[BASIC_NAME_BUF];
	item_rect(i, &x, &y);
	strcpy(name, files[i]);
	char *dot = strchr(name, '.');
	if (dot) *dot = 0;
	int room = (ITEM_W - 8) / 8;               /* long names are cut to the button */
	if ((int)strlen(name) > room) { name[room - 1] = '~'; name[room] = 0; }
	draw_button(x, y, ITEM_W, ITEM_H, name, COL_CARD, i == pressed);
}

static void draw_buttons(void) {
	draw_button(6, BTN_Y, 30, 18, "\x1b", COL_KEY_FN, pressed == T_PREV);
	draw_button(40, BTN_Y, 30, 18, "\x1a", COL_KEY_FN, pressed == T_NEXT);
	draw_button(76, BTN_Y, 174, 18, hasProgram ? L("ZUM PROGRAMM", "BACK TO PROGRAM") : L("NEUES PROGRAMM", "NEW PROGRAM"),
	            COL_ACCENT, pressed == T_CONTINUE);
}

static void draw_list(void) {
	char b[24];
	draw_fill(0, 36, 256, BTN_Y - 36 - 2, COL_BODY);
	draw_text(8, 40, L("PROGRAMME", "PROGRAMS"), COL_MUTED, 1);
	if (nfiles > PER_PAGE) {
		sprintf(b, "%d/%d", page + 1, pages());
		draw_text(250 - 8 * (int)strlen(b), 40, b, COL_MUTED, 1);
	}
	if (nfiles < 0) {
		draw_text(L(36, 64), 90, L("KEINE SD-KARTE GEFUNDEN", "NO SD CARD FOUND"), COL_ACCENT, 1);
		draw_text(L(28, 52), 108, L("SPEICHERN NICHT M\x99GLICH", "SAVING NOT POSSIBLE"), COL_MUTED, 1);
	} else if (nfiles == 0) {
		draw_text(L(44, 68), 90, L("NOCH KEINE PROGRAMME", "NO PROGRAMS YET"), COL_TEXT, 1);
		draw_text(L(32, 48), 108, L("SPEICHERN: SAVE \"NAME\"", "TO SAVE: SAVE \"NAME\""), COL_MUTED, 1);
	} else {
		for (int i = page * PER_PAGE; i < nfiles && i < (page + 1) * PER_PAGE; i++) draw_item(i);
	}
}

static void draw_bottom(void) {
	draw_fill(0, 0, 256, 192, COL_PAGE);
	draw_page_header(34);
	draw_list();
	draw_buttons();
}

static int hit_target(int px, int py) {
	if (py >= BTN_Y && py < BTN_Y + 18) {
		if (px >= 6 && px < 36) return T_PREV;
		if (px >= 40 && px < 70) return T_NEXT;
		if (px >= 76 && px < 250) return T_CONTINUE;
	}
	for (int i = page * PER_PAGE; i < nfiles && i < (page + 1) * PER_PAGE; i++) {
		int x, y;
		item_rect(i, &x, &y);
		if (px >= x && px < x + ITEM_W && py >= y && py < y + ITEM_H) return i;
	}
	return T_NONE;
}

static void redraw_target(int t) {
	if (t >= 0) draw_item(t);
	else if (t != T_NONE) draw_buttons();
}

bool title_screen(char *chosen, bool programInMemory) {
	/* bottom: 16-bit bitmap like the keyboard */
	videoSetModeSub(MODE_5_2D);
	vramSetBankC(VRAM_C_SUB_BG);
	int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	draw_init(bgGetGfxPtr(bg));

	/* top: 3D */
	videoSetMode(MODE_0_3D);
	glInit();
	glEnable(GL_ANTIALIAS | GL_OUTLINE);
	glSetOutlineColor(0, RGB15(10, 10, 10));
	glClearColor(31, 31, 30, 31);
	glClearPolyID(63);
	glClearDepth(0x7FFF);
	glViewport(0, 0, 255, 191);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(36, 256.0f / 192.0f, 0.1f, 40);

	hasProgram = programInMemory;
	nfiles = basic_list_files(files, BASIC_MAX_FILES);
	page = 0;
	pressed = T_NONE;
	draw_bottom();

	float angle = 0.0f, t = 0;
	bool result = false;
	scanKeys();       /* ignore keys that are still held from boot */
	while (pmMainLoop()) {
		render_scene(angle, t);
		swiWaitForVBlank();
		t += 1.0f / 60.0f;
		angle = sinf(t * 0.45f) * 18.0f;   /* gentle swing around the front view */

		scanKeys();
		u32 down = keysDown();
		if (down & ~KEY_TOUCH) break;                       /* any button: continue */
		if (keysHeld() & KEY_TOUCH) {
			if (!(down & KEY_TOUCH)) continue;
			touchPosition tp;
			touchRead(&tp);
			int tg = hit_target(tp.px, tp.py);
			if (tg == T_NONE) continue;
			pressed = tg;
			redraw_target(tg);
			if (tg == T_CONTINUE) break;
			if (tg >= 0) { strcpy(chosen, files[tg]); result = true; break; }
			int np = page + (tg == T_NEXT ? 1 : -1);
			if (np >= 0 && np < pages()) { page = np; draw_list(); }
		} else if (pressed != T_NONE) {
			int old = pressed;
			pressed = T_NONE;
			redraw_target(old);
		}
	}
	/* wait for release so the key/tap does not reach the editor */
	do { swiWaitForVBlank(); scanKeys(); } while (keysHeld() && pmMainLoop());
	/* leave 3D mode; video_init() sets up the 2D screens again */
	videoSetMode(MODE_0_2D);
	return result;
}
