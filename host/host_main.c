/* host_main.c - run the PHP interpreter on a PC terminal (for testing).
 * Build: make -C host   Usage: BASIC_DIR=/tmp/x ./host/basic < program.txt
 * Lines "NN code" are appended to the program, all others run as REPL lines.
 * Debugger: "#BP n" toggles a breakpoint on program line n (main.php),
 * "#DEBUG" makes the next RUN stop at the first line. While paused, input
 * lines are debugger commands: s(tep), o(ver), u (out), c(ontinue), q(uit).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/basic.h"

static void h_putch(char c) { putchar(c); }
static void h_cls(void) { printf("<CLS>\n"); }

/* 256x192 test framebuffer, -1 = empty */
static signed char fb[192][256];
static void h_pset(int x, int y, int c) { fb[y][x] = (signed char)c; }
static int  h_pget(int x, int y) { return fb[y][x]; }
static void h_gcls(void) { memset(fb, -1, sizeof fb); }

static int soundFrames;   /* fake playback: busy for a few frames */
static void h_sound(int n) {
	printf("<SOUND %d>\n", n);
	BasicSound *s = basic_sound(n);
	soundFrames = (n >= 0 && s) ? s->count * 2 : 0;
}
static int h_sound_busy(void) { return soundFrames > 0; }

int main(void) {
	static const BasicIO io = { h_putch, h_cls, NULL, NULL, NULL, 32, NULL,
	                                    h_pset, h_pget, h_gcls, 256, 192,
	                                    h_sound, h_sound_busy, NULL, NULL, NULL, NULL, NULL, NULL };
	char line[512];
	basic_init(&io);
	h_gcls();
	basic_set_storage(getenv("BASIC_DIR"));
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		/* "NN code" appends a program line (stands in for the editor) */
		char *sp = line;
		while (*sp >= '0' && *sp <= '9') sp++;
		if (sp != line && (*sp == ' ' || !*sp)) {
			if (*sp == ' ') sp++;
			if (!basic_insert_line(basic_line_count(), sp)) printf("?LINE ERROR\n");
			continue;
		}
		if (!strncmp(line, "#BP ", 4)) { basic_toggle_breakpoint(0, atoi(line + 4) - 1); continue; }
		if (!strcmp(line, "#DEBUG")) { basic_debug_step_next(); continue; }
		int r = basic_enter(line, NULL);
		if (r == BASIC_ENTER_ERROR) printf("?BUSY\n");
		while (basic_state() != BASIC_IDLE) {
			if (basic_state() == BASIC_PAUSED) {
				int f, i;
				BasicVar v[32];
				basic_debug_location(&f, &i);
				printf("[PAUSED line %d in %s:", i + 1, basic_debug_function());
				int nv = basic_debug_vars(v, 32);
				for (int k = 0; k < nv; k++) printf(" %s$%s=%s", v[k].global ? "g:" : "", v[k].name, v[k].value);
				printf("]\n");
				if (!fgets(line, sizeof line, stdin)) { basic_break(); break; }
				switch (line[0]) {
				case 's': basic_debug_resume(BASIC_DBG_STEP); break;
				case 'o': basic_debug_resume(BASIC_DBG_OVER); break;
				case 'u': basic_debug_resume(BASIC_DBG_OUT); break;
				case 'q': basic_break(); break;
				default: basic_debug_resume(BASIC_DBG_CONTINUE);
				}
				continue;
			}
			if (basic_state() == BASIC_INPUT) {
				if (!fgets(line, sizeof line, stdin)) { basic_break(); break; }
				line[strcspn(line, "\r\n")] = 0;
				printf("%s\n", line);
				basic_input(line);
			}
			basic_run_slice(1000);
			basic_frame();
			if (soundFrames > 0 && --soundFrames == 0) printf("<SOUND END>\n");
		}
	}
	return 0;
}
