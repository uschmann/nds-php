/*
 * basic.h - interpreter API (historic basic_* names; implemented by php.c).
 *
 * The interpreter is step driven: the frontend runs REPL lines with
 * basic_enter() and then repeatedly calls basic_run_slice() while
 * basic_state() == BASIC_RUNNING. When a program executes INPUT the state
 * becomes BASIC_INPUT and the frontend answers with basic_input().
 */
#ifndef BASIC_H
#define BASIC_H

typedef struct {
	void (*putch)(char c);          /* write one char, '\n' = newline   */
	void (*cls)(void);              /* clear output screen              */
	int  (*inkey)(void);            /* next pending key or 0 (optional) */
	void (*locate)(int col, int row); /* move cursor (optional)          */
	void (*color)(int c);           /* set text colour 0-15 (optional)  */
	int width;                      /* output width in characters       */
	void (*message)(const char *m); /* SAVE/LOAD status text (optional) */
	/* graphics (optional): colours 0-15, -1 = empty / transparent */
	void (*pset)(int x, int y, int c);
	int  (*pget)(int x, int y);
	void (*gcls)(void);
	int gw, gh;                     /* graphics size in pixels          */
	/* sound (optional): play slot 0-9, -1 = stop; busy = still playing */
	void (*sound)(int slot);
	int  (*sound_busy)(void);
	void (*exit)(void);             /* EXIT: back to title screen (opt.) */
	int  (*joy)(void);              /* JOY: held buttons as bit mask    */
	/* program output on the bottom (touch) screen: mode 1 on, 0 off,
	 * -1 toggle; returns the new state (optional) */
	int  (*swap)(int mode);
	/* stylus on the touch screen? then x 0-255, y 0-191 (optional) */
	int  (*touch)(int *x, int *y);
	/* effect voice (optional): queue a sweep hz0->hz1, volume vol0->vol1
	 * (0-127) over frames; wave = SND_SQ12..SND_NOISE; replace = clear the
	 * queue first. tone_queued = segments still waiting or playing. */
	int  (*tone)(int wave, int hz0, int hz1, int frames, int vol0, int vol1, int replace);
	int  (*tone_queued)(void);
} BasicIO;

typedef enum {
	BASIC_IDLE = 0,
	BASIC_RUNNING,
	BASIC_INPUT,
	BASIC_PAUSED             /* debugger: stopped at a breakpoint or step */
} BasicState;

typedef enum {
	BASIC_ENTER_EMPTY = 0,   /* nothing entered                       */
	BASIC_ENTER_EXEC = 3,    /* REPL line started                     */
	BASIC_ENTER_ERROR        /* interpreter busy                      */
} BasicEnterResult;

void        basic_init(const BasicIO *io);
/* run one REPL line (PHP code or a command such as RUN, SAVE "X"); lineNum unused */
int         basic_enter(const char *line, int *lineNum);
BasicState  basic_state(void);
int         basic_run_slice(int maxStatements);
void        basic_frame(void);           /* call once per video frame   */
void        basic_input(const char *line);
void        basic_break(void);
void        basic_seed(unsigned seed);
const char *basic_last_error(void);      /* "" when the last run was ok */
int         basic_error_line(void);      /* line of the last error/break or -1 */
int         basic_current_line(void);    /* line number executing or -1 */

/* sprites: BASIC_SPRITES slots of 8x8 pixels, colour 0-15 or -1 = transparent */
#define BASIC_SPRITES 256
int         basic_sprite_get(int slot, int x, int y);
void        basic_sprite_set(int slot, int x, int y, int c);

/* hardware sprite objects shown with SPRITE o,n,x,y[,s] */
#define BASIC_OBJECTS   64
#define BASIC_OBJ_SCALE 4
typedef struct {
	unsigned char visible;
	unsigned char slot;          /* sprite slot (graphic)                 */
	unsigned char scale;         /* 1-BASIC_OBJ_SCALE                      */
	unsigned char dirty;         /* graphic must be re-rendered            */
	int x, y;                    /* top left in pixels                     */
} BasicObject;
BasicObject *basic_object(int o);

/* sounds: BASIC_SOUNDS slots, each a sequence of up to BASIC_SOUND_LEN notes */
#define BASIC_SOUNDS    10
#define BASIC_SOUND_LEN 64
enum { SND_SQ12 = 0, SND_SQ25, SND_SQ50, SND_NOISE, SND_INSTRUMENTS };
typedef struct {
	unsigned char inst;                    /* SND_SQ12 .. SND_NOISE          */
	unsigned char tempo;                   /* beats per minute (quarter)     */
	unsigned char count;                   /* number of notes                */
	unsigned char note[BASIC_SOUND_LEN];   /* MIDI note 24-108, 0 = rest     */
	unsigned char len[BASIC_SOUND_LEN];    /* length in 16th notes, 1-16     */
} BasicSound;
BasicSound *basic_sound(int slot);
void        basic_sound_clear(int slot);

/* storage for SAVE/LOAD/DIR/DELETE: directory without trailing '/', NULL = none */
void        basic_set_storage(const char *dir);
int         basic_has_storage(void);
const char *basic_storage_dir(void);     /* "" without storage */
const char *basic_program_name(void);    /* name of the last LOAD/SAVE or "" */
int         basic_modified(void);        /* program changed since LOAD/SAVE */
/* sorted names of saved programs: "NAME" = project folder, "NAME.PHP" = old
 * single file; -1 = no storage */
#define BASIC_NAME_MAX  24      /* program and file names (without .php) */
#define BASIC_NAME_BUF  32      /* buffer for a name + ".php" + 0        */
#define BASIC_MAX_FILES 64
int         basic_list_files(char names[][BASIC_NAME_BUF], int max);
/* versions of the current program (COMMIT / LOG / CHECKOUT), newest first;
 * -1 when the program has not been saved yet */
#define BASIC_VC_MSG 40
typedef struct { int num; char date[17]; char msg[BASIC_VC_MSG + 1]; } BasicVersion;
int         basic_vc_list(BasicVersion *out, int max);
int         basic_vc_base(void);        /* checked out version, 0 = latest */
/* DIFF of version v against the program as text ("+3 code" ...); 0 on error */
int         basic_vc_diff(int v, char *out, int size);

#define BASIC_LINE_MAX  255     /* characters per program line          */
#define BASIC_MAX_LINES 1000
#define BASIC_PROJECT_FILES 16  /* files per program, 0 = main.php      */

/* files of the program: main.php (index 0) is the entry point, others are
 * pulled in with require/include. The line functions below work on the
 * selected file. */
int         basic_file_count(void);
const char *basic_file_name(int index);
int         basic_current_file(void);
void        basic_select_file(int index);
int         basic_files_version(void);   /* changes when files are added/removed/renamed/loaded */
int         basic_edit_request(void);    /* file the REPL opened with EDIT, or -1 (clears it) */
int         basic_error_file(void);      /* file of the last error/break */

/* program lines for the editor; the number of line i is i + 1 and lines
 * can only be changed while the interpreter is idle (functions return 0
 * otherwise or when the program is full) */
int         basic_insert_line(int index, const char *text);
int         basic_set_line(int index, const char *text);
void        basic_delete_line(int index);
int         basic_line_count(void);
int         basic_line_number(int index);
const char *basic_line_text(int index);
int         basic_find_index(int lineNum); /* first index with num >= lineNum */

/* debugger: breakpoints are line indexes (0 = first line) of a file. A
 * program running with breakpoints (or a step request) stops in
 * BASIC_PAUSED before the first instruction of such a line. */
enum { BASIC_DBG_CONTINUE, BASIC_DBG_STEP, BASIC_DBG_OVER, BASIC_DBG_OUT };
void        basic_toggle_breakpoint(int file, int index);
int         basic_breakpoint(int file, int index);
void        basic_clear_breakpoints(void);
void        basic_debug_step_next(void);      /* next RUN stops at the first line */
void        basic_debug_resume(int mode);     /* while paused: BASIC_DBG_*      */
int         basic_debug_location(int *file, int *index); /* paused at? 0 = no */
const char *basic_debug_function(void);       /* "{main}", "foo", "Cls::m"      */
typedef struct { char name[24]; char value[64]; unsigned char global; } BasicVar;
int         basic_debug_vars(BasicVar *out, int max);  /* locals, then globals */
/* lines of any file (the editor functions above use the selected one) */
int         basic_file_line_count(int file);
const char *basic_file_line_text(int file, int index);

/* names for the editor's autocompletion */
int         basic_builtin_count(void);
const char *basic_builtin_name(int i);
void        basic_builtin_args(int i, int *minArgs, int *maxArgs);   /* max -1 = any */
int         basic_constant_count(void);
const char *basic_constant_name(int i);

#endif
