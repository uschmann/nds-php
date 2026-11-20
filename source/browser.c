/*
 * browser.c - file browser for the saved programs (bottom screen)
 *
 * Level 1 lists the programs in /nds-php (project folders and old single
 * files), level 2 the files of one program. The selected entry is previewed
 * on the top screen (rendered by main.c from browser_preview_*()).
 * Loading and deleting whole programs is done by the interpreter (main.c
 * runs LOAD/DELETE); single files inside a folder are removed here.
 */
#include <nds.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "browser.h"
#include "draw.h"
#include "basic.h"
#include "lang.h"

#define LIST_Y    26
#define ROW_H     17
#define ROWS      7
#define MSG_Y     148
#define BTN_Y     164
#define BTN_H     22
#define PREVIEW_LINES 22
#define PREVIEW_LEN   64

typedef struct { char name[16]; char info[16]; } Entry;

static Entry entries[BASIC_MAX_FILES];
static int   nentries, sel, top, level;          /* level 0 = programs, 1 = files */
static char  project[16];                         /* program opened in level 1    */
static bool  projectIsFolder;
static char  message[33];
static int   confirm = -1;                        /* button waiting for a 2nd tap */
static int   pressed = -1;

static char  previewTitle[40];
static char  preview[PREVIEW_LINES][PREVIEW_LEN + 1];
static int   npreview;
static bool  previewCode;

static char  target[16], targetFile[16];

/* ---------------- buttons ---------------- */

enum { B_UP, B_DOWN, B_OPEN, B_LOAD, B_DEL, B_CLOSE, NBTN };
typedef struct { short x, w; } BtnPos;
static const BtnPos btnPos[2][NBTN] = {
	/* programs:  up      down     open      load      delete    close */
	{ { 6, 22 }, { 30, 22 }, { 54, 56 }, { 112, 48 }, { 162, 64 }, { 228, 22 } },
	/* files:     up      down     -         load      delete    back */
	{ { 6, 22 }, { 30, 22 }, { 0, 0 },   { 54, 48 },  { 104, 64 }, { 170, 80 } },
};

static const char *btn_label(int b) {
	if (confirm == b) return b == B_LOAD ? L("JA?", "YES?") : L("SICHER?", "SURE?");
	switch (b) {
	case B_UP:    return "\x18";
	case B_DOWN:  return "\x19";
	case B_OPEN:  return L("\x99" "FFNEN", "OPEN");
	case B_LOAD:  return L("LADEN", "LOAD");
	case B_DEL:   return L("L\x99SCHEN", "DELETE");
	default:      return level ? L("ZUR\x9a" "CK", "BACK") : "X";
	}
}

static void draw_btn(int b) {
	const BtnPos *p = &btnPos[level][b];
	if (!p->w) return;
	u16 face = COL_KEY_FN;
	if (b == B_LOAD) face = COL_ACCENT;
	if (confirm == b) face = COL_ACCENT_DK;
	bool off = (b == B_OPEN || b == B_LOAD || b == B_DEL) && nentries == 0;
	if (b == B_DEL && level == 1 && sel == 0) off = true;          /* main.php stays */
	if (off) face = COL_TRAY;
	const char *label = btn_label(b);
	draw_button(p->x, BTN_Y, p->w, BTN_H, label, face, b == pressed);
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
	draw_glyph(5, 7, '<', C(31, 31, 31), 1);
	draw_glyph(11, 7, '>', C(31, 31, 31), 1);
	const char *root = L("PROGRAMME", "PROGRAMS");
	draw_text(26, 7, root, level ? COL_MUTED : COL_ACCENT, 1);
	if (level) {
		int x = 26 + 8 * (int)strlen(root);
		draw_text(x, 7, " / ", COL_MUTED, 1);
		draw_text(x + 24, 7, project, COL_ACCENT, 1);
	}
	draw_fill(0, 21, 256, 1, COL_LINE);
}

static void draw_row(int r) {
	int i = top + r, y = LIST_Y + r * ROW_H;
	draw_fill(4, y, 248, ROW_H - 1, COL_PAGE);
	if (i >= nentries) return;
	bool s = i == sel;
	if (s) {
		draw_fill(4, y, 248, ROW_H - 1, COL_TINT);
		draw_fill(4, y, 3, ROW_H - 1, COL_ACCENT);
	}
	draw_text(12, y + 4, entries[i].name, s ? COL_ACCENT : COL_TEXT, 1);
	int il = (int)strlen(entries[i].info);
	draw_text(248 - il * 8, y + 4, entries[i].info, COL_MUTED, 1);
}

static void draw_list(void) {
	draw_fill(0, 22, 256, MSG_Y - 22, COL_PAGE);
	if (nentries == 0) {
		const char *t = !basic_has_storage() ? L("KEINE SD-KARTE", "NO SD CARD")
		              : level ? L("KEINE DATEIEN", "NO FILES") : L("NOCH KEINE PROGRAMME", "NO PROGRAMS YET");
		draw_text(128 - 4 * (int)strlen(t), 70, t, COL_MUTED, 1);
	}
	for (int r = 0; r < ROWS; r++) draw_row(r);
	/* scroll marks */
	if (top > 0) draw_text(240, 22, "\x1e", COL_MUTED, 1);
	if (top + ROWS < nentries) draw_text(240, MSG_Y - 10, "\x1f", COL_MUTED, 1);
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

/* ---------------- reading the card ---------------- */

static void path_of(char *out, int size, const char *a, const char *b) {
	if (b) snprintf(out, size, "%.100s/%.15s/%.15s", basic_storage_dir(), a, b);
	else snprintf(out, size, "%.100s/%.15s", basic_storage_dir(), a);
}

static int count_lines(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	/* like the editor: without the "<?php" line and the #SPR / #SND lines */
	int n = 0;
	bool first = true;
	char raw[300];
	while (fgets(raw, sizeof raw, f)) {
		if (first && !strncasecmp(raw, "<?php", 5)) { first = false; continue; }
		first = false;
		if (!strncmp(raw, "#SPR ", 5) || !strncmp(raw, "#SND ", 5)) continue;
		n++;
	}
	fclose(f);
	return n;
}

static int cmp_entries(const void *a, const void *b) {
	const Entry *x = a, *y = b;
	if (!strcmp(x->name, "main.php")) return -1;
	if (!strcmp(y->name, "main.php")) return 1;
	return strcmp(x->name, y->name);
}

static int project_files(const char *proj, Entry *out, int max) {
	char path[160], fpath[200];
	path_of(path, sizeof path, proj, NULL);
	DIR *d = opendir(path);
	if (!d) return 0;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 5 || l > 12 || strcasecmp(e->d_name + l - 4, ".php")) continue;
		for (size_t k = 0; k <= l; k++) out[n].name[k] = (char)tolower((unsigned char)e->d_name[k]);
		snprintf(fpath, sizeof fpath, "%s/%s", path, e->d_name);
		int lines = count_lines(fpath);
		snprintf(out[n].info, sizeof out[n].info, L("%d ZEILE%s", "%d LINE%s"), lines,
		         lines == 1 ? "" : L("N", "S"));
		n++;
	}
	closedir(d);
	qsort(out, n, sizeof(Entry), cmp_entries);
	return n;
}

static void read_preview_file(const char *path, int maxLines) {
	FILE *f = fopen(path, "r");
	if (!f) return;
	char raw[300];
	bool first = true;
	while (npreview < PREVIEW_LINES && maxLines-- > 0 && fgets(raw, sizeof raw, f)) {
		raw[strcspn(raw, "\r\n")] = 0;
		if (first && !strncasecmp(raw, "<?php", 5) && !raw[5]) { first = false; maxLines++; continue; }
		first = false;
		if (!strncmp(raw, "#SPR ", 5) || !strncmp(raw, "#SND ", 5)) { maxLines++; continue; }
		/* tabs to two spaces, cut at the preview width */
		int o = 0;
		for (int i = 0; raw[i] && o < PREVIEW_LEN; i++) {
			if (raw[i] == '\t') { preview[npreview][o++] = ' '; if (o < PREVIEW_LEN) preview[npreview][o++] = ' '; }
			else preview[npreview][o++] = raw[i];
		}
		preview[npreview][o] = 0;
		npreview++;
	}
	fclose(f);
}

static void add_preview_text(const char *t) {
	if (npreview >= PREVIEW_LINES) return;
	strncpy(preview[npreview], t, PREVIEW_LEN);
	preview[npreview][PREVIEW_LEN] = 0;
	npreview++;
}

static bool is_folder(const char *name) { return strchr(name, '.') == NULL; }

static void update_preview(void) {
	char path[200];
	npreview = 0;
	previewCode = true;
	if (nentries == 0) {
		strcpy(previewTitle, level ? project : L("PROGRAMME", "PROGRAMS"));
		return;
	}
	if (level == 0) {
		const char *name = entries[sel].name;
		snprintf(previewTitle, sizeof previewTitle, "%s", name);
		if (is_folder(name)) {
			/* file list, then the start of main.php */
			Entry files[BASIC_PROJECT_FILES];
			int n = project_files(name, files, BASIC_PROJECT_FILES);
			for (int i = 0; i < n; i++) {
				char line[40];
				snprintf(line, sizeof line, "// %-12.12s %.15s", files[i].name, files[i].info);
				add_preview_text(line);
			}
			add_preview_text("");
			path_of(path, sizeof path, name, "main.php");
		} else path_of(path, sizeof path, name, NULL);
		read_preview_file(path, PREVIEW_LINES);
	} else {
		snprintf(previewTitle, sizeof previewTitle, "%s/%s", project, entries[sel].name);
		if (projectIsFolder) path_of(path, sizeof path, project, entries[sel].name);
		else path_of(path, sizeof path, project, NULL);
		read_preview_file(path, PREVIEW_LINES);
	}
}

static void read_list(void) {
	nentries = 0;
	if (level == 0) {
		static char names[BASIC_MAX_FILES][16];
		int n = basic_list_files(names, BASIC_MAX_FILES);
		char path[200];
		for (int i = 0; i < n; i++) {
			Entry *e = &entries[nentries++];
			strcpy(e->name, names[i]);
			if (is_folder(names[i])) {
				Entry files[BASIC_PROJECT_FILES];
				int nf = project_files(names[i], files, BASIC_PROJECT_FILES);
				snprintf(e->info, sizeof e->info, L("%d DATEI%s", "%d FILE%s"), nf, nf == 1 ? "" : L("EN", "S"));
			} else {
				path_of(path, sizeof path, names[i], NULL);
				int lines = count_lines(path);
				snprintf(e->info, sizeof e->info, L("1 DATEI", "1 FILE"));
				(void)lines;
			}
		}
	} else if (projectIsFolder) {
		nentries = project_files(project, entries, BASIC_MAX_FILES);
	} else {
		/* old single file: it becomes main.php when loaded */
		char path[200];
		path_of(path, sizeof path, project, NULL);
		strcpy(entries[0].name, project);
		int lines = count_lines(path);
		snprintf(entries[0].info, sizeof entries[0].info, L("%d ZEILE%s", "%d LINE%s"), lines,
		         lines == 1 ? "" : L("N", "S"));
		nentries = 1;
	}
	if (sel >= nentries) sel = nentries ? nentries - 1 : 0;
	if (sel < 0) sel = 0;
}

static void keep_visible(void) {
	if (sel < top) top = sel;
	if (sel >= top + ROWS) top = sel - ROWS + 1;
	if (top < 0) top = 0;
}

static void select_entry(int i) {
	if (i < 0 || i >= nentries || i == sel) return;
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

void browser_open(void) {
	level = 0;
	sel = top = 0;
	confirm = -1;
	pressed = -2;            /* the tap that opened the browser is still down */
	message[0] = 0;
	/* start on the program that is loaded */
	read_list();
	const char *cur = basic_program_name();
	for (int i = 0; i < nentries; i++) {
		char base[16];
		strcpy(base, entries[i].name);
		char *dot = strchr(base, '.');
		if (dot) *dot = 0;
		if (cur[0] && !strcmp(base, cur)) { sel = i; break; }
	}
	keep_visible();
	draw_all();
	update_preview();
}

void browser_refresh(void) {
	read_list();
	keep_visible();
	confirm = -1;
	draw_all();
	update_preview();
}

void browser_set_message(const char *m) { set_message(m); }

const char *browser_target(void) { return target; }
const char *browser_target_file(void) { return targetFile; }
const char *browser_preview_title(void) { return previewTitle; }
int browser_preview_count(void) { return npreview; }
const char *browser_preview_line(int i) { return (i >= 0 && i < npreview) ? preview[i] : ""; }

static void enter_project(void) {
	if (!nentries) return;
	strcpy(project, entries[sel].name);
	projectIsFolder = is_folder(project);
	level = 1;
	sel = top = 0;
	confirm = -1;
	message[0] = 0;
	read_list();
	draw_all();
	update_preview();
}

static void leave_project(void) {
	char was[16];
	strcpy(was, project);
	level = 0;
	confirm = -1;
	message[0] = 0;
	read_list();
	sel = 0;
	for (int i = 0; i < nentries; i++) if (!strcmp(entries[i].name, was)) sel = i;
	top = 0;
	keep_visible();
	draw_all();
	update_preview();
}

/* button pressed; returns an action for main.c */
static int press(int b) {
	switch (b) {
	case B_UP:   select_entry(sel - 1); return BR_NONE;
	case B_DOWN: select_entry(sel + 1); return BR_NONE;
	case B_OPEN: enter_project(); return BR_NONE;
	case B_CLOSE:
		if (level) { leave_project(); return BR_NONE; }
		return BR_EXIT;
	case B_LOAD:
		if (!nentries) return BR_NONE;
		if (basic_modified() && confirm != B_LOAD) {
			confirm = B_LOAD;
			set_message(L("PROGRAMM GE\x8eNDERT - ERSETZEN?", "REPLACE THE UNSAVED PROGRAM?"));
			draw_buttons();
			return BR_NONE;
		}
		confirm = -1;
		strcpy(target, level ? project : entries[sel].name);
		targetFile[0] = 0;
		if (level && projectIsFolder) strcpy(targetFile, entries[sel].name);
		return BR_LOAD;
	case B_DEL:
		if (!nentries || (level == 1 && sel == 0)) return BR_NONE;
		if (confirm != B_DEL) {
			confirm = B_DEL;
			char m[40];
			snprintf(m, sizeof m, L("%s L\x99SCHEN?", "DELETE %s?"), entries[sel].name);
			set_message(m);
			draw_buttons();
			return BR_NONE;
		}
		confirm = -1;
		if (level == 0) {
			strcpy(target, entries[sel].name);
			char *dot = strchr(target, '.');
			if (dot && !strcasecmp(dot, ".PHP")) { /* keep: DELETE "X.PHP" removes the single file */ }
			return BR_DELETE;
		}
		if (!projectIsFolder) return BR_NONE;
		{
			char path[200];
			path_of(path, sizeof path, project, entries[sel].name);
			if (remove(path) != 0) set_message(L("FEHLER BEIM L\x99SCHEN", "COULD NOT DELETE"));
			else {
				char m[40];
				snprintf(m, sizeof m, L("%s GEL\x99SCHT", "%s DELETED"), entries[sel].name);
				read_list();
				keep_visible();
				draw_all();
				update_preview();
				set_message(m);
			}
		}
		return BR_NONE;
	}
	return BR_NONE;
}

static int hit_button(int px, int py) {
	if (py < BTN_Y || py >= BTN_Y + BTN_H) return -1;
	for (int b = 0; b < NBTN; b++) {
		const BtnPos *p = &btnPos[level][b];
		if (p->w && px >= p->x && px < p->x + p->w) return b;
	}
	return -1;
}

static int lastTap = -100, lastTapRow = -1, frame;

int browser_poll(bool touching, int px, int py, u32 down, u32 rep) {
	frame++;
	int result = BR_NONE;
	if (rep & KEY_UP) select_entry(sel - 1);
	if (rep & KEY_DOWN) select_entry(sel + 1);
	if (rep & KEY_LEFT) select_entry(sel - ROWS);
	if (rep & KEY_RIGHT) select_entry(sel + ROWS < nentries ? sel + ROWS : nentries - 1);
	if (down & KEY_A) result = level ? press(B_LOAD) : (nentries ? (enter_project(), BR_NONE) : BR_NONE);
	if (down & KEY_B) result = press(B_CLOSE);
	if (down & KEY_START) result = press(B_LOAD);

	if (touching) {
		if (pressed == -1) {
			int b = hit_button(px, py);
			if (b >= 0) {
				pressed = b;
				draw_btn(b);
			} else if (py >= LIST_Y && py < LIST_Y + ROWS * ROW_H) {
				int i = top + (py - LIST_Y) / ROW_H;
				if (i < nentries) {
					/* tap selects, a second tap on the same row opens / loads */
					if (i == sel && lastTapRow == i && frame - lastTap < 40)
						result = level ? press(B_LOAD) : (enter_project(), BR_NONE);
					else select_entry(i);
					lastTap = frame;
					lastTapRow = i;
				}
				pressed = -2;
			} else if (px < 22 && py < 22) {
				pressed = -2;
				result = BR_EXIT;
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
