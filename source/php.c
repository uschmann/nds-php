/*
 * php.c - small PHP dialect for the DS (implements the interpreter API in
 * basic.h, which keeps its historic basic_* names).
 *
 * RUN joins the program lines and compiles the text to bytecode, which a small stack VM executes in slices,
 * so the frontend stays responsive and START can break any loop.
 *
 * Language: $variables, int/float/string/bool/null, arrays (ordered maps with
 * copy-on-write), echo/print, if/elseif/else, while, do-while, for, foreach,
 * switch, break/continue [n], functions with default parameters, global,
 * return, exit/die, string interpolation ("$x", "{$a['k']}"), casts,
 * ++/--, compound assignment, ?:, ??, and a set of built-in functions plus
 * DS functions for text, graphics, sprites, sound and input.
 *
 * The program is a list of lines edited by the frontend (basic_insert_line
 * and friends); a line's number is its position. basic_enter() runs a REPL
 * line: PHP code or one of the commands RUN NEW CLR LIST SAVE LOAD DIR
 * DELETE EXIT, which are not PHP.
 */
#include "basic.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define MAX_LINES    BASIC_MAX_LINES
#define LINE_MAX_LEN BASIC_LINE_MAX
#define CODE_MAX     32768
#define CONST_MAX    4096
#define STACK_MAX    2048
#define FRAMES_MAX   200
#define GLOBALS_MAX  512
#define FUNCS_MAX    256
#define LOCALS_MAX   64
#define PARAMS_MAX   8
#define CAPS_MAX     16          /* captured variables of a closure */
#define NAME_MAX_LEN 31
#define LOOPS_MAX    16
#define JUMPS_MAX    64
#define STR_LIMIT    (64 * 1024)

/* ------------------------------------------------------------------ */
/* program lines                                                       */
/* ------------------------------------------------------------------ */

typedef struct { int num; char *text; } Line;

static const BasicIO *io;
static BasicState state = BASIC_IDLE;
/* a program is a project of several files; main.php (index 0) is the
 * entry point. lines/nlines always refer to the file selected with
 * basic_select_file(). */
#define MAX_SRC     BASIC_PROJECT_FILES
static Line  fileLines[MAX_SRC][MAX_LINES];
static int   fileCount[MAX_SRC];
static char  fileNames[MAX_SRC][16];
static int   nsrc = 1, curFile;
static int   filesVersion, editRequest = -1;
static int   modified;                  /* program changed since LOAD/SAVE */
#define lines  (fileLines[curFile])
#define nlines (fileCount[curFile])

static char  lastError[128];
static int   lastErrorLine = -1, lastErrorFile = 0;
static char  storageDir[128];
static char  lastFile[16];
static int   col;                       /* output column                 */
static unsigned rndState = 12345;
static int   waitFrames, waitSound, waitTone;
static int   gcolor = 1;

static signed char sprites[BASIC_SPRITES][64];
static BasicObject objects[BASIC_OBJECTS];
static BasicSound  sounds[BASIC_SOUNDS];

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

static jmp_buf errjmp;
static char errText[80];
static int  errIsParse;
static int  errLine;

static void fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ------------------------------------------------------------------ */
/* values                                                              */
/* ------------------------------------------------------------------ */

enum { T_NULL = 0, T_BOOL, T_INT, T_FLOAT, T_STR, T_ARR, T_APPEND, T_OBJ, T_PNAME, T_CLO };
/* T_PNAME: property name on the stack (path element of $a->b)
 * T_CLO: closure (function or fn) with its captured variables */

typedef struct Str { int rc, len; char s[1]; } Str;
typedef struct Arr Arr;
typedef struct Obj Obj;
typedef struct Clo Clo;
typedef struct Value {
	unsigned char t;
	union { int i; float f; Str *s; Arr *a; Obj *o; Clo *c; } u;
} Value;
typedef struct { Value key, val; } Entry;
struct Arr { int rc, n, cap, next; Entry *e; };

static inline Value mk_null(void) { Value v; v.t = T_NULL; v.u.i = 0; return v; }
static inline Value mk_bool(int b) { Value v; v.t = T_BOOL; v.u.i = b != 0; return v; }
static inline Value mk_int(int i) { Value v; v.t = T_INT; v.u.i = i; return v; }
static inline Value mk_float(float f) { Value v; v.t = T_FLOAT; v.u.f = f; return v; }

static Str *str_alloc(int len) {
	if (len > STR_LIMIT) fail("String too long");
	Str *s = malloc(sizeof(Str) + len);
	if (!s) fail("Out of memory");
	s->rc = 1;
	s->len = len;
	s->s[len] = 0;
	return s;
}

static Value mk_str(const char *p, int len) {
	Value v;
	v.t = T_STR;
	v.u.s = str_alloc(len);
	memcpy(v.u.s->s, p, len);
	return v;
}
static Value mk_cstr(const char *p) { return mk_str(p, (int)strlen(p)); }

static void arr_free(Arr *a);
static void obj_free(Obj *o);

/* objects are handles: assignment shares the object */
struct Obj { int rc, cls, id, nprops; Arr *dyn; Value props[1]; };

/* closures share like objects; use[] are the captured values, thisv and
 * cls/scls the class context they were created in */
struct Clo { int rc, func, id, nuse; short cls, scls; Value thisv; Value use[1]; };
static void clo_free(Clo *c);

static inline void retain(Value v) {
	if (v.t == T_STR || v.t == T_PNAME) v.u.s->rc++;
	else if (v.t == T_ARR) v.u.a->rc++;
	else if (v.t == T_OBJ) v.u.o->rc++;
	else if (v.t == T_CLO) v.u.c->rc++;
}

static void release(Value v) {
	if (v.t == T_STR || v.t == T_PNAME) { if (--v.u.s->rc == 0) free(v.u.s); }
	else if (v.t == T_ARR) { if (--v.u.a->rc == 0) arr_free(v.u.a); }
	else if (v.t == T_OBJ) { if (--v.u.o->rc == 0) obj_free(v.u.o); }
	else if (v.t == T_CLO) { if (--v.u.c->rc == 0) clo_free(v.u.c); }
}

static void clo_free(Clo *c) {
	for (int i = 0; i < c->nuse; i++) release(c->use[i]);
	release(c->thisv);
	free(c);
}

/* --- arrays: ordered maps with int or string keys --- */

static Arr *arr_alloc(void) {
	Arr *a = calloc(1, sizeof(Arr));
	if (!a) fail("Out of memory");
	a->rc = 1;
	return a;
}

static Value mk_arr(void) { Value v; v.t = T_ARR; v.u.a = arr_alloc(); return v; }

static void arr_free(Arr *a) {
	for (int i = 0; i < a->n; i++) { release(a->e[i].key); release(a->e[i].val); }
	free(a->e);
	free(a);
}

static void arr_grow(Arr *a) {
	if (a->n < a->cap) return;
	int nc = a->cap ? a->cap * 2 : 4;
	Entry *e = realloc(a->e, nc * sizeof(Entry));
	if (!e) fail("Out of memory");
	a->e = e;
	a->cap = nc;
}

/* "12" and "-3" are integer keys, "012" or "1.5" are not */
static int str_int_key(const Str *s, int *out) {
	const char *p = s->s;
	int len = s->len, neg = 0;
	if (len == 0 || len > 10) return 0;
	if (*p == '-') { neg = 1; p++; len--; if (!len) return 0; }
	if (*p == '0' && (len > 1 || neg)) return 0;
	long long v = 0;
	for (int i = 0; i < len; i++) {
		if (!isdigit((unsigned char)p[i])) return 0;
		v = v * 10 + (p[i] - '0');
	}
	if (neg) v = -v;
	if (v < -2147483647LL - 1 || v > 2147483647LL) return 0;
	*out = (int)v;
	return 1;
}

/* normalized key with its own reference */
static Value norm_key(Value k) {
	int i;
	switch (k.t) {
	case T_INT:   return k;
	case T_BOOL:  return mk_int(k.u.i);
	case T_FLOAT: return mk_int((int)k.u.f);
	case T_NULL:  return mk_str("", 0);
	case T_STR:
		if (str_int_key(k.u.s, &i)) return mk_int(i);
		retain(k);
		return k;
	default: fail("Illegal offset type");
	}
}

static int key_eq(Value a, Value b) {
	if (a.t != b.t) return 0;
	if (a.t == T_INT) return a.u.i == b.u.i;
	return a.u.s->len == b.u.s->len && !memcmp(a.u.s->s, b.u.s->s, a.u.s->len);
}

static int arr_find(const Arr *a, Value nk) {
	if (nk.t == T_INT && nk.u.i >= 0 && nk.u.i < a->n &&
	    a->e[nk.u.i].key.t == T_INT && a->e[nk.u.i].key.u.i == nk.u.i) return nk.u.i;
	for (int i = 0; i < a->n; i++) if (key_eq(a->e[i].key, nk)) return i;
	return -1;
}

/* pointer to the value slot for key k (borrowed); creates a null entry */
static Value *arr_slot(Arr *a, Value k, int create) {
	Value nk = norm_key(k);
	int i = arr_find(a, nk);
	if (i >= 0) { release(nk); return &a->e[i].val; }
	if (!create) { release(nk); return NULL; }
	arr_grow(a);
	a->e[a->n].key = nk;
	a->e[a->n].val = mk_null();
	if (nk.t == T_INT && nk.u.i >= a->next) a->next = nk.u.i + 1;
	return &a->e[a->n++].val;
}

static Value *arr_append_slot(Arr *a) { return arr_slot(a, mk_int(a->next), 1); }

static void arr_push(Arr *a, Value v) { *arr_append_slot(a) = v; }

static void arr_set(Arr *a, Value k, Value v) {
	Value *p = arr_slot(a, k, 1);
	release(*p);
	*p = v;
}

static void arr_remove_at(Arr *a, int i) {
	release(a->e[i].key);
	release(a->e[i].val);
	memmove(&a->e[i], &a->e[i + 1], (a->n - i - 1) * sizeof(Entry));
	a->n--;
}

static void arr_reindex(Arr *a) {
	int k = 0;
	for (int i = 0; i < a->n; i++)
		if (a->e[i].key.t == T_INT) a->e[i].key.u.i = k++;
	a->next = k;
}

static Arr *arr_clone(const Arr *a) {
	Arr *c = arr_alloc();
	if (a->n) {
		c->e = malloc(a->n * sizeof(Entry));
		if (!c->e) { free(c); fail("Out of memory"); }
		c->cap = a->n;
	}
	for (int i = 0; i < a->n; i++) {
		c->e[i] = a->e[i];
		retain(c->e[i].key);
		retain(c->e[i].val);
	}
	c->n = a->n;
	c->next = a->next;
	return c;
}

/* make the array in *v exclusively ours before writing to it */
static void separate(Value *v) {
	if (v->t == T_ARR && v->u.a->rc > 1) {
		Arr *c = arr_clone(v->u.a);
		v->u.a->rc--;
		v->u.a = c;
	}
}

/* ------------------------------------------------------------------ */
/* classes, interfaces, traits                                         */
/* ------------------------------------------------------------------ */

#define CLASSES_MAX 80          /* incl. the built-in exception classes */
#define CPROPS_MAX  24
#define CMETH_MAX   32
#define CCONST_MAX  16
#define CLIST_MAX   8
#define IFACES_MAX  16

enum { K_CLASS, K_INTERFACE, K_TRAIT };
enum { VIS_PUBLIC, VIS_PROTECTED, VIS_PRIVATE };

typedef struct { char name[NAME_MAX_LEN + 1]; Value val; unsigned char vis, isStatic; short decl; } CProp;
typedef struct { char name[NAME_MAX_LEN + 1]; int func; unsigned char vis, isStatic, isAbstract; short decl; } CMethod;
typedef struct { char name[NAME_MAX_LEN + 1]; Value val; } CConst;

typedef struct {
	char name[NAME_MAX_LEN + 1];
	int  kind, isAbstract, defined, line, firstUse;
	char parentName[NAME_MAX_LEN + 1];
	char ifaceNames[CLIST_MAX][NAME_MAX_LEN + 1];
	int  nifaces;
	char traitNames[CLIST_MAX][NAME_MAX_LEN + 1];
	int  ntraits;
	CProp   own[CPROPS_MAX];      /* declared properties; static ones hold their value */
	int     nown;
	CMethod ownM[CMETH_MAX];
	int     nownM;
	CConst  ownC[CCONST_MAX];
	int     nownC;
	/* filled in by link_class() */
	int linked, linking, parent;
	int ifaces[IFACES_MAX], nallIfaces;
	CProp   *props;               /* instance properties incl. inherited ones */
	int      nprops;
	CMethod *methods;             /* all callable methods incl. inherited / trait ones */
	int      nmethods;
	CConst  *consts;
	int      nconsts;
} Class;

static Class classes[CLASSES_MAX];
static int   nclasses, objIds;

static const char *kind_name(int k) { return k == K_INTERFACE ? "interface" : k == K_TRAIT ? "trait" : "class"; }

static int find_class(const char *name) {
	for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, name)) return i;
	return -1;
}

static void free_class(Class *k) {
	for (int i = 0; i < k->nown; i++) release(k->own[i].val);
	for (int i = 0; i < k->nownC; i++) release(k->ownC[i].val);
	for (int i = 0; i < k->nprops; i++) release(k->props[i].val);
	for (int i = 0; i < k->nconsts; i++) release(k->consts[i].val);
	free(k->props);
	free(k->methods);
	free(k->consts);
	memset(k, 0, sizeof *k);
}

static int is_a(int c, int target) {
	for (; c >= 0; c = classes[c].parent) {
		if (c == target) return 1;
		for (int i = 0; i < classes[c].nallIfaces; i++) if (classes[c].ifaces[i] == target) return 1;
	}
	return 0;
}

static CMethod *find_method(int c, const char *name) {
	Class *k = &classes[c];
	for (int i = 0; i < k->nmethods; i++) if (!strcasecmp(k->methods[i].name, name)) return &k->methods[i];
	return NULL;
}

static int find_prop(int c, const char *name) {
	Class *k = &classes[c];
	for (int i = 0; i < k->nprops; i++) if (!strcmp(k->props[i].name, name)) return i;
	return -1;
}

static CConst *find_cconst(int c, const char *name) {
	Class *k = &classes[c];
	for (int i = 0; i < k->nconsts; i++) if (!strcmp(k->consts[i].name, name)) return &k->consts[i];
	return NULL;
}

static void add_method(Class *k, const CMethod *m, int decl) {
	for (int i = 0; i < k->nmethods; i++)
		if (!strcasecmp(k->methods[i].name, m->name)) {
			/* an abstract declaration never replaces an implementation */
			if (m->isAbstract && !k->methods[i].isAbstract) return;
			k->methods[i] = *m;
			k->methods[i].decl = (short)decl;
			return;
		}
	if (k->nmethods >= CMETH_MAX * 2) fail("Too many methods in %s", k->name);
	k->methods[k->nmethods] = *m;
	k->methods[k->nmethods].decl = (short)decl;
	k->nmethods++;
}

static void add_prop(Class *k, const CProp *p, int decl) {
	for (int i = 0; i < k->nprops; i++)
		if (!strcmp(k->props[i].name, p->name)) {
			release(k->props[i].val);
			k->props[i] = *p;
			retain(p->val);
			k->props[i].decl = (short)decl;
			return;
		}
	if (k->nprops >= CPROPS_MAX * 2) fail("Too many properties in %s", k->name);
	k->props[k->nprops] = *p;
	retain(p->val);
	k->props[k->nprops].decl = (short)decl;
	k->nprops++;
}

static void add_cconst(Class *k, const CConst *c) {
	for (int i = 0; i < k->nconsts; i++)
		if (!strcmp(k->consts[i].name, c->name)) {
			release(k->consts[i].val);
			k->consts[i].val = c->val;
			retain(c->val);
			return;
		}
	if (k->nconsts >= CCONST_MAX * 2) fail("Too many constants in %s", k->name);
	k->consts[k->nconsts] = *c;
	retain(c->val);
	k->nconsts++;
}

static void add_iface(Class *k, int i) {
	for (int j = 0; j < k->nallIfaces; j++) if (k->ifaces[j] == i) return;
	if (k->nallIfaces >= IFACES_MAX) fail("Too many interfaces for %s", k->name);
	k->ifaces[k->nallIfaces++] = i;
}

static int errLineSet(int line);

static int resolve_named(const char *name, int kind, int user) {
	int i = find_class(name);
	if (i < 0 || !classes[i].defined) {
		errLineSet(classes[user].line);
		fail("%s %s not found", kind == K_INTERFACE ? "Interface" : kind == K_TRAIT ? "Trait" : "Class", name);
	}
	if (classes[i].kind != kind) {
		errLineSet(classes[user].line);
		fail("%s cannot use %s %s here", classes[user].name, kind_name(classes[i].kind), classes[i].name);
	}
	return i;
}

/* build the flattened tables: parent first, then interfaces, traits and the
 * class's own members; check that concrete classes implement everything */
static void link_class(int c) {
	Class *k = &classes[c];
	if (k->linked) return;
	if (k->linking) { errLineSet(k->line); fail("%s inherits from itself", k->name); }
	k->linking = 1;
	k->parent = -1;
	k->props = calloc(CPROPS_MAX * 2, sizeof(CProp));
	k->methods = calloc(CMETH_MAX * 2, sizeof(CMethod));
	k->consts = calloc(CCONST_MAX * 2, sizeof(CConst));
	if (!k->props || !k->methods || !k->consts) fail("Out of memory");
	if (k->parentName[0]) {
		int p = resolve_named(k->parentName, K_CLASS, c);
		link_class(p);
		k->parent = p;
		Class *pk = &classes[p];
		for (int i = 0; i < pk->nprops; i++) add_prop(k, &pk->props[i], pk->props[i].decl);
		for (int i = 0; i < pk->nmethods; i++) add_method(k, &pk->methods[i], pk->methods[i].decl);
		for (int i = 0; i < pk->nconsts; i++) add_cconst(k, &pk->consts[i]);
		for (int i = 0; i < pk->nallIfaces; i++) add_iface(k, pk->ifaces[i]);
	}
	for (int n = 0; n < k->nifaces; n++) {
		int f = resolve_named(k->ifaceNames[n], K_INTERFACE, c);
		link_class(f);
		Class *fk = &classes[f];
		add_iface(k, f);
		for (int i = 0; i < fk->nallIfaces; i++) add_iface(k, fk->ifaces[i]);
		for (int i = 0; i < fk->nconsts; i++) add_cconst(k, &fk->consts[i]);
		for (int i = 0; i < fk->nmethods; i++) add_method(k, &fk->methods[i], fk->methods[i].decl);
	}
	for (int n = 0; n < k->ntraits; n++) {
		int t = resolve_named(k->traitNames[n], K_TRAIT, c);
		link_class(t);
		Class *tk = &classes[t];
		for (int i = 0; i < tk->nprops; i++) add_prop(k, &tk->props[i], c);
		for (int i = 0; i < tk->nmethods; i++) add_method(k, &tk->methods[i], c);
		/* every class using a trait gets its own static properties */
		for (int i = 0; i < tk->nown; i++) {
			if (!tk->own[i].isStatic) continue;
			int dup = 0;
			for (int j = 0; j < k->nown; j++) if (!strcmp(k->own[j].name, tk->own[i].name)) dup = 1;
			if (dup) continue;
			if (k->nown >= CPROPS_MAX) fail("Too many properties in %s", k->name);
			k->own[k->nown] = tk->own[i];
			retain(tk->own[i].val);
			k->nown++;
		}
	}
	for (int i = 0; i < k->nown; i++) if (!k->own[i].isStatic) add_prop(k, &k->own[i], c);
	for (int i = 0; i < k->nownM; i++) add_method(k, &k->ownM[i], c);
	for (int i = 0; i < k->nownC; i++) add_cconst(k, &k->ownC[i]);
	if (k->kind == K_CLASS && !k->isAbstract)
		for (int i = 0; i < k->nmethods; i++)
			if (k->methods[i].isAbstract) {
				errLineSet(k->line);
				fail("Class %s must implement %s()", k->name, k->methods[i].name);
			}
	/* the tables were allocated for the worst case: shrink them */
	CProp *np = realloc(k->props, (k->nprops ? k->nprops : 1) * sizeof(CProp));
	CMethod *nm = realloc(k->methods, (k->nmethods ? k->nmethods : 1) * sizeof(CMethod));
	CConst *nc = realloc(k->consts, (k->nconsts ? k->nconsts : 1) * sizeof(CConst));
	if (np) k->props = np;
	if (nm) k->methods = nm;
	if (nc) k->consts = nc;
	k->linking = 0;
	k->linked = 1;
}

static Value new_object(int c) {
	Class *k = &classes[c];
	Obj *o = malloc(sizeof(Obj) + sizeof(Value) * (k->nprops ? k->nprops - 1 : 0));
	if (!o) fail("Out of memory");
	o->rc = 1;
	o->cls = c;
	o->id = ++objIds;
	o->nprops = k->nprops;
	o->dyn = NULL;
	for (int i = 0; i < k->nprops; i++) { o->props[i] = k->props[i].val; retain(o->props[i]); }
	Value v;
	v.t = T_OBJ;
	v.u.o = o;
	return v;
}

static void obj_free(Obj *o) {
	for (int i = 0; i < o->nprops; i++) release(o->props[i]);
	if (o->dyn) arr_free(o->dyn);
	free(o);
}

static Value obj_to_str(Value v);       /* __toString (needs the VM) */

/* ------------------------------------------------------------------ */
/* conversions                                                         */
/* ------------------------------------------------------------------ */

static int fmt_float(char *b, float f) {
	if (isnan(f)) return sprintf(b, "NAN");
	if (isinf(f)) return sprintf(b, f < 0 ? "-INF" : "INF");
	if (f == floorf(f) && fabsf(f) < 1e15f) return sprintf(b, "%.0f", (double)f);
	int n = sprintf(b, "%.7G", (double)f);
	return n;
}

/* string form of a value; pointer stays valid until 4 more calls */
static const char *sval(Value v, int *len) {
	static char bufs[4][40];
	static int k;
	char *b;
	int l;
	if (!len) len = &l;
	switch (v.t) {
	case T_STR:   *len = v.u.s->len; return v.u.s->s;
	case T_INT:   b = bufs[k++ & 3]; *len = sprintf(b, "%d", v.u.i); return b;
	case T_FLOAT: b = bufs[k++ & 3]; *len = fmt_float(b, v.u.f); return b;
	case T_BOOL:  *len = v.u.i ? 1 : 0; return v.u.i ? "1" : "";
	case T_ARR:   *len = 5; return "Array";
	case T_CLO:   fail("Object of class Closure could not be converted to string");
	case T_OBJ: {
		/* __toString(); the string is kept until 4 more conversions */
		static Value hold[4];
		static int h;
		h = (h + 1) & 3;
		Value r = obj_to_str(v);
		release(hold[h]);
		hold[h] = r;
		*len = r.u.s->len;
		return r.u.s->s;
	}
	default:      *len = 0; return "";
	}
}

static Value to_str_val(Value v) {
	if (v.t == T_STR) { retain(v); return v; }
	int len;
	const char *s = sval(v, &len);
	return mk_str(s, len);
}

/* parse a number at the start of s; returns chars used (0 = none) */
static int parse_num(const char *s, int len, Value *out) {
	int i = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
	int start = i;
	if (i < len && (s[i] == '+' || s[i] == '-')) i++;
	int digits = 0, isFloat = 0;
	while (i < len && isdigit((unsigned char)s[i])) { i++; digits++; }
	if (i < len && s[i] == '.') {
		int j = i + 1, fd = 0;
		while (j < len && isdigit((unsigned char)s[j])) { j++; fd++; }
		if (digits || fd) { i = j; digits += fd; isFloat = 1; }
	}
	if (!digits) return 0;
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		int j = i + 1;
		if (j < len && (s[j] == '+' || s[j] == '-')) j++;
		if (j < len && isdigit((unsigned char)s[j])) {
			while (j < len && isdigit((unsigned char)s[j])) j++;
			i = j;
			isFloat = 1;
		}
	}
	char tmp[48];
	int n = i - start < 47 ? i - start : 47;
	memcpy(tmp, s + start, n);
	tmp[n] = 0;
	if (!isFloat) {
		long long v = strtoll(tmp, NULL, 10);
		if (v >= -2147483647LL - 1 && v <= 2147483647LL) *out = mk_int((int)v);
		else *out = mk_float((float)strtod(tmp, NULL));
	} else *out = mk_float((float)strtod(tmp, NULL));
	return i;
}

/* whole string is numeric (leading/trailing whitespace allowed) */
static int numeric_str(const Str *s, Value *out) {
	int n = parse_num(s->s, s->len, out);
	if (!n) return 0;
	while (n < s->len && (s->s[n] == ' ' || s->s[n] == '\t' || s->s[n] == '\n')) n++;
	return n == s->len;
}

static Value to_num(Value v) {
	Value r;
	switch (v.t) {
	case T_INT: case T_FLOAT: return v;
	case T_BOOL: return mk_int(v.u.i);
	case T_STR:
		if (parse_num(v.u.s->s, v.u.s->len, &r)) return r;
		return mk_int(0);
	case T_ARR: fail("Unsupported operand types: array");
	case T_OBJ: fail("Unsupported operand types: %s", classes[v.u.o->cls].name);
	case T_CLO: fail("Unsupported operand types: Closure");
	default: return mk_int(0);
	}
}

static float to_float(Value v) {
	v = to_num(v);
	return v.t == T_INT ? (float)v.u.i : v.u.f;
}

static int float_to_int(float f) {
	if (isnan(f) || isinf(f)) return 0;
	if (f >= 2147483647.0f) return 2147483647;
	if (f <= -2147483648.0f) return -2147483647 - 1;
	return (int)f;
}

static int to_int(Value v) {
	if (v.t == T_ARR) return v.u.a->n ? 1 : 0;
	v = to_num(v);
	return v.t == T_INT ? v.u.i : float_to_int(v.u.f);
}

static int to_bool(Value v) {
	switch (v.t) {
	case T_BOOL: case T_INT: return v.u.i != 0;
	case T_FLOAT: return v.u.f != 0;
	case T_STR: return !(v.u.s->len == 0 || (v.u.s->len == 1 && v.u.s->s[0] == '0'));
	case T_ARR: return v.u.a->n != 0;
	case T_OBJ: case T_CLO: return 1;
	default: return 0;
	}
}

/* ------------------------------------------------------------------ */
/* comparison                                                          */
/* ------------------------------------------------------------------ */

static int sign(float d) { return d < 0 ? -1 : d > 0 ? 1 : 0; }

static int num_cmp(Value a, Value b) {
	if (a.t == T_INT && b.t == T_INT) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i;
	return sign(to_float(a) - to_float(b));
}

static int str_cmp(const char *a, int al, const char *b, int bl) {
	int n = al < bl ? al : bl;
	int c = memcmp(a, b, n);
	if (c) return c < 0 ? -1 : 1;
	return al < bl ? -1 : al > bl;
}

static int loose_cmp(Value a, Value b);

static int arr_cmp(const Arr *a, const Arr *b) {
	if (a->n != b->n) return a->n < b->n ? -1 : 1;
	for (int i = 0; i < a->n; i++) {
		Value *p = arr_slot((Arr *)b, a->e[i].key, 0);
		if (!p) return 1;
		int c = loose_cmp(a->e[i].val, *p);
		if (c) return c;
	}
	return 0;
}

static int loose_cmp(Value a, Value b) {
	Value x, y;
	if (a.t == T_STR && b.t == T_STR) {
		if (numeric_str(a.u.s, &x) && numeric_str(b.u.s, &y)) return num_cmp(x, y);
		return str_cmp(a.u.s->s, a.u.s->len, b.u.s->s, b.u.s->len);
	}
	if (a.t == T_NULL && b.t == T_STR) return b.u.s->len ? -1 : 0;
	if (b.t == T_NULL && a.t == T_STR) return a.u.s->len ? 1 : 0;
	if (a.t == T_BOOL || b.t == T_BOOL || a.t == T_NULL || b.t == T_NULL)
		return to_bool(a) - to_bool(b);
	if (a.t == T_ARR && b.t == T_ARR) return arr_cmp(a.u.a, b.u.a);
	if (a.t == T_ARR) return 1;
	if (b.t == T_ARR) return -1;
	if (a.t == T_CLO || b.t == T_CLO) return a.t == b.t && a.u.c == b.u.c ? 0 : 1;
	if (a.t == T_OBJ || b.t == T_OBJ) {
		/* same object, or same class with equal properties */
		if (a.t != T_OBJ || b.t != T_OBJ || a.u.o->cls != b.u.o->cls) return 1;
		if (a.u.o == b.u.o) return 0;
		for (int i = 0; i < a.u.o->nprops; i++) {
			int c = loose_cmp(a.u.o->props[i], b.u.o->props[i]);
			if (c) return c;
		}
		return 0;
	}
	if (a.t == T_STR || b.t == T_STR) {
		Value s = a.t == T_STR ? a : b, n = a.t == T_STR ? b : a;
		int c;
		if (numeric_str(s.u.s, &x)) c = num_cmp(n, x);
		else {
			int l;
			const char *ns = sval(n, &l);
			c = str_cmp(ns, l, s.u.s->s, s.u.s->len);
		}
		return a.t == T_STR ? -c : c;
	}
	return num_cmp(a, b);
}

static int strict_eq(Value a, Value b) {
	if (a.t != b.t) return 0;
	switch (a.t) {
	case T_NULL: return 1;
	case T_BOOL: case T_INT: return a.u.i == b.u.i;
	case T_FLOAT: return a.u.f == b.u.f;
	case T_STR: return a.u.s->len == b.u.s->len && !memcmp(a.u.s->s, b.u.s->s, a.u.s->len);
	case T_ARR:
		if (a.u.a->n != b.u.a->n) return 0;
		for (int i = 0; i < a.u.a->n; i++)
			if (!key_eq(a.u.a->e[i].key, b.u.a->e[i].key) ||
			    !strict_eq(a.u.a->e[i].val, b.u.a->e[i].val)) return 0;
		return 1;
	case T_OBJ: return a.u.o == b.u.o;
	case T_CLO: return a.u.c == b.u.c;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static void out_ch(char c) {
	io->putch(c);
	if (c == '\n') col = 0;
	else if (++col >= io->width) col = 0;
}
static void out_n(const char *s, int n) { for (int i = 0; i < n; i++) out_ch(s[i]); }
static void outs(const char *s) { while (*s) out_ch(*s++); }
static void out_val(Value v) { int n; const char *s = sval(v, &n); out_n(s, n); }

/* growing text buffer for sprintf / print_r / implode ... */
typedef struct { char *p; int n, cap; } Buf;

static void buf_add(Buf *b, const char *s, int n) {
	if (b->n + n + 1 > b->cap) {
		int nc = b->cap ? b->cap : 64;
		while (nc < b->n + n + 1) nc *= 2;
		if (nc > STR_LIMIT + 64) { free(b->p); b->p = NULL; fail("String too long"); }
		char *p = realloc(b->p, nc);
		if (!p) { free(b->p); b->p = NULL; fail("Out of memory"); }
		b->p = p;
		b->cap = nc;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}
static void buf_adds(Buf *b, const char *s) { buf_add(b, s, (int)strlen(s)); }
static Value buf_value(Buf *b) { Value v = mk_str(b->p ? b->p : "", b->n); free(b->p); b->p = NULL; return v; }

/* ------------------------------------------------------------------ */
/* lexer                                                               */
/* ------------------------------------------------------------------ */

enum { TK_EOF, TK_INT, TK_FLOAT, TK_STR, TK_ISTR, TK_VAR, TK_ID, TK_OP, TK_CAST };

typedef struct {
	int t, line, len;
	int i;
	float f;
	char s[LINE_MAX_LEN + 1];
} Tok;

static const char *lsrc;          /* source being lexed                 */
static int  lpos, lline;          /* position, line index in the file     */
static int  fixedLine = -2;       /* >= -1: nested lexing of "{$..}"     */
static int  lexFile = -1;         /* file being lexed, -1 = REPL line      */
static Tok  tok, peekTok;
static int  havePeek;

static int cur_line(void) {
	if (fixedLine >= -1) return fixedLine;
	return lexFile >= 0 ? ((lexFile << 10) | (lline + 1)) : -1;
}

static void syntax_error(void) __attribute__((noreturn));

static const char *const ops[] = {
	"===", "!==", "**=", "<=>", "<<=", ">>=", "?\?=",
	"==", "!=", "<>", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=",
	".=", "%=", "**", "->", "=>", "::", "<<", ">>", "??", "&=", "|=",
	"+", "-", "*", "/", "%", "=", "<", ">", "!", ".", ",", ";", ":", "(", ")",
	"[", "]", "{", "}", "?", "&", "|", "^", "~", "@", NULL
};

static int is_cast_word(const char *w, int n) {
	static const char *const casts[] = { "int", "integer", "float", "double", "string", "bool", "boolean", "array", NULL };
	for (int i = 0; casts[i]; i++)
		if ((int)strlen(casts[i]) == n && !strncasecmp(casts[i], w, n)) return 1;
	return 0;
}

static void lex(Tok *t) {
	const char *s = lsrc;
	for (;;) {
		char c = s[lpos];
		if (c == '\n') { lline++; lpos++; continue; }
		if (c == ' ' || c == '\t' || c == '\r') { lpos++; continue; }
		if (c == '#' || (c == '/' && s[lpos + 1] == '/')) {
			while (s[lpos] && s[lpos] != '\n') lpos++;
			continue;
		}
		if (c == '/' && s[lpos + 1] == '*') {
			lpos += 2;
			while (s[lpos] && !(s[lpos] == '*' && s[lpos + 1] == '/')) { if (s[lpos] == '\n') lline++; lpos++; }
			if (s[lpos]) lpos += 2;
			continue;
		}
		if (c == '<' && s[lpos + 1] == '?') {
			lpos += 2;
			if (!strncasecmp(s + lpos, "php", 3)) lpos += 3;
			continue;
		}
		if (c == '?' && s[lpos + 1] == '>') { lpos += 2; continue; }
		break;
	}
	t->line = cur_line();
	t->len = 0;
	t->s[0] = 0;
	char c = s[lpos];
	if (!c) { t->t = TK_EOF; return; }

	/* numbers */
	if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)s[lpos + 1]))) {
		if (c == '0' && (s[lpos + 1] == 'x' || s[lpos + 1] == 'X')) {
			char *e;
			long v = strtol(s + lpos + 2, &e, 16);
			lpos = (int)(e - s);
			t->t = TK_INT;
			t->i = (int)v;
			return;
		}
		Value v;
		int n = parse_num(s + lpos, (int)strlen(s + lpos), &v);
		lpos += n;
		if (v.t == T_INT) { t->t = TK_INT; t->i = v.u.i; }
		else { t->t = TK_FLOAT; t->f = v.u.f; }
		return;
	}
	/* $variable */
	if (c == '$' && (isalpha((unsigned char)s[lpos + 1]) || s[lpos + 1] == '_')) {
		lpos++;
		while (isalnum((unsigned char)s[lpos]) || s[lpos] == '_') {
			if (t->len >= NAME_MAX_LEN) fail("Name too long");
			t->s[t->len++] = s[lpos++];
		}
		t->s[t->len] = 0;
		t->t = TK_VAR;
		return;
	}
	/* identifier / keyword */
	if (isalpha((unsigned char)c) || c == '_') {
		while (isalnum((unsigned char)s[lpos]) || s[lpos] == '_') {
			if (t->len >= NAME_MAX_LEN) fail("Name too long");
			t->s[t->len++] = s[lpos++];
		}
		t->s[t->len] = 0;
		t->t = TK_ID;
		return;
	}
	/* strings */
	if (c == '\'' || c == '"') {
		char q = c;
		int interp = 0;
		lpos++;
		while (s[lpos] && s[lpos] != q) {
			char ch = s[lpos++];
			if (ch == '\n') lline++;
			if (ch == '\\' && s[lpos]) {
				char e = s[lpos++];
				if (q == '\'') {
					if (e != '\'' && e != '\\') { if (t->len < LINE_MAX_LEN) t->s[t->len++] = '\\'; }
					ch = e;
				} else {
					/* keep escapes raw in interpolated strings */
					if (t->len + 2 > LINE_MAX_LEN) fail("String too long");
					t->s[t->len++] = '\\';
					ch = e;
				}
			} else if (q == '"' && ch == '$' && (isalpha((unsigned char)s[lpos]) || s[lpos] == '_')) interp = 1;
			else if (q == '"' && ch == '{' && s[lpos] == '$') interp = 1;
			if (t->len >= LINE_MAX_LEN) fail("String too long");
			t->s[t->len++] = ch;
		}
		if (!s[lpos]) { t->t = TK_EOF; fail("Unterminated string"); }
		lpos++;
		t->s[t->len] = 0;
		if (q == '\'') { t->t = TK_STR; return; }
		if (interp) { t->t = TK_ISTR; return; }
		/* process escapes of a plain double-quoted string */
		int o = 0;
		for (int i = 0; i < t->len; i++) {
			char ch = t->s[i];
			if (ch == '\\' && i + 1 < t->len) {
				char e = t->s[++i];
				switch (e) {
				case 'n': ch = '\n'; break;
				case 't': ch = '\t'; break;
				case 'r': ch = '\r'; break;
				case 'e': ch = 27; break;
				case '0': ch = 0; break;
				case '\\': case '"': case '$': ch = e; break;
				default: t->s[o++] = '\\'; ch = e;
				}
			}
			t->s[o++] = ch;
		}
		t->len = o;
		t->s[o] = 0;
		t->t = TK_STR;
		return;
	}
	/* (int) style casts */
	if (c == '(') {
		int p = lpos + 1;
		while (s[p] == ' ') p++;
		int w = p;
		while (isalpha((unsigned char)s[p])) p++;
		int wl = p - w;
		while (s[p] == ' ') p++;
		if (wl && s[p] == ')' && is_cast_word(s + w, wl)) {
			t->t = TK_CAST;
			memcpy(t->s, s + w, wl);
			t->s[wl] = 0;
			for (int i = 0; i < wl; i++) t->s[i] = (char)tolower((unsigned char)t->s[i]);
			lpos = p + 1;
			return;
		}
	}
	for (int i = 0; ops[i]; i++) {
		int n = (int)strlen(ops[i]);
		if (!strncmp(s + lpos, ops[i], n)) {
			strcpy(t->s, ops[i]);
			t->len = n;
			t->t = TK_OP;
			lpos += n;
			return;
		}
	}
	t->t = TK_OP;
	t->s[0] = c;
	t->s[1] = 0;
	lpos++;
	syntax_error();
}

static void advance(void) {
	if (havePeek) { tok = peekTok; havePeek = 0; }
	else lex(&tok);
}

static Tok *peek(void) {
	if (!havePeek) { lex(&peekTok); havePeek = 1; }
	return &peekTok;
}

static int is_op(const char *o) { return tok.t == TK_OP && !strcmp(tok.s, o); }
static int is_kw(const char *k) { return tok.t == TK_ID && !strcasecmp(tok.s, k); }
static int accept(const char *o) { if (is_op(o)) { advance(); return 1; } return 0; }
static void expect(const char *o) { if (!accept(o)) syntax_error(); }

static void syntax_error(void) {
	errLine = tok.line;
	switch (tok.t) {
	case TK_EOF: fail("unexpected end of file");
	case TK_VAR: fail("unexpected $%s", tok.s);
	case TK_STR: case TK_ISTR: fail("unexpected string");
	case TK_INT: case TK_FLOAT: fail("unexpected number");
	default: fail("unexpected '%s'", tok.s);
	}
}

/* ------------------------------------------------------------------ */
/* bytecode                                                            */
/* ------------------------------------------------------------------ */

enum {
	OP_END, OP_CONST, OP_INT, OP_NULL, OP_TRUE, OP_FALSE, OP_POP, OP_DUP,
	OP_APPENDKEY,                 /* marker for $a[] = ...                   */
	OP_LOAD, OP_STORE, OP_INCDEC, OP_ISSET, OP_UNSET,   /* scope slot nkeys [op] */
	OP_INDEX,
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_CONCAT,
	OP_EQ, OP_NE, OP_IDENT, OP_NIDENT, OP_LT, OP_GT, OP_LE, OP_GE, OP_SPACESHIP,
	OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_LXOR, OP_LAND,
	OP_NEG, OP_PLUS, OP_NOT, OP_BNOT, OP_TOBOOL, OP_CAST,
	OP_JMP, OP_JZ, OP_JNZ, OP_JZK, OP_JNZK, OP_JNNK,
	OP_ECHO, OP_ARR_NEW, OP_ARR_PUSH, OP_ARR_SET,
	OP_FE_INIT, OP_FE_NEXT,
	OP_CALL, OP_CALLB, OP_CALLREF, OP_RET, OP_EXIT,
	OP_NEW, OP_MCALL, OP_SCALL, OP_CCONST, OP_CLSNAME, OP_INSTOF, OP_CLONE, OP_TOPNAME,
	OP_CLOSURE,                   /* func ncaps: closure from the captured values */
	OP_CALLV,                     /* argc: call the value below the arguments  */
	OP_TRY,                       /* handler: install an exception handler     */
	OP_ENDTRY,                    /* remove the innermost handler              */
	OP_THROW,                     /* throw the value on the stack              */
	OP_EXCMATCH,                  /* class depth: push (exception instanceof class) */
	OP_JSR, OP_RETSUB             /* call / return from a finally block        */
};

/* class references in the bytecode: index, or these for self / static / parent */
#define CREF_SELF   -1
#define CREF_STATIC -2
#define CREF_PARENT -3
#define CREF_CLOSURE -4            /* instanceof Closure */
/* VarRef scopes: 0 local, 1 global, 2 $this, >= SC_STATIC: static property
 * of class (scope - SC_STATIC + CREF bias), slot = name in the constant pool */
#define SC_STATIC   10

static int            code[CODE_MAX];
static unsigned short codeLine[CODE_MAX];
static int            ncode;
static Value          consts[CONST_MAX];
static int            nconst;

typedef struct {
	char  name[NAME_MAX_LEN + 1];      /* lower case                       */
	int   addr;                        /* -1 = called but not defined yet  */
	int   nparams, nrequired, nlocals;
	int   firstCallLine;
	Value defs[PARAMS_MAX];
	int   ncaps;                       /* closure: captured variables      */
	unsigned char capSlot[CAPS_MAX];   /* their local slots                */
} Func;
static Func funcs[FUNCS_MAX];
static int  nfuncs;

static char  globalNames[GLOBALS_MAX][NAME_MAX_LEN + 1];
static Value globals[GLOBALS_MAX];
static int   nglobals;

/* what the RUN-compiled program occupies; direct lines go behind it */
static int progCode, progConst, progFuncs, progClasses;
/* REPL lines that create closures stay in memory (the closures may live on
 * in variables); the next REPL line is compiled behind them */
static int keepCode, keepConst, keepFuncs, closuresMade;
static int directCompile;          /* compiling a REPL line */
static int compilingPrelude;       /* compiling the built-in classes */

static int compiling;

static int errLineSet(int line) {
	if (compiling && errLine == -3) errLine = line;
	return 0;
}

static void fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errText, sizeof errText, fmt, ap);
	va_end(ap);
	errIsParse = compiling;
	if (compiling) { if (errLine == -3) errLine = tok.line; }
	longjmp(errjmp, 1);
}

static void emit(int w) {
	if (ncode >= CODE_MAX) fail("Program too large");
	codeLine[ncode] = (unsigned short)(tok.line < 0 ? 0 : tok.line);
	code[ncode++] = w;
}
static void emit2(int a, int b) { emit(a); emit(b); }
static int emit_jump(int op) { emit(op); emit(0); return ncode - 1; }
static void patch(int at) { code[at] = ncode; }

static int add_const(Value v) {
	if (nconst >= CONST_MAX) { release(v); fail("Program too large"); }
	consts[nconst] = v;
	return nconst++;
}

static void emit_const(Value v) { emit2(OP_CONST, add_const(v)); }

/* --- variables --- */

typedef struct { int scope, slot; } VarRef;     /* scope 0 local, 1 global */

typedef struct {
	char names[LOCALS_MAX][NAME_MAX_LEN + 1];
	int  n;
	char globals[16][NAME_MAX_LEN + 1];
	int  ng;
	int  isMethod;                     /* $this is available */
	/* closures: captured names and their local slots; an arrow function
	 * (fn) captures every outer variable its body uses */
	char caps[CAPS_MAX][NAME_MAX_LEN + 1];
	unsigned char capSlot[CAPS_MAX];
	int  ncap, arrow, capturing;
} Scope;
static Scope *scope;              /* NULL = top level (globals)            */

static int global_slot(const char *name) {
	for (int i = 0; i < nglobals; i++) if (!strcmp(globalNames[i], name)) return i;
	if (nglobals >= GLOBALS_MAX) fail("Too many variables");
	strcpy(globalNames[nglobals], name);
	globals[nglobals] = mk_null();
	return nglobals++;
}

static void add_capture(const char *name, int slot) {
	if (scope->ncap >= CAPS_MAX) fail("Too many captured variables");
	strcpy(scope->caps[scope->ncap], name);
	scope->capSlot[scope->ncap++] = (unsigned char)slot;
}

static VarRef resolve_var(const char *name) {
	VarRef r;
	if (!strcmp(name, "this")) {
		if (!scope || !scope->isMethod) fail("Cannot use $this outside a method");
		r.scope = 2;
		r.slot = 0;
		return r;
	}
	if (!scope) { r.scope = 1; r.slot = global_slot(name); return r; }
	for (int i = 0; i < scope->ng; i++)
		if (!strcmp(scope->globals[i], name)) { r.scope = 1; r.slot = global_slot(name); return r; }
	r.scope = 0;
	for (int i = 0; i < scope->n; i++) if (!strcmp(scope->names[i], name)) { r.slot = i; return r; }
	if (scope->n >= LOCALS_MAX) fail("Too many local variables");
	strcpy(scope->names[scope->n], name);
	r.slot = scope->n++;
	if (scope->arrow && scope->capturing) add_capture(name, r.slot);
	return r;
}

/* --- functions --- */

static int func_index(const char *name, int create) {
	char low[NAME_MAX_LEN + 1];
	int i;
	for (i = 0; name[i] && i < NAME_MAX_LEN; i++) low[i] = (char)tolower((unsigned char)name[i]);
	low[i] = 0;
	for (i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, low)) return i;
	if (!create) return -1;
	if (nfuncs >= FUNCS_MAX) fail("Too many functions");
	memset(&funcs[nfuncs], 0, sizeof(Func));
	strcpy(funcs[nfuncs].name, low);
	funcs[nfuncs].addr = -1;
	funcs[nfuncs].firstCallLine = tok.line;
	return nfuncs++;
}

/* function entry for a method (not callable by name) */
static int new_func(const char *display) {
	if (nfuncs >= FUNCS_MAX) fail("Too many functions");
	memset(&funcs[nfuncs], 0, sizeof(Func));
	snprintf(funcs[nfuncs].name, sizeof funcs[nfuncs].name, "%.31s", display);
	funcs[nfuncs].addr = -1;
	return nfuncs++;
}

/* class by name; unknown classes get a placeholder that must be declared
 * somewhere in the program (checked when linking) */
static int class_index(const char *name) {
	int i = find_class(name);
	if (i >= 0) return i;
	if (nclasses >= CLASSES_MAX) fail("Too many classes");
	Class *k = &classes[nclasses];
	memset(k, 0, sizeof *k);
	snprintf(k->name, sizeof k->name, "%s", name);
	k->firstUse = tok.line;
	k->parent = -1;
	return nclasses++;
}

static int class_ref(const char *name) {
	if (!strcasecmp(name, "self")) return CREF_SELF;
	if (!strcasecmp(name, "static")) return CREF_STATIC;
	if (!strcasecmp(name, "parent")) return CREF_PARENT;
	return class_index(name);
}

static Value mk_pname(const char *name) {
	Value v = mk_cstr(name);
	v.t = T_PNAME;
	return v;
}

static void link_all(void) {
	for (int i = 0; i < nclasses; i++)
		if (!classes[i].defined) {
			errLineSet(classes[i].firstUse);
			fail("Class %s not found", classes[i].name);
		}
	for (int i = 0; i < nclasses; i++) link_class(i);
}

/* builtins (table further below) */
static int  find_builtin(const char *name);
static int  builtin_takes_ref(int b);
static void builtin_arity(int b, int *mn, int *mx);
static int  find_constant(const char *name, Value *out);
static void define_user_const(const char *name, Value v);
static void clear_user_consts(void);

/* --- loops (break / continue) --- */

typedef struct {
	int breaks[JUMPS_MAX], nb;
	int conts[JUMPS_MAX], nc;
	int slots;                  /* stack slots the construct keeps: foreach 2, switch 1 */
	int isSwitch;
} Loop;
static Loop loops[LOOPS_MAX];
static int  nloops, loopBase;         /* loopBase: first loop of the current function */

/* try blocks being compiled: return/break/continue leaving one must pop
 * its handler and run its finally block first */
typedef struct {
	int active;                 /* a handler is installed (try or catch body) */
	int inFinally;              /* compiling the finally block               */
	int nloops;                 /* loops open when the try started           */
	int jsr[JUMPS_MAX], njsr;   /* calls of the finally block to patch       */
} TryCtx;
static TryCtx trys[LOOPS_MAX];
static int    ntrys, tryBase;

static Loop *push_loop(int slots, int isSwitch) {
	if (nloops >= LOOPS_MAX) fail("Loops nested too deeply");
	Loop *l = &loops[nloops++];
	l->nb = l->nc = 0;
	l->slots = slots;
	l->isSwitch = isSwitch;
	return l;
}

static void pop_loop(int breakTarget, int contTarget) {
	Loop *l = &loops[--nloops];
	for (int i = 0; i < l->nb; i++) code[l->breaks[i]] = breakTarget;
	for (int i = 0; i < l->nc; i++) code[l->conts[i]] = contTarget;
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

static void expr(void);
static void expr_assign(void);
static void expr_unary(void);
static void statement(void);
static int  find_src(const char *name);
static void compile_file(int f);
static char included[BASIC_PROJECT_FILES];

static void rvalue_postfix(void);
static void call_args(int *argc);
static void compile_closure(int arrow);

/* path behind a variable: [key] and ->prop elements are pushed on the
 * stack. A method call in the chain turns it into a value (the result is
 * on the stack) and 1 is returned. */
static int parse_path(VarRef r, int *nkeys, int *hasAppend) {
	*nkeys = 0;
	*hasAppend = 0;
	for (;;) {
		if (is_op("[")) {
			advance();
			if (is_op("]")) { emit(OP_APPENDKEY); *hasAppend = 1; }
			else expr();
			expect("]");
			(*nkeys)++;
			continue;
		}
		if (is_op("->")) {
			advance();
			if (tok.t == TK_VAR) {                 /* $obj->$name */
				VarRef v = resolve_var(tok.s);
				advance();
				emit(OP_LOAD); emit(v.scope); emit(v.slot); emit(0);
				emit(OP_TOPNAME);
				(*nkeys)++;
				continue;
			}
			if (tok.t != TK_ID) syntax_error();
			char name[NAME_MAX_LEN + 1];
			strcpy(name, tok.s);
			advance();
			if (is_op("(")) {                      /* method call */
				if (*hasAppend) fail("Cannot use [] for reading");
				emit(OP_LOAD); emit(r.scope); emit(r.slot); emit(*nkeys);
				int argc;
				call_args(&argc);
				emit(OP_MCALL); emit(add_const(mk_cstr(name))); emit(argc);
				rvalue_postfix();
				return 1;
			}
			emit_const(mk_pname(name));
			(*nkeys)++;
			continue;
		}
		return 0;
	}
}

/* base of an lvalue: $var or Class::$prop */
static VarRef parse_base(void) {
	VarRef r;
	if (tok.t == TK_VAR) {
		r = resolve_var(tok.s);
		advance();
		return r;
	}
	if (tok.t == TK_ID && peek()->t == TK_OP && !strcmp(peek()->s, "::")) {
		int cr = class_ref(tok.s);
		advance();
		advance();
		if (tok.t != TK_VAR) syntax_error();
		r.scope = SC_STATIC + cr + 3;
		r.slot = add_const(mk_cstr(tok.s));
		advance();
		return r;
	}
	syntax_error();
}

/* lvalue for isset/unset/references/++: no method calls allowed */
static VarRef parse_lvalue(int *nkeys, int *hasAppend) {
	VarRef r = parse_base();
	if (parse_path(r, nkeys, hasAppend)) fail("Cannot use a method call result here");
	return r;
}

static int assign_op(void) {
	static const struct { const char *s; int op; } a[] = {
		{ "=", 0 }, { "+=", OP_ADD }, { "-=", OP_SUB }, { "*=", OP_MUL }, { "/=", OP_DIV },
		{ ".=", OP_CONCAT }, { "%=", OP_MOD }, { "**=", OP_POW }, { "&=", OP_BAND },
		{ "|=", OP_BOR }, { "<<=", OP_SHL }, { ">>=", OP_SHR }, { "?\?=", -1 }, { NULL, 0 }
	};
	if (tok.t != TK_OP) return -2;
	for (int i = 0; a[i].s; i++) if (!strcmp(tok.s, a[i].s)) return a[i].op;
	return -2;
}

/* after the base of a variable expression: path, then assignment, ++/--
 * or a plain load */
static void lvalue_rest(VarRef r) {
	int n, app;
	if (parse_path(r, &n, &app)) return;       /* became a method call */
	int op = assign_op();
	if (op == -1) {
		/* null coalescing assignment would evaluate the keys twice */
		fail("?\?= is not supported");
	}
	if (r.scope == 2 && n == 0 && (op != -2 || is_op("++") || is_op("--")))
		fail("Cannot re-assign $this");
	if (op != -2) {
		advance();
		if (op == 0 && is_op("&")) fail("References are not supported");
		expr_assign();
		emit(OP_STORE); emit(r.scope); emit(r.slot); emit(n); emit(op);
		return;
	}
	if (is_op("++") || is_op("--")) {
		int dec = is_op("--");
		advance();
		emit(OP_INCDEC); emit(r.scope); emit(r.slot); emit(n); emit(dec | 2);
		return;
	}
	if (app) fail("Cannot use [] for reading");
	emit(OP_LOAD); emit(r.scope); emit(r.slot); emit(n);
	if (is_op("(")) rvalue_postfix();         /* $f(...) */
}

static void var_expr(void) {
	VarRef r = parse_base();
	lvalue_rest(r);
}

/* method calls, properties and [ ] behind a value that is not a variable */
static void rvalue_postfix(void) {
	for (;;) {
		if (is_op("(")) {                      /* call of a closure or callable */
			int argc;
			call_args(&argc);
			emit2(OP_CALLV, argc);
			continue;
		}
		if (is_op("[")) {
			advance();
			expr();
			expect("]");
			emit(OP_INDEX);
			continue;
		}
		if (is_op("->")) {
			advance();
			if (tok.t == TK_VAR) {
				VarRef v = resolve_var(tok.s);
				advance();
				emit(OP_LOAD); emit(v.scope); emit(v.slot); emit(0);
				emit(OP_TOPNAME);
				emit(OP_INDEX);
				continue;
			}
			if (tok.t != TK_ID) syntax_error();
			char name[NAME_MAX_LEN + 1];
			strcpy(name, tok.s);
			advance();
			if (is_op("(")) {
				int argc;
				call_args(&argc);
				emit(OP_MCALL); emit(add_const(mk_cstr(name))); emit(argc);
			} else {
				emit_const(mk_pname(name));
				emit(OP_INDEX);
			}
			continue;
		}
		return;
	}
}

/* "text $var {$expr} text" */
static void parse_sub_expr(const char *src, int line) {
	const char *sSrc = lsrc;
	int sPos = lpos, sLine = lline, sFixed = fixedLine, sHave = havePeek;
	Tok sTok = tok, sPeek = peekTok;
	lsrc = src; lpos = 0; fixedLine = line; havePeek = 0;
	advance();
	expr();
	if (tok.t != TK_EOF) syntax_error();
	lsrc = sSrc; lpos = sPos; lline = sLine; fixedLine = sFixed; havePeek = sHave;
	tok = sTok; peekTok = sPeek;
}

static void compile_interp(const char *raw, int len, int line) {
	char buf[LINE_MAX_LEN + 1];
	int bl = 0, parts = 0;
#define FLUSH() do { if (bl) { emit_const(mk_str(buf, bl)); if (parts++) emit(OP_CONCAT); bl = 0; } } while (0)
#define BEGIN_PART() do { FLUSH(); if (!parts) { emit_const(mk_str("", 0)); parts = 1; } } while (0)
	for (int i = 0; i < len; i++) {
		char c = raw[i];
		if (c == '\\' && i + 1 < len) {
			char e = raw[++i];
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'e': c = 27; break;
			case '0': c = 0; break;
			case '\\': case '"': case '$': case '{': c = e; break;
			default: buf[bl++] = '\\'; c = e;
			}
			buf[bl++] = c;
			continue;
		}
		if (c == '$' && i + 1 < len && (isalpha((unsigned char)raw[i + 1]) || raw[i + 1] == '_')) {
			char name[NAME_MAX_LEN + 1];
			int nl = 0;
			i++;
			while (i < len && (isalnum((unsigned char)raw[i]) || raw[i] == '_') && nl < NAME_MAX_LEN) name[nl++] = raw[i++];
			name[nl] = 0;
			BEGIN_PART();
			VarRef r = resolve_var(name);
			int nk = 0;
			if (i < len && raw[i] == '[') {
				/* simple key: number, bare word or $var */
				int j = i + 1, k0 = j;
				while (j < len && raw[j] != ']') j++;
				if (j < len) {
					char key[NAME_MAX_LEN + 1];
					int kl = j - k0 < NAME_MAX_LEN ? j - k0 : NAME_MAX_LEN;
					memcpy(key, raw + k0, kl);
					key[kl] = 0;
					Value kv;
					if (key[0] == '$') {
						VarRef kr = resolve_var(key + 1);
						emit(OP_LOAD); emit(kr.scope); emit(kr.slot); emit(0);
					} else if (parse_num(key, kl, &kv) == kl && kv.t == T_INT) emit2(OP_INT, kv.u.i);
					else emit_const(mk_str(key, kl));
					nk = 1;
					i = j + 1;
				}
			} else if (i + 2 < len && raw[i] == '-' && raw[i + 1] == '>' &&
			           (isalpha((unsigned char)raw[i + 2]) || raw[i + 2] == '_')) {
				/* "$obj->prop" */
				char prop[NAME_MAX_LEN + 1];
				int pl = 0;
				i += 2;
				while (i < len && (isalnum((unsigned char)raw[i]) || raw[i] == '_') && pl < NAME_MAX_LEN) prop[pl++] = raw[i++];
				prop[pl] = 0;
				emit_const(mk_pname(prop));
				nk = 1;
			}
			emit(OP_LOAD); emit(r.scope); emit(r.slot); emit(nk);
			emit(OP_CONCAT);
			i--;
			continue;
		}
		if (c == '{' && i + 1 < len && raw[i + 1] == '$') {
			int depth = 0, j = i;
			for (; j < len; j++) {
				if (raw[j] == '{') depth++;
				else if (raw[j] == '}' && --depth == 0) break;
			}
			if (j >= len) fail("Unterminated {$ in string");
			char sub[LINE_MAX_LEN + 1];
			memcpy(sub, raw + i + 1, j - i - 1);
			sub[j - i - 1] = 0;
			BEGIN_PART();
			parse_sub_expr(sub, line);
			emit(OP_CONCAT);
			i = j;
			continue;
		}
		buf[bl++] = c;
	}
	FLUSH();
	if (!parts) emit_const(mk_str("", 0));
#undef FLUSH
#undef BEGIN_PART
}

static void call_args(int *argc) {
	expect("(");
	*argc = 0;
	if (!is_op(")")) {
		do {
			if (is_op(")")) break;      /* trailing comma */
			expr_assign();
			(*argc)++;
		} while (accept(","));
	}
	expect(")");
}

static void compile_call(const char *name) {
	int b = find_builtin(name);
	int argc;
	if (b >= 0 && builtin_takes_ref(b)) {
		expect("(");
		int n, app;
		VarRef r = parse_lvalue(&n, &app);
		if (app) fail("Cannot use [] here");
		argc = 1;
		while (accept(",")) { expr_assign(); argc++; }
		expect(")");
		int mn, mx;
		builtin_arity(b, &mn, &mx);
		if (argc < mn || (mx >= 0 && argc > mx)) fail("Wrong argument count for %s()", name);
		emit(OP_CALLREF); emit(b); emit(r.scope); emit(r.slot); emit(n); emit(argc);
		return;
	}
	call_args(&argc);
	if (b >= 0) {
		int mn, mx;
		builtin_arity(b, &mn, &mx);
		if (argc < mn || (mx >= 0 && argc > mx)) fail("Wrong argument count for %s()", name);
		emit(OP_CALLB); emit(b); emit(argc);
	} else {
		int f = func_index(name, 1);
		emit(OP_CALL); emit(f); emit(argc);
	}
}

static void array_literal(const char *close) {
	emit(OP_ARR_NEW);
	while (!is_op(close)) {
		expr_assign();
		if (accept("=>")) { expr_assign(); emit(OP_ARR_SET); }
		else emit(OP_ARR_PUSH);
		if (!accept(",")) break;
	}
	expect(close);
}

static void primary(void) {
	char name[NAME_MAX_LEN + 1];
	switch (tok.t) {
	case TK_INT: emit2(OP_INT, tok.i); advance(); return;
	case TK_FLOAT: emit_const(mk_float(tok.f)); advance(); return;
	case TK_STR: emit_const(mk_str(tok.s, tok.len)); advance(); return;
	case TK_ISTR: {
		char raw[LINE_MAX_LEN + 1];
		int len = tok.len, line = tok.line;
		memcpy(raw, tok.s, len + 1);
		compile_interp(raw, len, line);
		advance();
		return;
	}
	case TK_VAR: var_expr(); return;
	case TK_OP:
		if (accept("(")) { expr(); expect(")"); rvalue_postfix(); return; }
		if (accept("[")) { array_literal("]"); return; }
		syntax_error();
	case TK_ID:
		break;
	default:
		syntax_error();
	}
	strcpy(name, tok.s);
	if (!strcasecmp(name, "true")) { advance(); emit(OP_TRUE); return; }
	if (!strcasecmp(name, "false")) { advance(); emit(OP_FALSE); return; }
	if (!strcasecmp(name, "null")) { advance(); emit(OP_NULL); return; }
	if (!strcasecmp(name, "array") && peek()->t == TK_OP && !strcmp(peek()->s, "(")) {
		advance(); advance();
		array_literal(")");
		return;
	}
	if (!strcasecmp(name, "isset")) {
		advance();
		expect("(");
		int first = 1;
		do {
			int n, app;
			VarRef r = parse_lvalue(&n, &app);
			if (app) fail("Cannot use [] here");
			emit(OP_ISSET); emit(r.scope); emit(r.slot); emit(n);
			if (!first) emit(OP_LAND);
			first = 0;
		} while (accept(","));
		expect(")");
		return;
	}
	if (!strcasecmp(name, "empty")) {
		advance();
		expect("(");
		expr();
		expect(")");
		emit(OP_NOT);
		return;
	}
	if (!strcasecmp(name, "exit") || !strcasecmp(name, "die")) {
		advance();
		if (accept("(")) {
			if (!is_op(")")) expr(); else emit(OP_NULL);
			expect(")");
		} else emit(OP_NULL);
		emit(OP_EXIT);
		emit(OP_NULL);              /* expression value, never reached */
		return;
	}
	if (!strcasecmp(name, "throw")) {     /* PHP 8: throw is an expression */
		advance();
		expr_assign();
		emit(OP_THROW);
		emit(OP_NULL);                     /* never reached */
		return;
	}
	if (!strcasecmp(name, "print")) {
		advance();
		expr_assign();
		emit(OP_ECHO);
		emit2(OP_INT, 1);
		return;
	}
	if (!strcasecmp(name, "function") || !strcasecmp(name, "fn")) {
		compile_closure(!strcasecmp(name, "fn"));
		rvalue_postfix();
		return;
	}
	if (!strcasecmp(name, "static") && peek()->t == TK_ID &&
	    (!strcasecmp(peek()->s, "function") || !strcasecmp(peek()->s, "fn"))) {
		advance();                         /* static closure: same as a closure here */
		compile_closure(!strcasecmp(tok.s, "fn"));
		rvalue_postfix();
		return;
	}
	if (!strcasecmp(name, "new")) {
		advance();
		if (tok.t != TK_ID) fail("new needs a class name");
		int cr = class_ref(tok.s);
		advance();
		int argc = 0;
		if (is_op("(")) call_args(&argc);
		emit(OP_NEW); emit(cr); emit(argc);
		rvalue_postfix();
		return;
	}
	if (!strcasecmp(name, "clone")) {
		advance();
		expr_unary();
		emit(OP_CLONE);
		return;
	}
	if (peek()->t == TK_OP && !strcmp(peek()->s, "::")) {
		/* Class::$static, Class::CONST, Class::method(), Class::class */
		int cr = class_ref(name);
		advance();
		advance();
		if (tok.t == TK_VAR) {
			VarRef r;
			r.scope = SC_STATIC + cr + 3;
			r.slot = add_const(mk_cstr(tok.s));
			advance();
			lvalue_rest(r);
			return;
		}
		if (tok.t != TK_ID) syntax_error();
		char member[NAME_MAX_LEN + 1];
		strcpy(member, tok.s);
		advance();
		if (!strcasecmp(member, "class") && !is_op("(")) {
			if (cr >= 0) emit_const(mk_cstr(classes[cr].name));
			else emit2(OP_CLSNAME, cr);
		} else if (is_op("(")) {
			int argc;
			call_args(&argc);
			emit(OP_SCALL); emit(cr); emit(add_const(mk_cstr(member))); emit(argc);
		} else {
			emit(OP_CCONST); emit(cr); emit(add_const(mk_cstr(member)));
		}
		rvalue_postfix();
		return;
	}
	advance();
	if (is_op("(")) { compile_call(name); rvalue_postfix(); return; }
	Value cv;
	if (find_constant(name, &cv)) { emit_const(cv); return; }
	errLine = tok.line;
	fail("Undefined constant %s", name);
}

static void expr_postfix(void) {
	int isVar = tok.t == TK_VAR;
	primary();
	if (isVar) return;
	while (is_op("[")) {
		advance();
		expr();
		expect("]");
		emit(OP_INDEX);
	}
}

static void expr_pow(void) {
	expr_postfix();
	while (is_kw("instanceof")) {
		advance();
		if (tok.t != TK_ID) fail("instanceof needs a class name");
		int cr = !strcasecmp(tok.s, "Closure") ? CREF_CLOSURE : class_ref(tok.s);
		advance();
		emit2(OP_INSTOF, cr);
	}
	if (accept("**")) { expr_unary(); emit(OP_POW); }
}

static void expr_unary(void) {
	if (accept("-")) { expr_unary(); emit(OP_NEG); return; }
	if (accept("+")) { expr_unary(); emit(OP_PLUS); return; }
	if (accept("!")) { expr_unary(); emit(OP_NOT); return; }
	if (accept("~")) { expr_unary(); emit(OP_BNOT); return; }
	if (accept("@")) { expr_unary(); return; }
	if (tok.t == TK_CAST) {
		int t;
		switch (tok.s[0]) {
		case 'i': t = T_INT; break;
		case 'f': case 'd': t = T_FLOAT; break;
		case 's': t = T_STR; break;
		case 'b': t = T_BOOL; break;
		default: t = T_ARR;
		}
		advance();
		expr_unary();
		emit2(OP_CAST, t);
		return;
	}
	if (is_op("++") || is_op("--")) {
		int dec = is_op("--");
		advance();
		int n, app;
		VarRef r = parse_lvalue(&n, &app);
		if (app) fail("Cannot use [] here");
		emit(OP_INCDEC); emit(r.scope); emit(r.slot); emit(n); emit(dec);
		return;
	}
	expr_pow();
}

static const struct { const char *op; int prec, code; } binops[] = {
	{ "*", 12, OP_MUL }, { "/", 12, OP_DIV }, { "%", 12, OP_MOD },
	{ "+", 11, OP_ADD }, { "-", 11, OP_SUB },
	{ "<<", 10, OP_SHL }, { ">>", 10, OP_SHR },
	{ ".", 9, OP_CONCAT },
	{ "<", 8, OP_LT }, { "<=", 8, OP_LE }, { ">", 8, OP_GT }, { ">=", 8, OP_GE },
	{ "==", 7, OP_EQ }, { "!=", 7, OP_NE }, { "<>", 7, OP_NE }, { "===", 7, OP_IDENT },
	{ "!==", 7, OP_NIDENT }, { "<=>", 7, OP_SPACESHIP },
	{ "&", 6, OP_BAND }, { "^", 5, OP_BXOR }, { "|", 4, OP_BOR },
	{ "&&", 3, -1 }, { "||", 2, -2 }, { "??", 1, -3 },
	{ NULL, 0, 0 }
};

static int find_binop(int *prec) {
	if (tok.t != TK_OP) return -100;
	for (int i = 0; binops[i].op; i++)
		if (!strcmp(tok.s, binops[i].op)) { *prec = binops[i].prec; return binops[i].code; }
	return -100;
}

static void expr_binary(int minPrec) {
	expr_unary();
	for (;;) {
		int prec, op = find_binop(&prec);
		if (op == -100 || prec < minPrec) return;
		advance();
		if (op == -1 || op == -2) {           /* && || short circuit */
			int j = emit_jump(op == -1 ? OP_JZK : OP_JNZK);
			emit(OP_POP);
			expr_binary(prec + 1);
			patch(j);
			emit(OP_TOBOOL);
		} else if (op == -3) {                /* ?? (right assoc) */
			int j = emit_jump(OP_JNNK);
			emit(OP_POP);
			expr_binary(prec);
			patch(j);
		} else {
			expr_binary(prec + 1);
			emit(op);
		}
	}
}

static void expr_ternary(void) {
	expr_binary(1);
	while (is_op("?")) {
		advance();
		if (accept(":")) {                    /* a ?: b */
			int j = emit_jump(OP_JNZK);
			emit(OP_POP);
			expr_binary(1);
			patch(j);
			continue;
		}
		int jf = emit_jump(OP_JZ);
		expr_assign();
		expect(":");
		int je = emit_jump(OP_JMP);
		patch(jf);
		expr_binary(1);
		patch(je);
	}
}

static void expr_assign(void) { expr_ternary(); }

static void expr_and_low(void) {
	expr_assign();
	while (is_kw("and")) {
		advance();
		int j = emit_jump(OP_JZK);
		emit(OP_POP);
		expr_assign();
		patch(j);
		emit(OP_TOBOOL);
	}
}

static void expr_xor_low(void) {
	expr_and_low();
	while (is_kw("xor")) { advance(); expr_and_low(); emit(OP_LXOR); }
}

static void expr(void) {
	expr_xor_low();
	while (is_kw("or")) {
		advance();
		int j = emit_jump(OP_JNZK);
		emit(OP_POP);
		expr_xor_low();
		patch(j);
		emit(OP_TOBOOL);
	}
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void end_stmt(void) {
	if (accept(";")) return;
	if (tok.t == TK_EOF) return;           /* lenient at the very end */
	syntax_error();
}

static void block_or_statement(void) { statement(); }

static void compile_if(void) {
	advance();                             /* if / elseif */
	expect("(");
	expr();
	expect(")");
	int jf = emit_jump(OP_JZ);
	block_or_statement();
	if (is_kw("elseif") || (is_kw("else") && peek()->t == TK_ID && !strcasecmp(peek()->s, "if"))) {
		int je = emit_jump(OP_JMP);
		patch(jf);
		if (is_kw("else")) advance();
		compile_if();
		patch(je);
	} else if (is_kw("else")) {
		advance();
		int je = emit_jump(OP_JMP);
		patch(jf);
		block_or_statement();
		patch(je);
	} else patch(jf);
}

static void compile_while(void) {
	advance();
	int top = ncode;
	expect("(");
	expr();
	expect(")");
	int jf = emit_jump(OP_JZ);
	push_loop(0, 0);
	block_or_statement();
	emit2(OP_JMP, top);
	patch(jf);
	pop_loop(ncode, top);
}

static void compile_do(void) {
	advance();
	int top = ncode;
	push_loop(0, 0);
	block_or_statement();
	int cont = ncode;
	if (!is_kw("while")) syntax_error();
	advance();
	expect("(");
	expr();
	expect(")");
	emit2(OP_JNZ, top);
	end_stmt();
	pop_loop(ncode, cont);
}

static void compile_for(void) {
	advance();
	expect("(");
	if (!is_op(";")) do { expr(); emit(OP_POP); } while (accept(","));
	expect(";");
	int top = ncode, jf = -1;
	if (!is_op(";")) {
		for (;;) {
			expr();
			if (!accept(",")) break;
			emit(OP_POP);
		}
		jf = emit_jump(OP_JZ);
	}
	expect(";");
	int jb = emit_jump(OP_JMP);
	int stepL = ncode;
	if (!is_op(")")) do { expr(); emit(OP_POP); } while (accept(","));
	emit2(OP_JMP, top);
	expect(")");
	patch(jb);
	push_loop(0, 0);
	block_or_statement();
	emit2(OP_JMP, stepL);
	if (jf >= 0) patch(jf);
	pop_loop(ncode, stepL);
}

static void compile_foreach(void) {
	advance();
	expect("(");
	expr();
	if (!is_kw("as")) syntax_error();
	advance();
	emit(OP_FE_INIT);
	int top = ncode;
	int jEnd = emit_jump(OP_FE_NEXT);       /* pushes key, value */
	if (is_op("&")) fail("References are not supported");
	if (tok.t != TK_VAR) syntax_error();
	VarRef a = resolve_var(tok.s), keyVar = { -1, -1 }, valVar = a;
	advance();
	if (accept("=>")) {
		if (is_op("&")) fail("References are not supported");
		if (tok.t != TK_VAR) syntax_error();
		keyVar = a;
		valVar = resolve_var(tok.s);
		advance();
	}
	expect(")");
	emit(OP_STORE); emit(valVar.scope); emit(valVar.slot); emit(0); emit(0);
	emit(OP_POP);
	if (keyVar.scope >= 0) {
		emit(OP_STORE); emit(keyVar.scope); emit(keyVar.slot); emit(0); emit(0);
	}
	emit(OP_POP);
	push_loop(2, 0);
	block_or_statement();
	emit2(OP_JMP, top);
	patch(jEnd);
	pop_loop(ncode, top);
	emit(OP_POP);
	emit(OP_POP);
}

static void compile_switch(void) {
	advance();
	expect("(");
	expr();
	expect(")");
	expect("{");
	push_loop(1, 1);
	int failJump = emit_jump(OP_JMP), defaultBody = -1;
	while (!is_op("}")) {
		if (tok.t == TK_EOF) syntax_error();
		if (is_kw("case")) {
			advance();
			int skip = emit_jump(OP_JMP);
			patch(failJump);
			emit(OP_DUP);
			expr();
			emit(OP_EQ);
			failJump = emit_jump(OP_JZ);
			if (!accept(":")) expect(";");
			patch(skip);
		} else if (is_kw("default")) {
			advance();
			if (!accept(":")) expect(";");
			defaultBody = ncode;
		} else statement();
	}
	advance();
	int jEnd = emit_jump(OP_JMP);
	patch(failJump);
	if (defaultBody >= 0) emit2(OP_JMP, defaultBody);
	patch(jEnd);
	pop_loop(ncode, ncode);
	emit(OP_POP);
}

/* leaving try blocks (return, or break/continue to loop index targetLoop):
 * pop their handlers and run their finally blocks, innermost first */
static void leave_trys(int targetLoop) {
	for (int k = ntrys - 1; k >= tryBase && trys[k].nloops > targetLoop; k--) {
		TryCtx *t = &trys[k];
		if (t->active) emit(OP_ENDTRY);
		if (!t->inFinally) {
			if (t->njsr >= JUMPS_MAX) fail("Too many jumps");
			emit(OP_JSR);
			emit(0);
			t->jsr[t->njsr++] = ncode - 1;
		}
	}
}

static void jsr_finally(TryCtx *t) {
	if (t->njsr >= JUMPS_MAX) fail("Too many jumps");
	emit(OP_JSR);
	emit(0);
	t->jsr[t->njsr++] = ncode - 1;
}

static void block(void) {
	expect("{");
	while (!is_op("}")) {
		if (tok.t == TK_EOF) syntax_error();
		statement();
	}
	advance();
}

/* try { } catch (A | B $e) { } ... finally { }
 *
 *        TRY H1; try body; ENDTRY; JSR F; JMP END
 *   H1:  (exception on the stack) per catch: EXCMATCH..; JZ next;
 *        store/pop; TRY H2; catch body; ENDTRY; JSR F; JMP END
 *        no catch matched: JSR F; THROW
 *   H2:  exception in a catch body: JSR F; THROW
 *   F:   finally body; RETSUB
 */
static void compile_try(void) {
	advance();                             /* try */
	if (ntrys >= LOOPS_MAX) fail("try blocks nested too deeply");
	TryCtx *t = &trys[ntrys++];
	memset(t, 0, sizeof *t);
	t->nloops = nloops;
	int ends[JUMPS_MAX], nends = 0;
	int h1 = emit_jump(OP_TRY);
	t->active = 1;
	block();
	emit(OP_ENDTRY);
	t->active = 0;
	jsr_finally(t);
	ends[nends++] = emit_jump(OP_JMP);
	patch(h1);
	int h2s[JUMPS_MAX], nh2 = 0, ncatch = 0;
	while (is_kw("catch")) {
		advance();
		expect("(");
		int first = 1;
		do {
			if (tok.t != TK_ID) fail("catch needs a class name");
			emit2(OP_EXCMATCH, add_const(mk_cstr(tok.s)));
			emit(first ? 0 : 1);               /* the exception is below the result so far */
			if (!first) emit(OP_BOR);
			first = 0;
			advance();
		} while (accept("|"));
		int next = emit_jump(OP_JZ);
		if (tok.t == TK_VAR) {
			VarRef r = resolve_var(tok.s);
			advance();
			emit(OP_STORE); emit(r.scope); emit(r.slot); emit(0); emit(0);
		}
		emit(OP_POP);
		expect(")");
		if (nh2 >= JUMPS_MAX || nends >= JUMPS_MAX - 1) fail("Too many catch blocks");
		h2s[nh2++] = emit_jump(OP_TRY);
		t->active = 1;
		block();
		emit(OP_ENDTRY);
		t->active = 0;
		jsr_finally(t);
		ends[nends++] = emit_jump(OP_JMP);
		patch(next);
		ncatch++;
	}
	int hasFinally = is_kw("finally");
	if (!ncatch && !hasFinally) fail("try needs catch or finally");
	/* no catch matched: run finally, throw on */
	jsr_finally(t);
	emit(OP_THROW);
	for (int i = 0; i < nh2; i++) patch(h2s[i]);
	if (nh2) { jsr_finally(t); emit(OP_THROW); }
	/* the finally block as a subroutine */
	int f = ncode;
	t->inFinally = 1;
	if (hasFinally) { advance(); block(); }
	emit(OP_RETSUB);
	for (int i = 0; i < t->njsr; i++) code[t->jsr[i]] = f;
	for (int i = 0; i < nends; i++) patch(ends[i]);
	ntrys--;
}

static void compile_break(int isCont) {
	advance();
	int n = 1;
	if (tok.t == TK_INT) { n = tok.i; advance(); }
	end_stmt();
	if (n < 1 || n > nloops - loopBase) fail("'%s' not in loop", isCont ? "continue" : "break");
	leave_trys(nloops - n);
	Loop *target = &loops[nloops - n];
	for (int k = nloops - 1; k > nloops - n; k--)
		for (int s = 0; s < loops[k].slots; s++) emit(OP_POP);
	int j = emit_jump(OP_JMP);
	if (isCont && !target->isSwitch) {
		if (target->nc >= JUMPS_MAX) fail("Too many jumps");
		target->conts[target->nc++] = j;
	} else {
		if (target->nb >= JUMPS_MAX) fail("Too many jumps");
		target->breaks[target->nb++] = j;
	}
}

static int const_value(Value *out) {
	int neg = accept("-");
	if (tok.t == TK_INT) { *out = mk_int(neg ? -tok.i : tok.i); advance(); return 1; }
	if (tok.t == TK_FLOAT) { *out = mk_float(neg ? -tok.f : tok.f); advance(); return 1; }
	if (neg) return 0;
	if (tok.t == TK_STR) { *out = mk_str(tok.s, tok.len); advance(); return 1; }
	if (is_op("[") || (tok.t == TK_ID && !strcasecmp(tok.s, "array") && peek()->t == TK_OP && !strcmp(peek()->s, "("))) {
		const char *close = is_op("[") ? "]" : ")";
		if (!is_op("[")) advance();
		advance();
		Value a = mk_arr();
		while (!is_op(close)) {
			Value k;
			if (!const_value(&k)) { release(a); return 0; }
			if (accept("=>")) {
				Value v;
				if (!const_value(&v)) { release(k); release(a); return 0; }
				arr_set(a.u.a, k, v);
				release(k);
			} else arr_push(a.u.a, k);
			if (!accept(",")) break;
		}
		expect(close);
		*out = a;
		return 1;
	}
	if (tok.t == TK_ID) {
		if (!strcasecmp(tok.s, "true")) { *out = mk_bool(1); advance(); return 1; }
		if (!strcasecmp(tok.s, "false")) { *out = mk_bool(0); advance(); return 1; }
		if (!strcasecmp(tok.s, "null")) { *out = mk_null(); advance(); return 1; }
		if (find_constant(tok.s, out)) { advance(); return 1; }
	}
	return 0;
}

static void skip_type_hint(void) {
	accept("?");
	while (tok.t == TK_ID) {
		Tok *p = peek();
		if (p->t == TK_VAR || (p->t == TK_OP && (!strcmp(p->s, "|") || !strcmp(p->s, "...") || !strcmp(p->s, "&")))) {
			advance();
			if (!accept("|")) break;
		} else break;
	}
}

static void skip_return_type(void) {
	if (!accept(":")) return;
	accept("?");
	while (tok.t == TK_ID) { advance(); if (!accept("|")) break; }
}

/* parameters and body; promoteCls >= 0: constructor of that class, where
 * "public $x" parameters declare and set properties. hasBody: 0 abstract,
 * 1 { body }, 2 arrow function "=> expr", 3 closure with optional use (...) */
static void params_and_body(Func *fn, int promoteCls, int hasBody) {
	char promoted[PARAMS_MAX][NAME_MAX_LEN + 1];
	int npromoted = 0;
	fn->nparams = fn->nrequired = 0;
	expect("(");
	while (!is_op(")")) {
		int vis = -1;
		while (is_kw("public") || is_kw("protected") || is_kw("private") || is_kw("readonly")) {
			if (!is_kw("readonly")) vis = is_kw("public") ? VIS_PUBLIC : is_kw("protected") ? VIS_PROTECTED : VIS_PRIVATE;
			else if (vis < 0) vis = VIS_PUBLIC;
			advance();
		}
		if (vis >= 0 && promoteCls < 0) fail("Property promotion is only allowed in __construct");
		skip_type_hint();
		if (is_op("&")) fail("References are not supported");
		if (is_op("...")) fail("Variadic parameters are not supported");
		if (tok.t != TK_VAR) syntax_error();
		if (fn->nparams >= PARAMS_MAX) fail("Too many parameters");
		char pname[NAME_MAX_LEN + 1];
		strcpy(pname, tok.s);
		resolve_var(pname);
		advance();
		Value d = mk_null();
		if (accept("=")) { if (!const_value(&d)) fail("Default value must be a constant"); }
		else if (fn->nrequired != fn->nparams) fail("Required parameter after optional one");
		else fn->nrequired++;
		fn->defs[fn->nparams++] = d;
		if (vis >= 0) {
			Class *k = &classes[promoteCls];
			if (k->nown >= CPROPS_MAX) fail("Too many properties in %s", k->name);
			CProp *pr = &k->own[k->nown++];
			memset(pr, 0, sizeof *pr);
			strcpy(pr->name, pname);
			pr->vis = (unsigned char)vis;
			strcpy(promoted[npromoted++], pname);
		}
		if (!accept(",")) break;
	}
	expect(")");
	if (hasBody == 3 && is_kw("use")) {
		advance();
		expect("(");
		while (!is_op(")")) {
			if (is_op("&")) fail("References are not supported (use (&$x))");
			if (tok.t != TK_VAR) syntax_error();
			VarRef v = resolve_var(tok.s);
			if (v.scope != 0 || v.slot < fn->nparams) fail("Cannot use $%s twice", tok.s);
			for (int i = 0; i < scope->ncap; i++) if (scope->capSlot[i] == v.slot) fail("Cannot use $%s twice", tok.s);
			add_capture(tok.s, v.slot);
			advance();
			if (!accept(",")) break;
		}
		expect(")");
	}
	skip_return_type();
	if (!hasBody) { expect(";"); return; }
	if (hasBody == 2) {
		if (is_op("{")) fail("fn needs => expression (for a block use function () { })");
		expect("=>");
		scope->capturing = 1;
		expr_assign();
		emit(OP_RET);
		return;
	}
	expect("{");
	for (int i = 0; i < npromoted; i++) {           /* $this->x = $x; */
		VarRef v = resolve_var(promoted[i]);
		emit_const(mk_pname(promoted[i]));
		emit(OP_LOAD); emit(v.scope); emit(v.slot); emit(0);
		emit(OP_STORE); emit(2); emit(0); emit(1); emit(0);
		emit(OP_POP);
	}
	while (!is_op("}")) {
		if (tok.t == TK_EOF) syntax_error();
		statement();
	}
	advance();
	emit(OP_NULL);
	emit(OP_RET);
}

static void compile_function(void) {
	advance();                             /* function */
	if (tok.t != TK_ID) syntax_error();
	int f = func_index(tok.s, 1);
	if (funcs[f].addr >= 0) fail("Cannot redeclare %s()", tok.s);
	advance();
	int skip = emit_jump(OP_JMP);
	Scope sc;
	memset(&sc, 0, sizeof sc);
	sc.isMethod = 0;
	Scope *saved = scope;
	int savedLoopBase = loopBase, savedTryBase = tryBase;
	scope = &sc;
	loopBase = nloops;
	tryBase = ntrys;
	Func *fn = &funcs[f];
	fn->addr = ncode;                      /* allows recursion */
	params_and_body(fn, -1, 1);
	fn->nlocals = sc.n;
	scope = saved;
	loopBase = savedLoopBase;
	tryBase = savedTryBase;
	patch(skip);
}

/* function (...) use (...) { ... } or fn (...) => expr as a value */
static void compile_closure(int arrow) {
	advance();                             /* function / fn */
	accept("&");
	int f = new_func("{closure}");
	closuresMade++;
	int skip = emit_jump(OP_JMP);
	Scope sc;
	memset(&sc, 0, sizeof sc);
	sc.isMethod = scope ? scope->isMethod : 0;
	sc.arrow = arrow;
	Scope *saved = scope;
	int savedLoopBase = loopBase, savedTryBase = tryBase;
	scope = &sc;
	loopBase = nloops;
	tryBase = ntrys;
	Func *fn = &funcs[f];
	fn->addr = ncode;
	params_and_body(fn, -1, arrow ? 2 : 3);
	fn->nlocals = sc.n;
	fn->ncaps = sc.ncap;
	memcpy(fn->capSlot, sc.capSlot, sizeof sc.capSlot);
	scope = saved;
	loopBase = savedLoopBase;
	tryBase = savedTryBase;
	patch(skip);
	/* the captured values come from the surrounding scope */
	for (int i = 0; i < sc.ncap; i++) {
		VarRef v = resolve_var(sc.caps[i]);
		emit(OP_LOAD); emit(v.scope); emit(v.slot); emit(0);
	}
	emit(OP_CLOSURE); emit(f); emit(sc.ncap);
}

static void compile_method(int c, int vis, int isStatic, int isAbstract) {
	Class *k = &classes[c];
	advance();                             /* function */
	accept("&");
	if (tok.t != TK_ID) syntax_error();
	char mname[NAME_MAX_LEN + 1];
	strcpy(mname, tok.s);
	advance();
	for (int i = 0; i < k->nownM; i++)
		if (!strcasecmp(k->ownM[i].name, mname)) fail("Cannot redeclare %s::%s()", k->name, mname);
	if (k->nownM >= CMETH_MAX) fail("Too many methods in %s", k->name);
	if (k->kind == K_INTERFACE) isAbstract = 1;
	char display[NAME_MAX_LEN * 2 + 3];
	snprintf(display, sizeof display, "%s::%s", k->name, mname);
	int f = new_func(display);
	Func *fn = &funcs[f];
	int skip = isAbstract ? -1 : emit_jump(OP_JMP);
	Scope sc;
	memset(&sc, 0, sizeof sc);
	sc.isMethod = !isStatic;
	Scope *saved = scope;
	int savedLoopBase = loopBase, savedTryBase = tryBase;
	scope = &sc;
	loopBase = nloops;
	tryBase = ntrys;
	fn->addr = ncode;
	params_and_body(fn, !strcasecmp(mname, "__construct") ? c : -1, !isAbstract);
	fn->nlocals = sc.n;
	scope = saved;
	loopBase = savedLoopBase;
	tryBase = savedTryBase;
	if (skip >= 0) patch(skip);
	CMethod *m = &k->ownM[k->nownM++];
	memset(m, 0, sizeof *m);
	strcpy(m->name, mname);
	m->func = isAbstract ? -1 : f;
	m->vis = (unsigned char)vis;
	m->isStatic = (unsigned char)isStatic;
	m->isAbstract = (unsigned char)isAbstract;
}

static void name_list(char (*names)[NAME_MAX_LEN + 1], int *n) {
	do {
		if (tok.t != TK_ID) syntax_error();
		if (*n >= CLIST_MAX) fail("Too many names");
		strcpy(names[(*n)++], tok.s);
		advance();
	} while (accept(","));
}

static void compile_class(int kind, int isAbstract) {
	if (directCompile) fail("Classes can only be declared in the program, not in the REPL");
	advance();                             /* class / interface / trait */
	if (tok.t != TK_ID) syntax_error();
	int c = class_index(tok.s);
	Class *k = &classes[c];
	if (k->defined) fail("Cannot redeclare %s %s", kind_name(k->kind), tok.s);
	snprintf(k->name, sizeof k->name, "%.31s", tok.s);
	k->defined = 1;
	k->kind = kind;
	k->isAbstract = isAbstract;
	k->line = tok.line;
	advance();
	if (kind == K_CLASS && is_kw("extends")) {
		advance();
		if (tok.t != TK_ID) syntax_error();
		snprintf(k->parentName, sizeof k->parentName, "%.31s", tok.s);
		class_index(tok.s);
		advance();
	}
	if ((kind == K_CLASS && is_kw("implements")) || (kind == K_INTERFACE && is_kw("extends"))) {
		advance();
		name_list(k->ifaceNames, &k->nifaces);
	}
	expect("{");
	while (!is_op("}")) {
		if (tok.t == TK_EOF) syntax_error();
		if (is_kw("use")) {
			if (kind == K_INTERFACE) fail("Interfaces cannot use traits");
			advance();
			name_list(k->traitNames, &k->ntraits);
			if (is_op("{")) fail("Trait conflict resolution is not supported");
			expect(";");
			continue;
		}
		int vis = VIS_PUBLIC, isStatic = 0, isAbs = 0;
		for (;;) {
			if (is_kw("public")) vis = VIS_PUBLIC;
			else if (is_kw("protected")) vis = VIS_PROTECTED;
			else if (is_kw("private")) vis = VIS_PRIVATE;
			else if (is_kw("static")) isStatic = 1;
			else if (is_kw("abstract")) isAbs = 1;
			else if (!(is_kw("final") || is_kw("readonly") || is_kw("var"))) break;
			advance();
		}
		if (is_kw("const")) {
			advance();
			if (tok.t == TK_ID && peek()->t == TK_ID) advance();      /* typed constant */
			do {
				if (tok.t != TK_ID) syntax_error();
				if (k->nownC >= CCONST_MAX) fail("Too many constants in %s", k->name);
				CConst *cc = &k->ownC[k->nownC];
				snprintf(cc->name, sizeof cc->name, "%.31s", tok.s);
				advance();
				expect("=");
				if (!const_value(&cc->val)) fail("Constant value must be a constant expression");
				k->nownC++;
			} while (accept(","));
			expect(";");
			continue;
		}
		if (is_kw("function")) {
			if (isAbs && kind == K_CLASS && !k->isAbstract)
				fail("Class %s with abstract method must be declared abstract", k->name);
			compile_method(c, vis, isStatic, isAbs);
			continue;
		}
		if (kind == K_INTERFACE) fail("Interfaces may not include properties");
		skip_type_hint();
		do {
			if (tok.t != TK_VAR) syntax_error();
			for (int i = 0; i < k->nown; i++)
				if (!strcmp(k->own[i].name, tok.s)) fail("Cannot redeclare %s::$%s", k->name, tok.s);
			if (k->nown >= CPROPS_MAX) fail("Too many properties in %s", k->name);
			CProp *pr = &k->own[k->nown];
			memset(pr, 0, sizeof *pr);
			snprintf(pr->name, sizeof pr->name, "%.31s", tok.s);
			pr->vis = (unsigned char)vis;
			pr->isStatic = (unsigned char)isStatic;
			advance();
			if (accept("=") && !const_value(&pr->val)) fail("Property default must be a constant expression");
			k->nown++;
		} while (accept(","));
		expect(";");
	}
	advance();
}

static void statement(void) {
	if (accept(";")) return;
	if (accept("{")) {
		while (!is_op("}")) {
			if (tok.t == TK_EOF) syntax_error();
			statement();
		}
		advance();
		return;
	}
	if (tok.t == TK_ID) {
		if (is_kw("echo")) {
			advance();
			do { expr(); emit(OP_ECHO); } while (accept(","));
			end_stmt();
			return;
		}
		if (is_kw("if")) { compile_if(); return; }
		if (is_kw("while")) { compile_while(); return; }
		if (is_kw("do")) { compile_do(); return; }
		if (is_kw("for")) { compile_for(); return; }
		if (is_kw("foreach")) { compile_foreach(); return; }
		if (is_kw("switch")) { compile_switch(); return; }
		if (is_kw("break")) { compile_break(0); return; }
		if (is_kw("continue")) { compile_break(1); return; }
		if (is_kw("function") && peek()->t == TK_ID) { compile_function(); return; }
		if (is_kw("try")) { compile_try(); return; }
		if (is_kw("throw")) {
			advance();
			expr();
			end_stmt();
			emit(OP_THROW);
			return;
		}
		if (is_kw("return")) {
			advance();
			if (is_op(";") || tok.t == TK_EOF) emit(OP_NULL); else expr();
			end_stmt();
			leave_trys(-1);
			if (scope) emit(OP_RET);
			else { emit(OP_POP); emit(OP_END); }
			return;
		}
		if (is_kw("global")) {
			advance();
			do {
				if (tok.t != TK_VAR) syntax_error();
				if (scope) {
					if (scope->ng >= 16) fail("Too many globals");
					strcpy(scope->globals[scope->ng++], tok.s);
				}
				advance();
			} while (accept(","));
			end_stmt();
			return;
		}
		if (is_kw("unset")) {
			advance();
			expect("(");
			do {
				int n, app;
				VarRef r = parse_lvalue(&n, &app);
				if (app) fail("Cannot use [] here");
				emit(OP_UNSET); emit(r.scope); emit(r.slot); emit(n);
			} while (accept(","));
			expect(")");
			end_stmt();
			return;
		}
		if (is_kw("require") || is_kw("require_once") || is_kw("include") || is_kw("include_once")) {
			int once = strchr(tok.s, '_') != NULL, reqLine = tok.line;
			advance();
			int paren = accept("(");
			if (tok.t != TK_STR) fail("require needs a file name in quotes");
			char name[16];
			int n = 0;
			for (int i = 0; i < tok.len && n < 12; i++) name[n++] = (char)tolower((unsigned char)tok.s[i]);
			name[n] = 0;
			if (!strchr(name, '.') && n <= 8) strcat(name, ".php");
			advance();
			if (paren) expect(")");
			end_stmt();
			int f = find_src(name);
			if (f < 0) { errLine = reqLine; fail("File %s not found", name); }
			if (once && included[f]) return;
			included[f] = 1;
			compile_file(f);
			return;
		}
		if (is_kw("abstract") || is_kw("final") || (is_kw("readonly") && peek()->t == TK_ID)) {
			int abs = 0;
			while (is_kw("abstract") || is_kw("final") || is_kw("readonly")) { if (is_kw("abstract")) abs = 1; advance(); }
			if (!is_kw("class")) syntax_error();
			compile_class(K_CLASS, abs);
			return;
		}
		if (is_kw("class") && peek()->t == TK_ID) { compile_class(K_CLASS, 0); return; }
		if (is_kw("interface") && peek()->t == TK_ID) { compile_class(K_INTERFACE, 0); return; }
		if (is_kw("trait") && peek()->t == TK_ID) { compile_class(K_TRAIT, 0); return; }
		if (is_kw("const")) {
			advance();
			do {
				if (tok.t != TK_ID) syntax_error();
				char cname[NAME_MAX_LEN + 1];
				strcpy(cname, tok.s);
				advance();
				expect("=");
				Value v;
				if (!const_value(&v)) fail("Constant value must be a constant expression");
				define_user_const(cname, v);
			} while (accept(","));
			end_stmt();
			return;
		}
		if ((is_kw("static") && !(peek()->t == TK_OP && !strcmp(peek()->s, "::"))) || is_kw("namespace") ||
		    is_kw("try"))
			fail("'%s' is not supported", tok.s);
	}
	expr();
	emit(OP_POP);
	end_stmt();
}

/* source text of a project file (lines joined by '\n'); buffers are kept
 * until the next compile so that errors (longjmp) do not leak them */
static char *srcBufs[MAX_SRC + 1];
static int   nsrcBufs, includeDepth;
static char included[MAX_SRC];

static void free_src_bufs(void) {
	while (nsrcBufs > 0) free(srcBufs[--nsrcBufs]);
}

static const char *file_source(int f) {
	size_t total = 1;
	for (int i = 0; i < fileCount[f]; i++) total += strlen(fileLines[f][i].text) + 1;
	if (nsrcBufs > MAX_SRC) fail("Files included too often");
	char *src = malloc(total);
	if (!src) fail("Out of memory");
	srcBufs[nsrcBufs++] = src;
	char *p = src;
	for (int i = 0; i < fileCount[f]; i++) {
		size_t l = strlen(fileLines[f][i].text);
		memcpy(p, fileLines[f][i].text, l);
		p += l;
		*p++ = '\n';
	}
	*p = 0;
	return src;
}

static int find_src(const char *name) {
	for (int i = 0; i < nsrc; i++) if (!strcasecmp(fileNames[i], name)) return i;
	return -1;
}

/* compile the statements of another project file right here (require) */
static void compile_file(int f) {
	const char *sSrc = lsrc;
	int sPos = lpos, sLine = lline, sFile = lexFile, sFixed = fixedLine, sHave = havePeek;
	Tok sTok = tok, sPeek = peekTok;
	if (++includeDepth > 8) fail("Files included too deeply");
	lsrc = file_source(f);
	lpos = 0; lline = 0; lexFile = f; fixedLine = -2; havePeek = 0;
	advance();
	while (tok.t != TK_EOF) statement();
	includeDepth--;
	lsrc = sSrc; lpos = sPos; lline = sLine; lexFile = sFile; fixedLine = sFixed; havePeek = sHave;
	tok = sTok; peekTok = sPeek;
}

/* compile src (file = project file index or -1 for a REPL line) */
static int compile_source(const char *src, int file) {
	int start = ncode;
	lsrc = src;
	lpos = 0;
	lline = 0;
	lexFile = file;
	fixedLine = compilingPrelude ? 0 : -2;   /* prelude code has no line numbers */
	havePeek = 0;
	scope = NULL;
	nloops = loopBase = 0;
	ntrys = tryBase = 0;
	includeDepth = 0;
	if (file >= 0) { memset(included, 0, sizeof included); included[file] = 1; }
	advance();
	while (tok.t != TK_EOF) statement();
	emit(OP_END);
	for (int i = 0; i < nfuncs; i++)
		if (funcs[i].addr < 0) {
			errLine = funcs[i].firstCallLine;
			fail("Call to undefined function %s()", funcs[i].name);
		}
	link_all();
	return start;
}

/* ------------------------------------------------------------------ */
/* virtual machine                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	int   retpc, base, func;
	Value thisv;                  /* $this or null                       */
	short cls, scls;              /* class context (self), called class (static) */
	char  ctor;                   /* constructor: returns $this          */
} Frame;

static Value stack[STACK_MAX];
static int   sp;
static Frame frames[FRAMES_MAX];
static int   nframes;
static int   pc, curPc;
static Value *refArg;               /* by-reference argument of builtins   */

/* exception handlers installed by OP_TRY */
#define HANDLERS_MAX 64
typedef struct { int pc, nframes, sp, nscratch; } Handler;
static Handler handlers[HANDLERS_MAX];
static int     nhandlers;
static Value   pendingExc;                 /* thrown, not caught yet       */
static int     excPending;
static int     throwableClass = -1;        /* class index of Throwable     */

static void push(Value v) {
	if (sp >= STACK_MAX) { release(v); fail("Stack overflow"); }
	stack[sp++] = v;
}
static Value pop(void) { return stack[--sp]; }

/* temporaries of builtins that run callbacks: freed by vm_reset when a
 * callback fails, popped by the builtin when it finishes */
#define SCRATCH_MAX 16
static struct { void *p; Value v; } scratch[SCRATCH_MAX];
static int nscratch;

static void scratch_push(void *p, Value v) {
	if (nscratch >= SCRATCH_MAX) { free(p); release(v); fail("Callbacks nested too deeply"); }
	scratch[nscratch].p = p;
	scratch[nscratch++].v = v;
}
static void scratch_pop(int n) { nscratch -= n; }

static void vm_reset(void) {
	nhandlers = 0;
	if (excPending) { release(pendingExc); excPending = 0; }
	while (nscratch > 0) { nscratch--; free(scratch[nscratch].p); release(scratch[nscratch].v); }
	while (sp > 0) release(stack[--sp]);
	while (nframes > 0) { nframes--; release(frames[nframes].thisv); frames[nframes].thisv = mk_null(); }
}

static const char *type_name(Value v) {
	switch (v.t) {
	case T_NULL: return "null";
	case T_BOOL: return "bool";
	case T_INT: return "int";
	case T_FLOAT: return "float";
	case T_STR: return "string";
	case T_ARR: return "array";
	case T_OBJ: return classes[v.u.o->cls].name;
	case T_CLO: return "Closure";
	default: return "value";
	}
}

static const char *vis_name(int v) { return v == VIS_PRIVATE ? "private" : v == VIS_PROTECTED ? "protected" : "public"; }

static int resolve_cref(int cr) {
	Frame *f = &frames[nframes - 1];
	if (cr >= 0) return cr;
	if (cr == CREF_SELF) { if (f->cls < 0) fail("Cannot use \"self\" outside a class"); return f->cls; }
	if (cr == CREF_STATIC) { if (f->scls < 0) fail("Cannot use \"static\" outside a class"); return f->scls; }
	if (f->cls < 0 || classes[f->cls].parent < 0) fail("Cannot use \"parent\" without a parent class");
	return classes[f->cls].parent;
}

/* visibility of a member declared in class decl, seen from the running code */
static int can_access(int vis, int decl) {
	if (vis == VIS_PUBLIC) return 1;
	int c = nframes ? frames[nframes - 1].cls : -1;
	if (c < 0) return 0;
	if (vis == VIS_PRIVATE) return c == decl;
	return is_a(c, decl) || is_a(decl, c);
}

/* property slot of an object (declared or dynamic); NULL if missing and !create */
static Value *obj_prop(Obj *o, Value pname, int create) {
	const char *name = pname.u.s->s;
	int i = find_prop(o->cls, name);
	if (i >= 0) {
		CProp *d = &classes[o->cls].props[i];
		if (!can_access(d->vis, d->decl))
			fail("Cannot access %s property %s::$%s", vis_name(d->vis), classes[o->cls].name, name);
		return &o->props[i];
	}
	Value key = pname;
	key.t = T_STR;
	if (o->dyn) {
		Value *p = arr_slot(o->dyn, key, 0);
		if (p || !create) return p;
	}
	if (!create) return NULL;
	if (!o->dyn) o->dyn = arr_alloc();
	return arr_slot(o->dyn, key, 1);
}

static Value *static_prop(int cr, int slot) {
	int c = resolve_cref(cr);
	const char *name = consts[slot].u.s->s;
	for (int k = c; k >= 0; k = classes[k].parent)
		for (int i = 0; i < classes[k].nown; i++) {
			CProp *p = &classes[k].own[i];
			if (!p->isStatic || strcmp(p->name, name)) continue;
			if (!can_access(p->vis, k)) fail("Cannot access %s property %s::$%s", vis_name(p->vis), classes[k].name, name);
			return &p->val;
		}
	fail("Access to undeclared static property %s::$%s", classes[c].name, name);
}

static Value *var_ptr(int sc, int slot) {
	if (sc == 0) return &stack[frames[nframes - 1].base + slot];
	if (sc == 1) return &globals[slot];
	if (sc == 2) {
		Value *t = &frames[nframes - 1].thisv;
		if (t->t != T_OBJ) fail("Using $this when not in object context");
		return t;
	}
	return static_prop(sc - SC_STATIC - 3, slot);
}

/* read access: returns an owned value, missing keys give null */
static Value index_get(Value c, Value k) {
	if (k.t == T_PNAME) {
		if (c.t != T_OBJ) return mk_null();           /* PHP: warning, null */
		Value *p = obj_prop(c.u.o, k, 0);
		if (!p) return mk_null();
		retain(*p);
		return *p;
	}
	if (c.t == T_OBJ) fail("Cannot use object of type %s as array", classes[c.u.o->cls].name);
	if (c.t == T_ARR) {
		if (k.t == T_APPEND) fail("Cannot use [] for reading");
		Value *p = arr_slot(c.u.a, k, 0);
		if (p) { retain(*p); return *p; }
		return mk_null();
	}
	if (c.t == T_STR) {
		int i = to_int(k), len = c.u.s->len;
		if (i < 0) i += len;
		if (i >= 0 && i < len) return mk_str(c.u.s->s + i, 1);
		return mk_str("", 0);
	}
	return mk_null();
}

/* write access: walks/creates the keys, returns the slot */
static Value *lval_slot(int sc, int slot, Value *keys, int n) {
	Value *p = var_ptr(sc, slot);
	for (int i = 0; i < n; i++) {
		if (keys[i].t == T_PNAME) {
			if (p->t != T_OBJ) fail("Attempt to assign property \"%s\" on %s", keys[i].u.s->s, type_name(*p));
			p = obj_prop(p->u.o, keys[i], 1);
			continue;
		}
		if (p->t == T_OBJ) fail("Cannot use object of type %s as array", classes[p->u.o->cls].name);
		if (p->t == T_NULL) *p = mk_arr();
		else if (p->t == T_STR && p->u.s->len == 0) { release(*p); *p = mk_arr(); }
		else if (p->t != T_ARR) fail("Cannot use a scalar value as an array");
		separate(p);
		p = keys[i].t == T_APPEND ? arr_append_slot(p->u.a) : arr_slot(p->u.a, keys[i], 1);
	}
	return p;
}

static void pop_n(int n) { while (n-- > 0) release(pop()); }

static Value arith(int op, Value a, Value b) {
	Value x = to_num(a), y = to_num(b);
	int bothInt = x.t == T_INT && y.t == T_INT;
	switch (op) {
	case OP_ADD: case OP_SUB: case OP_MUL:
		if (bothInt) {
			long long r = op == OP_ADD ? (long long)x.u.i + y.u.i :
			              op == OP_SUB ? (long long)x.u.i - y.u.i : (long long)x.u.i * y.u.i;
			if (r >= -2147483647LL - 1 && r <= 2147483647LL) return mk_int((int)r);
			return mk_float((float)r);
		}
		{
			float fx = to_float(x), fy = to_float(y);
			return mk_float(op == OP_ADD ? fx + fy : op == OP_SUB ? fx - fy : fx * fy);
		}
	case OP_DIV:
		if (to_float(y) == 0) fail("Division by zero");
		if (bothInt && x.u.i % y.u.i == 0) return mk_int(x.u.i / y.u.i);
		return mk_float(to_float(x) / to_float(y));
	case OP_MOD: {
		int iy = to_int(y);
		if (iy == 0) fail("Modulo by zero");
		if (iy == -1) return mk_int(0);
		return mk_int(to_int(x) % iy);
	}
	case OP_POW:
		if (bothInt && y.u.i >= 0) {
			long long r = 1;
			int e = y.u.i;
			while (e-- > 0) {
				r *= x.u.i;
				if (r > 2147483647LL || r < -2147483647LL - 1) return mk_float(powf((float)x.u.i, (float)y.u.i));
			}
			return mk_int((int)r);
		}
		return mk_float(powf(to_float(x), to_float(y)));
	}
	return mk_null();
}

static Value concat(Value a, Value b) {
	int la, lb;
	const char *sa = sval(a, &la), *sb = sval(b, &lb);
	Value r;
	r.t = T_STR;
	r.u.s = str_alloc(la + lb);
	memcpy(r.u.s->s, sa, la);
	memcpy(r.u.s->s + la, sb, lb);
	return r;
}

/* any binary operator (a, b borrowed, result owned) */
static Value binop(int op, Value a, Value b) {
	switch (op) {
	case OP_ADD:
		if (a.t == T_ARR && b.t == T_ARR) {          /* array union */
			Value r; r.t = T_ARR; r.u.a = arr_clone(a.u.a);
			for (int i = 0; i < b.u.a->n; i++)
				if (arr_find(r.u.a, b.u.a->e[i].key) < 0) {
					retain(b.u.a->e[i].val);
					arr_set(r.u.a, b.u.a->e[i].key, b.u.a->e[i].val);
				}
			return r;
		}
		return arith(op, a, b);
	case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: return arith(op, a, b);
	case OP_CONCAT: return concat(a, b);
	case OP_EQ: return mk_bool(loose_cmp(a, b) == 0);
	case OP_NE: return mk_bool(loose_cmp(a, b) != 0);
	case OP_IDENT: return mk_bool(strict_eq(a, b));
	case OP_NIDENT: return mk_bool(!strict_eq(a, b));
	case OP_LT: return mk_bool(loose_cmp(a, b) < 0);
	case OP_GT: return mk_bool(loose_cmp(a, b) > 0);
	case OP_LE: return mk_bool(loose_cmp(a, b) <= 0);
	case OP_GE: return mk_bool(loose_cmp(a, b) >= 0);
	case OP_SPACESHIP: { int c = loose_cmp(a, b); return mk_int(c < 0 ? -1 : c > 0); }
	case OP_BAND: return mk_int(to_int(a) & to_int(b));
	case OP_BOR: return mk_int(to_int(a) | to_int(b));
	case OP_BXOR: return mk_int(to_int(a) ^ to_int(b));
	case OP_SHL: return mk_int((int)((unsigned)to_int(a) << (to_int(b) & 31)));
	case OP_SHR: return mk_int(to_int(a) >> (to_int(b) & 31));
	case OP_LXOR: return mk_bool(to_bool(a) != to_bool(b));
	case OP_LAND: return mk_bool(to_bool(a) && to_bool(b));
	}
	return mk_null();
}

static Value cast(Value v, int t) {
	switch (t) {
	case T_INT: return mk_int(to_int(v));
	case T_FLOAT: return mk_float(to_float(v));
	case T_BOOL: return mk_bool(to_bool(v));
	case T_STR: return to_str_val(v);
	default:
		if (v.t == T_ARR) { retain(v); return v; }
		{
			Value a = mk_arr();
			if (v.t != T_NULL) { retain(v); arr_push(a.u.a, v); }
			return a;
		}
	}
}

typedef Value (*BuiltinFn)(Value *a, int n);
static BuiltinFn builtin_fn(int b);

static void enter_func(int f, int argc, Value thisv, int cls, int scls, int ctor) {
	Func *fn = &funcs[f];
	if (argc < fn->nrequired) { release(thisv); fail("Too few arguments to %s()", fn->name); }
	while (argc > fn->nparams) { release(pop()); argc--; }
	int base = sp - argc;
	for (int i = argc; i < fn->nparams; i++) { retain(fn->defs[i]); push(fn->defs[i]); }
	for (int i = fn->nparams; i < fn->nlocals; i++) push(mk_null());
	if (nframes >= FRAMES_MAX) { release(thisv); fail("Too much recursion"); }
	Frame *fr = &frames[nframes++];
	fr->retpc = pc;
	fr->base = base;
	fr->func = f;
	fr->thisv = thisv;
	fr->cls = (short)cls;
	fr->scls = (short)scls;
	fr->ctor = (char)ctor;
	pc = fn->addr;
}

static void exec_one(void);

/* the value below argc arguments is called: closure, function name
 * ("strlen", "myfunc", "Class::method"), [$obj, "method"], ["Class", "method"]
 * or an object with __invoke. User code gets a frame, builtins run at once
 * and leave their result. */
static void invoke(int argc) {
	Value *cv = &stack[sp - argc - 1];
	Value callee = *cv;                        /* we own this reference now */
	memmove(cv, cv + 1, argc * sizeof(Value));
	sp--;
	if (callee.t == T_CLO) {
		Clo *c = callee.u.c;
		Func *fn = &funcs[c->func];
		retain(c->thisv);
		enter_func(c->func, argc, c->thisv, c->cls, c->scls, 0);
		Value *loc = &stack[frames[nframes - 1].base];
		for (int i = 0; i < c->nuse; i++) {
			release(loc[fn->capSlot[i]]);
			loc[fn->capSlot[i]] = c->use[i];
			retain(c->use[i]);
		}
		release(callee);
		return;
	}
	if (callee.t == T_STR) {
		char name[NAME_MAX_LEN * 2 + 4];
		snprintf(name, sizeof name, "%s", callee.u.s->s);
		char *dc = strstr(name, "::");
		if (dc) {                              /* "Class::method" */
			*dc = 0;
			Value arr = mk_arr();
			arr_push(arr.u.a, mk_cstr(name));
			arr_push(arr.u.a, mk_cstr(dc + 2));
			release(callee);
			callee = arr;
		} else {
			int f = func_index(name, 0);
			if (f >= 0 && funcs[f].addr >= 0) {
				release(callee);
				enter_func(f, argc, mk_null(), -1, -1, 0);
				return;
			}
			int b = find_builtin(name);
			release(callee);
			if (b < 0) fail("Call to undefined function %s()", name);
			int mn, mx;
			builtin_arity(b, &mn, &mx);
			if (builtin_takes_ref(b)) fail("%s() cannot be called this way", name);
			if (argc < mn || (mx >= 0 && argc > mx)) fail("Wrong argument count for %s()", name);
			Value r = builtin_fn(b)(&stack[sp - argc], argc);
			pop_n(argc);
			push(r);
			return;
		}
	}
	Value thisv = mk_null();
	const char *mname = "__invoke";
	int c = -1, viaClass = 0;
	if (callee.t == T_OBJ) {
		thisv = callee;
		c = callee.u.o->cls;
	} else if (callee.t == T_ARR && callee.u.a->n == 2 && callee.u.a->e[1].val.t == T_STR) {
		Value t = callee.u.a->e[0].val;
		mname = callee.u.a->e[1].val.u.s->s;
		if (t.t == T_OBJ) { thisv = t; retain(t); c = t.u.o->cls; }
		else if (t.t == T_STR) {
			c = find_class(t.u.s->s);
			if (c < 0 || !classes[c].defined) { release(callee); fail("Class %s not found", t.u.s->s); }
			viaClass = 1;
		}
	}
	if (c < 0) { fail("Value of type %s is not callable", type_name(callee)); }
	CMethod *m = find_method(c, mname);
	if (!m || m->func < 0 || !can_access(m->vis, m->decl)) {
		char msg[80];
		snprintf(msg, sizeof msg, "Cannot call %s::%s()", classes[c].name, mname);
		release(thisv);
		if (callee.t != T_OBJ) release(callee);
		fail("%s", msg);
	}
	if (callee.t != T_OBJ) release(callee);
	if (m->isStatic) { release(thisv); thisv = mk_null(); }
	else if (viaClass) fail("Non-static method %s::%s() cannot be called statically", classes[c].name, m->name);
	enter_func(m->func, argc, thisv, m->decl, c, 0);
}

/* --- exceptions --- */

static void throw_value(Value exc) __attribute__((noreturn));
static void throw_value(Value exc) {
	if (excPending) release(pendingExc);
	pendingExc = exc;
	excPending = 1;
	errIsParse = 0;
	longjmp(errjmp, 2);
}

static void set_prop(Value o, const char *name, Value v) {
	int i = find_prop(o.u.o->cls, name);
	if (i < 0) { release(v); return; }
	release(o.u.o->props[i]);
	o.u.o->props[i] = v;
}

/* line and file of the running code into a new Throwable */
static void set_exc_origin(Value o) {
	int line = curPc >= 0 && curPc < ncode ? codeLine[curPc] : 0;
	set_prop(o, "line", mk_int(line > 0 ? (line & 1023) : 0));
	int f = line > 0 ? line >> 10 : 0;
	set_prop(o, "file", mk_cstr(f < nsrc ? fileNames[f] : ""));
}

/* runtime error (fail) as an Error object, like PHP 8 */
static Value error_object(const char *msg) {
	const char *cn = "Error";
	if (!strncmp(msg, "Division by zero", 16) || !strncmp(msg, "Modulo by zero", 14)) cn = "DivisionByZeroError";
	else if (!strncmp(msg, "Too few arguments", 17) || strstr(msg, "argument count")) cn = "ArgumentCountError";
	else if (!strncmp(msg, "Unsupported operand", 19) || strstr(msg, "expects")) cn = "TypeError";
	int c = find_class(cn);
	if (c < 0) c = find_class("Error");
	Value o = new_object(c);
	set_prop(o, "message", mk_cstr(msg));
	set_exc_origin(o);
	return o;
}

/* after a longjmp: catch the pending exception (or a runtime error turned
 * into an Error) with the innermost handler, if that handler belongs to
 * frames >= minFrames. Returns 1 when the VM continues at the handler. */
static int handle_throw(int minFrames) {
	if (nhandlers == 0 || handlers[nhandlers - 1].nframes < minFrames) return 0;
	if (!excPending) {
		if (compiling || errIsParse || state != BASIC_RUNNING || throwableClass < 0) return 0;
		pendingExc = error_object(errText);
		excPending = 1;
	}
	Handler h = handlers[--nhandlers];
	while (nframes > h.nframes) { Frame *fr = &frames[--nframes]; release(fr->thisv); fr->thisv = mk_null(); }
	while (sp > h.sp) release(pop());
	while (nscratch > h.nscratch) { nscratch--; free(scratch[nscratch].p); release(scratch[nscratch].v); }
	refArg = NULL;
	excPending = 0;
	push(pendingExc);
	pc = h.pc;
	return 1;
}

/* run the frames above depth to completion (callbacks, __toString);
 * exceptions are caught inside or passed on to the code around */
static void run_nested(int depth, const char *what) {
	jmp_buf outer;
	memcpy(outer, errjmp, sizeof outer);
	int jc = setjmp(errjmp);
	if (jc && !handle_throw(depth + 1)) {
		memcpy(errjmp, outer, sizeof outer);
		longjmp(errjmp, jc);
	}
	while (nframes > depth) {
		if (state != BASIC_RUNNING) { memcpy(errjmp, outer, sizeof outer); fail("input() is not allowed in %s", what); }
		exec_one();
	}
	memcpy(errjmp, outer, sizeof outer);
	if (state != BASIC_RUNNING) fail("The program ended inside %s", what);
}

/* call a callable with n arguments to completion right now (builtins such
 * as usort and array_map); returns the result */
static Value call_now(Value callee, Value *args, int n) {
	int savedPc = pc, savedCur = curPc, depth = nframes;
	retain(callee);
	push(callee);
	for (int i = 0; i < n; i++) { retain(args[i]); push(args[i]); }
	invoke(n);
	run_nested(depth, "a callback");
	pc = savedPc;
	curPc = savedCur;
	return pop();
}

/* run a method to completion right now (used for __toString) */
static Value call_method_now(CMethod *m, Value thisv) {
	int savedPc = pc, savedCur = curPc, depth = nframes;
	retain(thisv);
	enter_func(m->func, 0, thisv, m->decl, thisv.u.o->cls, 0);
	frames[nframes - 1].retpc = -1;
	run_nested(depth, m->name);
	pc = savedPc;
	curPc = savedCur;
	return pop();
}

static Value obj_to_str(Value v) {
	CMethod *m = find_method(v.u.o->cls, "__toString");
	if (!m || m->func < 0) fail("Object of class %s could not be converted to string", classes[v.u.o->cls].name);
	if (state != BASIC_RUNNING || nframes == 0) fail("Object of class %s could not be converted to string", classes[v.u.o->cls].name);
	Value r = call_method_now(m, v);
	if (r.t != T_STR) { release(r); fail("%s::__toString() must return a string", classes[v.u.o->cls].name); }
	return r;
}

static void end_program(void) {
	state = BASIC_IDLE;
	vm_reset();
}

static void exec_one(void) {
	curPc = pc;
	int op = code[pc++];
	Value a, b, r;
	switch (op) {
	case OP_END: end_program(); return;
	case OP_CONST: r = consts[code[pc++]]; retain(r); push(r); return;
	case OP_INT: push(mk_int(code[pc++])); return;
	case OP_NULL: push(mk_null()); return;
	case OP_TRUE: push(mk_bool(1)); return;
	case OP_FALSE: push(mk_bool(0)); return;
	case OP_POP: release(pop()); return;
	case OP_DUP: r = stack[sp - 1]; retain(r); push(r); return;
	case OP_APPENDKEY: r.t = T_APPEND; r.u.i = 0; push(r); return;
	case OP_LOAD: {
		int sc = code[pc++], slot = code[pc++], n = code[pc++];
		Value cur = *var_ptr(sc, slot);
		retain(cur);
		for (int i = 0; i < n; i++) {
			Value nv = index_get(cur, stack[sp - n + i]);
			release(cur);
			cur = nv;
		}
		pop_n(n);
		push(cur);
		return;
	}
	case OP_STORE: {
		int sc = code[pc++], slot = code[pc++], n = code[pc++], bop = code[pc++];
		Value rhs = pop();
		Value *p = lval_slot(sc, slot, &stack[sp - n], n);
		if (bop) {
			r = binop(bop, *p, rhs);
			release(rhs);
		} else r = rhs;
		release(*p);
		*p = r;
		retain(r);
		pop_n(n);
		push(r);
		return;
	}
	case OP_INCDEC: {
		int sc = code[pc++], slot = code[pc++], n = code[pc++], mode = code[pc++];
		Value *p = lval_slot(sc, slot, &stack[sp - n], n);
		Value old = *p;
		if (old.t == T_NULL) r = (mode & 1) ? mk_null() : mk_int(1);
		else r = arith((mode & 1) ? OP_SUB : OP_ADD, old, mk_int(1));
		*p = r;
		pop_n(n);
		if (mode & 2) push(old);
		else { release(old); retain(r); push(r); }
		return;
	}
	case OP_ISSET: case OP_UNSET: {
		int sc = code[pc++], slot = code[pc++], n = code[pc++];
		Value *p = var_ptr(sc, slot);
		if (op == OP_UNSET) {
			if (n == 0) { release(*p); *p = mk_null(); return; }
			for (int i = 0; i < n - 1 && p; i++) {
				Value k = stack[sp - n + i];
				if (k.t == T_PNAME) p = p->t == T_OBJ ? obj_prop(p->u.o, k, 0) : NULL;
				else if (p->t != T_ARR) p = NULL;
				else { separate(p); p = arr_slot(p->u.a, k, 0); }
			}
			Value last = stack[sp - 1];
			if (p && last.t == T_PNAME && p->t == T_OBJ) {
				Obj *o = p->u.o;
				if (find_prop(o->cls, last.u.s->s) >= 0) {
					Value *q = obj_prop(o, last, 0);
					release(*q);
					*q = mk_null();
				} else if (o->dyn) {
					Value key = last;
					key.t = T_STR;
					Value nk = norm_key(key);
					int i = arr_find(o->dyn, nk);
					release(nk);
					if (i >= 0) arr_remove_at(o->dyn, i);
				}
			} else if (p && last.t != T_PNAME && p->t == T_ARR) {
				separate(p);
				Value nk = norm_key(last);
				int i = arr_find(p->u.a, nk);
				release(nk);
				if (i >= 0) arr_remove_at(p->u.a, i);
			}
			pop_n(n);
			return;
		}
		Value cur = *p;
		retain(cur);
		for (int i = 0; i < n; i++) {
			Value nv = cur.t == T_ARR || cur.t == T_STR || cur.t == T_OBJ ? index_get(cur, stack[sp - n + i]) : mk_null();
			if (cur.t == T_STR) {
				int k = to_int(stack[sp - n + i]);
				if (k < 0 || k >= cur.u.s->len) { release(nv); nv = mk_null(); }
			}
			release(cur);
			cur = nv;
		}
		pop_n(n);
		r = mk_bool(cur.t != T_NULL);
		release(cur);
		push(r);
		return;
	}
	case OP_INDEX:
		b = pop(); a = pop();
		r = index_get(a, b);
		release(a); release(b);
		push(r);
		return;
	case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_CONCAT:
	case OP_EQ: case OP_NE: case OP_IDENT: case OP_NIDENT: case OP_LT: case OP_GT: case OP_LE:
	case OP_GE: case OP_SPACESHIP: case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
	case OP_LXOR: case OP_LAND:
		b = pop(); a = pop();
		r = binop(op, a, b);
		release(a); release(b);
		push(r);
		return;
	case OP_NEG:
		a = pop();
		r = to_num(a);
		if (r.t == T_INT) r = r.u.i == (-2147483647 - 1) ? mk_float(2147483648.0f) : mk_int(-r.u.i);
		else r.u.f = -r.u.f;
		release(a);
		push(r);
		return;
	case OP_PLUS: a = pop(); r = to_num(a); release(a); push(r); return;
	case OP_NOT: a = pop(); r = mk_bool(!to_bool(a)); release(a); push(r); return;
	case OP_BNOT: a = pop(); r = mk_int(~to_int(a)); release(a); push(r); return;
	case OP_TOBOOL: a = pop(); r = mk_bool(to_bool(a)); release(a); push(r); return;
	case OP_CAST: a = pop(); r = cast(a, code[pc++]); release(a); push(r); return;
	case OP_JMP: pc = code[pc]; return;
	case OP_JZ: case OP_JNZ:
		a = pop();
		if (to_bool(a) == (op == OP_JNZ)) pc = code[pc]; else pc++;
		release(a);
		return;
	case OP_JZK: case OP_JNZK:
		if (to_bool(stack[sp - 1]) == (op == OP_JNZK)) pc = code[pc]; else pc++;
		return;
	case OP_JNNK:
		if (stack[sp - 1].t != T_NULL) pc = code[pc]; else pc++;
		return;
	case OP_ECHO: a = pop(); out_val(a); release(a); return;
	case OP_ARR_NEW: push(mk_arr()); return;
	case OP_ARR_PUSH: a = pop(); arr_push(stack[sp - 1].u.a, a); return;
	case OP_ARR_SET:
		b = pop(); a = pop();
		arr_set(stack[sp - 1].u.a, a, b);
		release(a);
		return;
	case OP_FE_INIT:
		a = pop();
		if (a.t == T_OBJ) {                        /* visible properties */
			Obj *o = a.u.o;
			Value arr = mk_arr();
			for (int i = 0; i < o->nprops; i++) {
				CProp *d = &classes[o->cls].props[i];
				if (!can_access(d->vis, d->decl)) continue;
				retain(o->props[i]);
				Value key = mk_cstr(d->name);
				arr_set(arr.u.a, key, o->props[i]);
				release(key);
			}
			if (o->dyn)
				for (int i = 0; i < o->dyn->n; i++) {
					retain(o->dyn->e[i].val);
					arr_set(arr.u.a, o->dyn->e[i].key, o->dyn->e[i].val);
				}
			release(a);
			a = arr;
		}
		if (a.t != T_ARR) { release(a); a = mk_arr(); }
		push(a);
		push(mk_int(0));
		return;
	case OP_FE_NEXT: {
		Value *arr = &stack[sp - 2];
		int i = stack[sp - 1].u.i;
		if (i >= arr->u.a->n) { pc = code[pc]; return; }
		pc++;
		stack[sp - 1].u.i = i + 1;
		Entry *e = &arr->u.a->e[i];
		retain(e->key);
		retain(e->val);
		push(e->key);
		push(e->val);
		return;
	}
	case OP_CALL: {
		int f = code[pc++], argc = code[pc++];
		enter_func(f, argc, mk_null(), -1, -1, 0);
		return;
	}
	case OP_RET: {
		r = pop();
		Frame *fr = &frames[--nframes];
		while (nhandlers > 0 && handlers[nhandlers - 1].nframes > nframes) nhandlers--;
		while (sp > fr->base) release(pop());
		if (fr->ctor) { release(r); r = fr->thisv; }
		else release(fr->thisv);
		fr->thisv = mk_null();
		push(r);
		pc = fr->retpc;
		return;
	}
	case OP_NEW: {
		int cr = code[pc++], argc = code[pc++];
		int c = resolve_cref(cr);
		Class *k = &classes[c];
		if (k->kind != K_CLASS) fail("Cannot instantiate %s %s", kind_name(k->kind), k->name);
		if (k->isAbstract) fail("Cannot instantiate abstract class %s", k->name);
		Value o = new_object(c);
		if (throwableClass >= 0 && is_a(c, throwableClass)) set_exc_origin(o);
		CMethod *m = find_method(c, "__construct");
		if (m && m->func >= 0) {
			if (!can_access(m->vis, m->decl)) {
				release(o);
				fail("Call to %s %s::__construct()", vis_name(m->vis), k->name);
			}
			enter_func(m->func, argc, o, m->decl, c, 1);
		} else {
			pop_n(argc);
			push(o);
		}
		return;
	}
	case OP_MCALL: {
		int ni = code[pc++], argc = code[pc++];
		const char *name = consts[ni].u.s->s;
		Value *ov = &stack[sp - argc - 1];
		if (ov->t != T_OBJ) fail("Call to a member function %s() on %s", name, type_name(*ov));
		int c = ov->u.o->cls;
		CMethod *m = find_method(c, name);
		if (!m) fail("Call to undefined method %s::%s()", classes[c].name, name);
		if (!can_access(m->vis, m->decl))
			fail("Call to %s method %s::%s()", vis_name(m->vis), classes[c].name, m->name);
		if (m->func < 0) fail("Cannot call abstract method %s::%s()", classes[c].name, m->name);
		Value thisv = *ov;                         /* the frame takes the reference */
		memmove(ov, ov + 1, argc * sizeof(Value));
		sp--;
		if (m->isStatic) { release(thisv); thisv = mk_null(); }
		enter_func(m->func, argc, thisv, m->decl, c, 0);
		return;
	}
	case OP_SCALL: {
		int cr = code[pc++], ni = code[pc++], argc = code[pc++];
		const char *name = consts[ni].u.s->s;
		int c = resolve_cref(cr);
		CMethod *m = find_method(c, name);
		if (!m) fail("Call to undefined method %s::%s()", classes[c].name, name);
		if (!can_access(m->vis, m->decl))
			fail("Call to %s method %s::%s()", vis_name(m->vis), classes[c].name, m->name);
		if (m->func < 0) fail("Cannot call abstract method %s::%s()", classes[c].name, m->name);
		Frame *cur = &frames[nframes - 1];
		Value thisv = mk_null();
		int scls;
		if (!m->isStatic) {
			/* parent::foo() and friends keep $this */
			if (cur->thisv.t != T_OBJ || !is_a(cur->thisv.u.o->cls, c))
				fail("Non-static method %s::%s() cannot be called statically", classes[c].name, m->name);
			thisv = cur->thisv;
			retain(thisv);
			scls = thisv.u.o->cls;
		} else scls = (cr < 0 && cur->scls >= 0) ? cur->scls : c;
		enter_func(m->func, argc, thisv, m->decl, scls, 0);
		return;
	}
	case OP_CCONST: {
		int cr = code[pc++], ni = code[pc++];
		int c = resolve_cref(cr);
		CConst *k = find_cconst(c, consts[ni].u.s->s);
		if (!k) fail("Undefined constant %s::%s", classes[c].name, consts[ni].u.s->s);
		retain(k->val);
		push(k->val);
		return;
	}
	case OP_CLSNAME: {
		int c = resolve_cref(code[pc++]);
		push(mk_cstr(classes[c].name));
		return;
	}
	case OP_INSTOF: {
		int cr = code[pc++];
		a = pop();
		if (cr == CREF_CLOSURE) r = mk_bool(a.t == T_CLO);
		else r = mk_bool(a.t == T_OBJ && is_a(a.u.o->cls, cr >= 0 ? cr : resolve_cref(cr)));
		release(a);
		push(r);
		return;
	}
	case OP_CLONE: {
		a = pop();
		if (a.t != T_OBJ) { release(a); fail("__clone method called on non-object"); }
		Obj *o = a.u.o;
		Value cv = new_object(o->cls);
		for (int i = 0; i < o->nprops; i++) {
			release(cv.u.o->props[i]);
			cv.u.o->props[i] = o->props[i];
			retain(o->props[i]);
		}
		if (o->dyn) cv.u.o->dyn = arr_clone(o->dyn);
		release(a);
		push(cv);
		return;
	}
	case OP_TOPNAME:
		a = pop();
		r = to_str_val(a);
		release(a);
		r.t = T_PNAME;
		push(r);
		return;
	case OP_CLOSURE: {
		int f = code[pc++], nc = code[pc++];
		Clo *c = malloc(sizeof(Clo) + sizeof(Value) * (nc ? nc - 1 : 0));
		if (!c) fail("Out of memory");
		Frame *fr = &frames[nframes - 1];
		c->rc = 1;
		c->func = f;
		c->id = ++objIds;
		c->nuse = nc;
		c->cls = fr->cls;
		c->scls = fr->scls;
		c->thisv = fr->thisv;
		retain(c->thisv);
		memcpy(c->use, &stack[sp - nc], nc * sizeof(Value));   /* takes the references */
		sp -= nc;
		r.t = T_CLO;
		r.u.c = c;
		push(r);
		return;
	}
	case OP_CALLV:
		invoke(code[pc++]);
		return;
	case OP_TRY:
		if (nhandlers >= HANDLERS_MAX) fail("try blocks nested too deeply");
		handlers[nhandlers].pc = code[pc++];
		handlers[nhandlers].nframes = nframes;
		handlers[nhandlers].sp = sp;
		handlers[nhandlers].nscratch = nscratch;
		nhandlers++;
		return;
	case OP_ENDTRY:
		if (nhandlers > 0) nhandlers--;
		return;
	case OP_THROW:
		a = pop();
		if (a.t != T_OBJ || throwableClass < 0 || !is_a(a.u.o->cls, throwableClass)) {
			release(a);
			fail("Can only throw objects that implement Throwable");
		}
		throw_value(a);
	case OP_EXCMATCH: {
		int c = find_class(consts[code[pc++]].u.s->s);
		Value e = stack[sp - 1 - code[pc++]];
		push(mk_bool(c >= 0 && e.t == T_OBJ && is_a(e.u.o->cls, c)));
		return;
	}
	case OP_JSR:
		push(mk_int(pc + 1));
		pc = code[pc];
		return;
	case OP_RETSUB:
		a = pop();
		pc = a.u.i;
		return;
	case OP_CALLB: {
		int bi = code[pc++], argc = code[pc++];
		r = builtin_fn(bi)(&stack[sp - argc], argc);
		pop_n(argc);
		push(r);
		return;
	}
	case OP_CALLREF: {
		int bi = code[pc++], sc = code[pc++], slot = code[pc++], n = code[pc++], argc = code[pc++];
		int extra = argc - 1;
		refArg = lval_slot(sc, slot, &stack[sp - extra - n], n);
		r = builtin_fn(bi)(&stack[sp - extra], extra);
		refArg = NULL;
		pop_n(extra + n);
		push(r);
		return;
	}
	case OP_EXIT:
		a = pop();
		if (a.t == T_STR) out_val(a);
		release(a);
		end_program();
		return;
	default:
		fail("Bad opcode %d", op);
	}
}

/* ------------------------------------------------------------------ */
/* graphics helpers                                                    */
/* ------------------------------------------------------------------ */

static void need_gfx(void) { if (!io->pset) fail("No graphics available"); }

static int color_arg(Value *a, int n, int i) {
	if (n <= i) return gcolor;
	int c = to_int(a[i]);
	if (c < -1 || c > 15) fail("Colour must be -1 to 15");
	return c;
}

static void pset(int x, int y, int c) {
	if (x >= 0 && y >= 0 && x < io->gw && y < io->gh) io->pset(x, y, c);
}

static void hline(int x1, int x2, int y, int c) {
	if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
	if (y < 0 || y >= io->gh) return;
	if (x1 < 0) x1 = 0;
	if (x2 >= io->gw) x2 = io->gw - 1;
	for (int x = x1; x <= x2; x++) io->pset(x, y, c);
}

static void draw_line(int x1, int y1, int x2, int y2, int c) {
	int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
	int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
	int err = dx + dy;
	for (int guard = 0; guard < 4096; guard++) {
		pset(x1, y1, c);
		if (x1 == x2 && y1 == y2) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x1 += sx; }
		if (e2 <= dx) { err += dx; y1 += sy; }
	}
}

static void hide_objects(void) {
	for (int i = 0; i < BASIC_OBJECTS; i++) objects[i].visible = 0;
}

static void slot_changed(int n) {
	for (int i = 0; i < BASIC_OBJECTS; i++)
		if (objects[i].slot == n) objects[i].dirty = 1;
}

static unsigned rnd_next(void) {
	rndState ^= rndState << 13;
	rndState ^= rndState >> 17;
	rndState ^= rndState << 5;
	return rndState;
}

static int rnd_range(int lo, int hi) {
	if (hi < lo) { int t = lo; lo = hi; hi = t; }
	unsigned span = (unsigned)(hi - lo) + 1u;
	return span ? lo + (int)(rnd_next() % span) : (int)rnd_next();
}

/* ------------------------------------------------------------------ */
/* built-in functions                                                  */
/* ------------------------------------------------------------------ */

#define BI(name) static Value bi_##name(Value *a, int n)
#define UNUSED (void)a; (void)n
#define S(i, l) sval(a[i], l)

static Arr *arr_arg(Value *a, int i, const char *fn) {
	if (a[i].t != T_ARR) fail("%s() expects an array", fn);
	return a[i].u.a;
}

static Arr *ref_array(const char *fn) {
	if (refArg->t == T_NULL) *refArg = mk_arr();
	if (refArg->t != T_ARR) fail("%s() expects an array", fn);
	separate(refArg);
	return refArg->u.a;
}

/* --- output --- */

static void dump(Buf *b, Value v, int indent, int varDump) {
	char t[48];
	int l;
	const char *s;
	if (v.t == T_CLO) {
		if (!varDump) { buf_adds(b, "Closure Object\n"); for (int i = 0; i < indent; i++) buf_add(b, " ", 1); buf_adds(b, "(\n"); }
		else { for (int i = 0; i < indent; i++) buf_add(b, " ", 1); snprintf(t, sizeof t, "object(Closure)#%d (0) {\n", v.u.c->id); buf_adds(b, t); }
		for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
		buf_adds(b, varDump ? "}\n" : ")\n");
		return;
	}
	if (v.t == T_OBJ) {
		Obj *o = v.u.o;
		Class *k = &classes[o->cls];
		int n = o->nprops + (o->dyn ? o->dyn->n : 0);
		if (!varDump) { buf_adds(b, k->name); buf_adds(b, " Object\n"); for (int i = 0; i < indent; i++) buf_add(b, " ", 1); buf_adds(b, "(\n"); }
		else { for (int i = 0; i < indent; i++) buf_add(b, " ", 1); snprintf(t, sizeof t, "object(%.20s)#%d (%d) {\n", k->name, o->id, n); buf_adds(b, t); }
		for (int i = 0; i < n; i++) {
			const char *pn;
			Value pv;
			int vis = VIS_PUBLIC;
			if (i < o->nprops) { pn = k->props[i].name; pv = o->props[i]; vis = k->props[i].vis; }
			else { pn = o->dyn->e[i - o->nprops].key.t == T_STR ? o->dyn->e[i - o->nprops].key.u.s->s : "?"; pv = o->dyn->e[i - o->nprops].val; }
			const char *suffix = vis == VIS_PROTECTED ? ":protected" : vis == VIS_PRIVATE ? ":private" : "";
			if (!varDump) {
				for (int j = 0; j < indent + 4; j++) buf_add(b, " ", 1);
				buf_add(b, "[", 1); buf_adds(b, pn); buf_adds(b, suffix); buf_adds(b, "] => ");
				dump(b, pv, indent + 8, 0);
				buf_add(b, "\n", 1);
			} else {
				for (int j = 0; j < indent + 2; j++) buf_add(b, " ", 1);
				buf_adds(b, "[\""); buf_adds(b, pn); buf_adds(b, "\""); buf_adds(b, suffix); buf_adds(b, "]=>\n");
				dump(b, pv, indent + 2, 1);
			}
		}
		for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
		buf_adds(b, varDump ? "}\n" : ")\n");
		return;
	}
	if (!varDump) {
		if (v.t != T_ARR) { s = sval(v, &l); buf_add(b, s, l); return; }
		buf_adds(b, "Array\n");
		for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
		buf_adds(b, "(\n");
		for (int k = 0; k < v.u.a->n; k++) {
			for (int i = 0; i < indent + 4; i++) buf_add(b, " ", 1);
			buf_add(b, "[", 1);
			s = sval(v.u.a->e[k].key, &l); buf_add(b, s, l);
			buf_adds(b, "] => ");
			dump(b, v.u.a->e[k].val, indent + 8, 0);
			buf_add(b, "\n", 1);
		}
		for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
		buf_adds(b, ")\n");
		return;
	}
	for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
	switch (v.t) {
	case T_NULL: buf_adds(b, "NULL\n"); return;
	case T_BOOL: buf_adds(b, v.u.i ? "bool(true)\n" : "bool(false)\n"); return;
	case T_INT: sprintf(t, "int(%d)\n", v.u.i); buf_adds(b, t); return;
	case T_FLOAT: { char u[60]; fmt_float(t, v.u.f); sprintf(u, "float(%s)\n", t); buf_adds(b, u); } return;
	case T_STR: sprintf(t, "string(%d) \"", v.u.s->len); buf_adds(b, t); buf_add(b, v.u.s->s, v.u.s->len); buf_adds(b, "\"\n"); return;
	case T_ARR:
		sprintf(t, "array(%d) {\n", v.u.a->n); buf_adds(b, t);
		for (int k = 0; k < v.u.a->n; k++) {
			for (int i = 0; i < indent + 2; i++) buf_add(b, " ", 1);
			Value key = v.u.a->e[k].key;
			if (key.t == T_INT) sprintf(t, "[%d]=>\n", key.u.i);
			else { buf_adds(b, "[\""); buf_add(b, key.u.s->s, key.u.s->len); strcpy(t, "\"]=>\n"); }
			buf_adds(b, t);
			dump(b, v.u.a->e[k].val, indent + 2, 1);
		}
		for (int i = 0; i < indent; i++) buf_add(b, " ", 1);
		buf_adds(b, "}\n");
		return;
	}
}

BI(print_r) {
	Buf b = { 0 };
	dump(&b, a[0], 0, 0);
	if (n > 1 && to_bool(a[1])) return buf_value(&b);
	if (b.p) out_n(b.p, b.n);
	free(b.p);
	return mk_bool(1);
}

BI(var_dump) {
	for (int i = 0; i < n; i++) {
		Buf b = { 0 };
		dump(&b, a[i], 0, 1);
		if (b.p) out_n(b.p, b.n);
		free(b.p);
	}
	return mk_null();
}

/* sprintf: %[-+ 0][width][.prec](d i u f F s x X o b c e E g %) */
static Value do_sprintf(Value *a, int n) {
	int fl;
	const char *f = S(0, &fl);
	char fmt[LINE_MAX_LEN + 1];
	if (fl > LINE_MAX_LEN) fl = LINE_MAX_LEN;
	memcpy(fmt, f, fl);
	fmt[fl] = 0;
	Buf b = { 0 };
	int arg = 1;
	for (int i = 0; i < fl; i++) {
		if (fmt[i] != '%') { buf_add(&b, &fmt[i], 1); continue; }
		if (fmt[i + 1] == '%') { buf_add(&b, "%", 1); i++; continue; }
		char spec[24];
		int sl = 0, argNo = 0;
		spec[sl++] = '%';
		i++;
		if (isdigit((unsigned char)fmt[i])) {          /* %2$s: argument number */
			int j = i, v = 0;
			while (isdigit((unsigned char)fmt[j])) v = v * 10 + (fmt[j++] - '0');
			if (fmt[j] == '$') {
				if (v < 1) { free(b.p); fail("sprintf(): argument number must be greater than zero"); }
				argNo = v;
				i = j + 1;
			}
		}
		char pad = ' ';
		while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '0' || fmt[i] == '\'') {
			if (fmt[i] == '\'') { pad = fmt[i + 1]; i += 2; continue; }
			if (sl < 10) spec[sl++] = fmt[i];
			i++;
		}
		while (isdigit((unsigned char)fmt[i])) { if (sl < 16) spec[sl++] = fmt[i]; i++; }
		if (fmt[i] == '.') {
			if (sl < 17) spec[sl++] = '.';
			i++;
			while (isdigit((unsigned char)fmt[i])) { if (sl < 20) spec[sl++] = fmt[i]; i++; }
		}
		char type = fmt[i];
		if (!type) break;
		int ai = argNo ? argNo : arg++;
		if (ai >= n) { free(b.p); fail("sprintf(): too few arguments"); }
		Value v = a[ai];
		char out[300];
		switch (type) {
		case 'd': case 'i': spec[sl++] = 'd'; spec[sl] = 0; snprintf(out, sizeof out, spec, to_int(v)); break;
		case 'u': spec[sl++] = 'u'; spec[sl] = 0; snprintf(out, sizeof out, spec, (unsigned)to_int(v)); break;
		case 'x': case 'X': case 'o': spec[sl++] = type; spec[sl] = 0; snprintf(out, sizeof out, spec, (unsigned)to_int(v)); break;
		case 'c': out[0] = (char)to_int(v); out[1] = 0; break;
		case 'b': {
			unsigned u = (unsigned)to_int(v);
			char bits[40];
			int k = 0;
			do { bits[k++] = (char)('0' + (u & 1)); u >>= 1; } while (u);
			for (int j = 0; j < k; j++) out[j] = bits[k - 1 - j];
			out[k] = 0;
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
			if (!strchr(spec, '.')) { spec[sl++] = '.'; spec[sl++] = '6'; }
			spec[sl++] = type == 'F' ? 'f' : type;
			spec[sl] = 0;
			snprintf(out, sizeof out, spec, (double)to_float(v));
			if (type == 'e' || type == 'E') {      /* PHP writes 1.5e+3, not 1.5e+03 */
				char *e = strpbrk(out, "eE");
				if (e && (e[1] == '+' || e[1] == '-')) {
					char *z = e + 2;
					while (z[0] == '0' && isdigit((unsigned char)z[1])) memmove(z, z + 1, strlen(z));
				}
			}
			break;
		case 's': {
			int l;
			const char *s = sval(v, &l);
			char tmp[LINE_MAX_LEN + 1];
			if (l > LINE_MAX_LEN) l = LINE_MAX_LEN;
			memcpy(tmp, s, l);
			tmp[l] = 0;
			spec[sl++] = 's';
			spec[sl] = 0;
			snprintf(out, sizeof out, spec, tmp);
			break;
		}
		default: out[0] = 0;
		}
		if (pad != ' ') {
			if (strchr(spec, '-')) for (char *p = out + strlen(out); p > out && p[-1] == ' '; p--) p[-1] = pad;
			else for (char *p = out; *p == ' '; p++) *p = pad;
		}
		buf_adds(&b, out);
	}
	return buf_value(&b);
}

BI(sprintf) { UNUSED; return do_sprintf(a, n); }

/* vsprintf(format, [args]): arguments from an array */
BI(vsprintf) {
	UNUSED;
	Arr *args = arr_arg(a, 1, "vsprintf");
	if (args->n > 31) fail("vsprintf(): too many arguments");
	Value v[32];
	v[0] = a[0];
	for (int i = 0; i < args->n; i++) v[i + 1] = args->e[i].val;
	return do_sprintf(v, args->n + 1);
}
BI(vprintf) {
	Value s = bi_vsprintf(a, n);
	out_val(s);
	int len = s.u.s->len;
	release(s);
	return mk_int(len);
}
BI(printf) {
	Value s = do_sprintf(a, n);
	out_val(s);
	int len = s.u.s->len;
	release(s);
	return mk_int(len);
}

BI(number_format) {
	float f = to_float(a[0]);
	int dec = n > 1 ? to_int(a[1]) : 0;
	if (dec < 0) dec = 0;
	if (dec > 8) dec = 8;
	char raw[64], outb[96];
	snprintf(raw, sizeof raw, "%.*f", dec, (double)fabsf(f));
	char *dot = strchr(raw, '.');
	int intLen = dot ? (int)(dot - raw) : (int)strlen(raw);
	int o = 0;
	if (f < 0 && atof(raw) != 0) outb[o++] = '-';
	for (int i = 0; i < intLen; i++) {
		if (i && (intLen - i) % 3 == 0) outb[o++] = ',';
		outb[o++] = raw[i];
	}
	if (dot) { strcpy(outb + o, dot); o += (int)strlen(dot); }
	outb[o] = 0;
	return mk_cstr(outb);
}

/* --- strings --- */

BI(strlen) { UNUSED; int l; S(0, &l); return mk_int(l); }
static Value case_map(Value v, int (*fn)(int), int firstOnly) {
	int l;
	const char *src = sval(v, &l);
	Value c = mk_str(src, l);
	for (int i = 0; i < (firstOnly ? (l > 0) : l); i++) c.u.s->s[i] = (char)fn((unsigned char)c.u.s->s[i]);
	return c;
}
BI(strtoupper) { UNUSED; return case_map(a[0], toupper, 0); }
BI(strtolower) { UNUSED; return case_map(a[0], tolower, 0); }
BI(ucfirst) { UNUSED; return case_map(a[0], toupper, 1); }
BI(lcfirst) { UNUSED; return case_map(a[0], tolower, 1); }
BI(strrev) {
	UNUSED;
	int l;
	const char *src = sval(a[0], &l);
	Value c = mk_str(src, l);
	for (int i = 0; i < l; i++) c.u.s->s[i] = src[l - 1 - i];
	return c;
}

static Value trim_impl(Value *a, int n, int left, int right) {
	int l;
	const char *s = S(0, &l);
	const char *chars = " \t\n\r\v";
	char cs[64];
	if (n > 1) { int cl; const char *c = S(1, &cl); if (cl > 63) cl = 63; memcpy(cs, c, cl); cs[cl] = 0; chars = cs; }
	int st = 0, en = l;
	if (left) while (st < en && (strchr(chars, s[st]) || s[st] == 0)) st++;
	if (right) while (en > st && (strchr(chars, s[en - 1]) || s[en - 1] == 0)) en--;
	return mk_str(s + st, en - st);
}
BI(trim) { return trim_impl(a, n, 1, 1); }
BI(ltrim) { return trim_impl(a, n, 1, 0); }
BI(rtrim) { return trim_impl(a, n, 0, 1); }

BI(substr) {
	int l;
	const char *s = S(0, &l);
	int st = to_int(a[1]);
	if (st < 0) { st += l; if (st < 0) st = 0; }
	if (st > l) st = l;
	int len = l - st;
	if (n > 2 && a[2].t != T_NULL) {
		int ln = to_int(a[2]);
		if (ln < 0) len = l - st + ln; else if (ln < len) len = ln;
		if (len < 0) len = 0;
	}
	return mk_str(s + st, len);
}

static int find_str(const char *h, int hl, const char *nd, int nl, int from) {
	if (nl == 0) return from <= hl ? from : -1;
	for (int i = from; i + nl <= hl; i++) if (!memcmp(h + i, nd, nl)) return i;
	return -1;
}

BI(strpos) {
	int hl, nl;
	const char *h = S(0, &hl), *nd = S(1, &nl);
	int off = n > 2 ? to_int(a[2]) : 0;
	if (off < 0) off += hl;
	if (off < 0 || off > hl) fail("strpos(): offset out of range");
	int i = find_str(h, hl, nd, nl, off);
	return i < 0 ? mk_bool(0) : mk_int(i);
}

BI(str_contains) { UNUSED; int hl, nl; const char *h = S(0, &hl), *nd = S(1, &nl); return mk_bool(find_str(h, hl, nd, nl, 0) >= 0); }
BI(str_starts_with) { UNUSED; int hl, nl; const char *h = S(0, &hl), *nd = S(1, &nl); return mk_bool(nl <= hl && !memcmp(h, nd, nl)); }
BI(str_ends_with) { UNUSED; int hl, nl; const char *h = S(0, &hl), *nd = S(1, &nl); return mk_bool(nl <= hl && !memcmp(h + hl - nl, nd, nl)); }

BI(strcmp) { UNUSED; int al, bl; const char *x = S(0, &al), *y = S(1, &bl); return mk_int(str_cmp(x, al, y, bl)); }
BI(strcasecmp) {
	UNUSED;
	int al, bl;
	const char *x = S(0, &al), *y = S(1, &bl);
	for (int i = 0; i < al && i < bl; i++) {
		int d = tolower((unsigned char)x[i]) - tolower((unsigned char)y[i]);
		if (d) return mk_int(d < 0 ? -1 : 1);
	}
	return mk_int(al < bl ? -1 : al > bl);
}
BI(str_repeat) {
	UNUSED;
	int l;
	const char *s = S(0, &l);
	int times = to_int(a[1]);
	if (times < 0) fail("str_repeat(): times must be >= 0");
	if ((long long)l * times > STR_LIMIT) fail("String too long");
	Value r;
	r.t = T_STR;
	r.u.s = str_alloc(l * times);
	for (int i = 0; i < times; i++) memcpy(r.u.s->s + i * l, s, l);
	return r;
}

BI(str_pad) {
	int l, pl = 1;
	const char *s = S(0, &l), *pad = " ";
	int len = to_int(a[1]);
	if (n > 2) pad = S(2, &pl);
	int type = n > 3 ? to_int(a[3]) : 1;     /* STR_PAD_RIGHT */
	if (pl == 0) fail("str_pad(): padding must not be empty");
	if (len <= l) return mk_str(s, l);
	if (len > STR_LIMIT) fail("String too long");
	int total = len - l, left = type == 0 ? total : type == 2 ? total / 2 : 0, right = total - left;
	Value r;
	r.t = T_STR;
	r.u.s = str_alloc(len);
	char *o = r.u.s->s;
	for (int i = 0; i < left; i++) *o++ = pad[i % pl];
	memcpy(o, s, l); o += l;
	for (int i = 0; i < right; i++) *o++ = pad[i % pl];
	return r;
}

BI(str_replace) {
	UNUSED;
	int sl, rl, hl;
	char se[LINE_MAX_LEN + 1];
	const char *s = S(0, &sl);
	if (sl > LINE_MAX_LEN) sl = LINE_MAX_LEN;
	memcpy(se, s, sl);
	const char *rep = S(1, &rl);
	char re[LINE_MAX_LEN + 1];
	if (rl > LINE_MAX_LEN) rl = LINE_MAX_LEN;
	memcpy(re, rep, rl);
	const char *h = S(2, &hl);
	if (sl == 0) return mk_str(h, hl);
	Buf b = { 0 };
	int i = 0;
	while (i < hl) {
		int f = find_str(h, hl, se, sl, i);
		if (f < 0) { buf_add(&b, h + i, hl - i); break; }
		buf_add(&b, h + i, f - i);
		buf_add(&b, re, rl);
		i = f + sl;
	}
	return buf_value(&b);
}

BI(explode) {
	int dl, sl;
	const char *d = S(0, &dl);
	char del[LINE_MAX_LEN + 1];
	if (dl == 0) fail("explode(): empty separator");
	if (dl > LINE_MAX_LEN) dl = LINE_MAX_LEN;
	memcpy(del, d, dl);
	const char *s = S(1, &sl);
	int limit = n > 2 ? to_int(a[2]) : 0x7fffffff;
	Value r = mk_arr();
	int i = 0;
	for (;;) {
		int f = r.u.a->n + 1 >= limit && limit > 0 ? -1 : find_str(s, sl, del, dl, i);
		if (f < 0) { arr_push(r.u.a, mk_str(s + i, sl - i)); break; }
		arr_push(r.u.a, mk_str(s + i, f - i));
		i = f + dl;
	}
	return r;
}

BI(implode) {
	const char *g = "";
	int gl = 0;
	Arr *arr;
	char glue[LINE_MAX_LEN + 1];
	if (n == 1) arr = arr_arg(a, 0, "implode");
	else if (a[0].t == T_ARR) { arr = a[0].u.a; g = S(1, &gl); }
	else { g = S(0, &gl); arr = arr_arg(a, 1, "implode"); }
	if (gl > LINE_MAX_LEN) gl = LINE_MAX_LEN;
	memcpy(glue, g, gl);
	Buf b = { 0 };
	for (int i = 0; i < arr->n; i++) {
		if (i) buf_add(&b, glue, gl);
		int l;
		const char *s = sval(arr->e[i].val, &l);
		buf_add(&b, s, l);
	}
	return buf_value(&b);
}

BI(chr) { UNUSED; char c = (char)(to_int(a[0]) & 255); return mk_str(&c, 1); }
BI(ord) { UNUSED; int l; const char *s = S(0, &l); return mk_int(l ? (unsigned char)s[0] : 0); }

/* --- types --- */

BI(intval) { UNUSED; return mk_int(to_int(a[0])); }
BI(floatval) { UNUSED; return mk_float(to_float(a[0])); }
BI(strval) { UNUSED; return to_str_val(a[0]); }
BI(boolval) { UNUSED; return mk_bool(to_bool(a[0])); }
BI(is_numeric) { UNUSED; Value x; return mk_bool(a[0].t == T_INT || a[0].t == T_FLOAT || (a[0].t == T_STR && numeric_str(a[0].u.s, &x))); }
BI(is_int) { UNUSED; return mk_bool(a[0].t == T_INT); }
BI(is_float) { UNUSED; return mk_bool(a[0].t == T_FLOAT); }
BI(is_string) { UNUSED; return mk_bool(a[0].t == T_STR); }
BI(is_bool) { UNUSED; return mk_bool(a[0].t == T_BOOL); }
BI(is_array) { UNUSED; return mk_bool(a[0].t == T_ARR); }
BI(is_null) { UNUSED; return mk_bool(a[0].t == T_NULL); }
BI(gettype) {
	UNUSED;
	static const char *const names[] = { "NULL", "boolean", "integer", "double", "string", "array" };
	if (a[0].t == T_OBJ || a[0].t == T_CLO) return mk_cstr("object");
	return mk_cstr(names[a[0].t <= T_ARR ? a[0].t : 0]);
}

/* --- classes --- */

static int class_of_arg(Value v) {
	if (v.t == T_OBJ) return v.u.o->cls;
	if (v.t == T_STR) {
		int c = find_class(v.u.s->s);
		return c >= 0 && classes[c].defined ? c : -1;
	}
	return -1;
}

BI(is_object) { UNUSED; return mk_bool(a[0].t == T_OBJ || a[0].t == T_CLO); }
BI(get_class) {
	UNUSED;
	if (a[0].t == T_CLO) return mk_cstr("Closure");
	if (a[0].t != T_OBJ) fail("get_class() expects an object");
	return mk_cstr(classes[a[0].u.o->cls].name);
}
BI(get_parent_class) {
	UNUSED;
	int c = class_of_arg(a[0]);
	if (c < 0 || classes[c].parent < 0) return mk_bool(0);
	return mk_cstr(classes[classes[c].parent].name);
}
BI(class_exists) { UNUSED; int c = class_of_arg(a[0]); return mk_bool(c >= 0 && classes[c].kind == K_CLASS); }
BI(interface_exists) { UNUSED; int c = class_of_arg(a[0]); return mk_bool(c >= 0 && classes[c].kind == K_INTERFACE); }
BI(method_exists) {
	UNUSED;
	int c = class_of_arg(a[0]);
	int l;
	const char *m = sval(a[1], &l);
	return mk_bool(c >= 0 && find_method(c, m) != NULL);
}
BI(property_exists) {
	UNUSED;
	int c = class_of_arg(a[0]);
	int l;
	const char *pn = sval(a[1], &l);
	if (c < 0) return mk_bool(0);
	if (find_prop(c, pn) >= 0) return mk_bool(1);
	for (int k = c; k >= 0; k = classes[k].parent)
		for (int i = 0; i < classes[k].nown; i++) if (!strcmp(classes[k].own[i].name, pn)) return mk_bool(1);
	if (a[0].t == T_OBJ && a[0].u.o->dyn) {
		Value key = mk_cstr(pn);
		int found = arr_slot(a[0].u.o->dyn, key, 0) != NULL;
		release(key);
		return mk_bool(found);
	}
	return mk_bool(0);
}
BI(get_object_vars) {
	UNUSED;
	if (a[0].t != T_OBJ) fail("get_object_vars() expects an object");
	Obj *o = a[0].u.o;
	Value r = mk_arr();
	for (int i = 0; i < o->nprops; i++) {
		CProp *d = &classes[o->cls].props[i];
		if (!can_access(d->vis, d->decl)) continue;
		Value key = mk_cstr(d->name);
		retain(o->props[i]);
		arr_set(r.u.a, key, o->props[i]);
		release(key);
	}
	if (o->dyn)
		for (int i = 0; i < o->dyn->n; i++) { retain(o->dyn->e[i].val); arr_set(r.u.a, o->dyn->e[i].key, o->dyn->e[i].val); }
	return r;
}

/* --- math --- */

BI(abs) { UNUSED; Value v = to_num(a[0]); if (v.t == T_INT) return v.u.i < 0 ? mk_int(-v.u.i) : v; return mk_float(fabsf(v.u.f)); }
BI(floor) { UNUSED; return mk_float(floorf(to_float(a[0]))); }
BI(ceil) { UNUSED; return mk_float(ceilf(to_float(a[0]))); }
BI(round) {
	float f = to_float(a[0]);
	int p = n > 1 ? to_int(a[1]) : 0;
	float m = powf(10, (float)p);
	float r = f * m;
	r = r < 0 ? -floorf(-r + 0.5f) : floorf(r + 0.5f);
	return mk_float(r / m);
}
BI(sqrt) { UNUSED; return mk_float(sqrtf(to_float(a[0]))); }
BI(pow) { UNUSED; return arith(OP_POW, a[0], a[1]); }
BI(sin) { UNUSED; return mk_float(sinf(to_float(a[0]))); }
BI(cos) { UNUSED; return mk_float(cosf(to_float(a[0]))); }
BI(tan) { UNUSED; return mk_float(tanf(to_float(a[0]))); }
BI(asin) { UNUSED; return mk_float(asinf(to_float(a[0]))); }
BI(acos) { UNUSED; return mk_float(acosf(to_float(a[0]))); }
BI(atan) { UNUSED; return mk_float(atanf(to_float(a[0]))); }
BI(atan2) { UNUSED; return mk_float(atan2f(to_float(a[0]), to_float(a[1]))); }
BI(exp) { UNUSED; return mk_float(expf(to_float(a[0]))); }
BI(log) { float f = logf(to_float(a[0])); if (n > 1) f /= logf(to_float(a[1])); return mk_float(f); }
BI(log10) { UNUSED; return mk_float(log10f(to_float(a[0]))); }
BI(pi) { UNUSED; return mk_float(3.14159265f); }
BI(fmod) { UNUSED; return mk_float(fmodf(to_float(a[0]), to_float(a[1]))); }
BI(intdiv) { UNUSED; int d = to_int(a[1]); if (!d) fail("Division by zero"); return mk_int(to_int(a[0]) / d); }
BI(deg2rad) { UNUSED; return mk_float(to_float(a[0]) * 3.14159265f / 180); }
BI(rad2deg) { UNUSED; return mk_float(to_float(a[0]) * 180 / 3.14159265f); }

static Value minmax(Value *a, int n, int wantMax, const char *fn) {
	Value *list = a;
	int cnt = n;
	Value tmp[1];
	if (n == 1) {
		Arr *arr = arr_arg(a, 0, fn);
		if (!arr->n) fail("%s(): empty array", fn);
		Value best = arr->e[0].val;
		for (int i = 1; i < arr->n; i++) {
			int c = loose_cmp(arr->e[i].val, best);
			if (wantMax ? c > 0 : c < 0) best = arr->e[i].val;
		}
		retain(best);
		return best;
	}
	(void)tmp;
	Value best = list[0];
	for (int i = 1; i < cnt; i++) {
		int c = loose_cmp(list[i], best);
		if (wantMax ? c > 0 : c < 0) best = list[i];
	}
	retain(best);
	return best;
}
BI(min) { return minmax(a, n, 0, "min"); }
BI(max) { return minmax(a, n, 1, "max"); }

BI(rand) {
	if (n >= 2) return mk_int(rnd_range(to_int(a[0]), to_int(a[1])));
	return mk_int((int)(rnd_next() & 0x7fffffff));
}
BI(srand) { if (n) rndState = (unsigned)to_int(a[0]) * 2654435761u + 1; if (!rndState) rndState = 1; return mk_null(); }
BI(getrandmax) { UNUSED; return mk_int(0x7fffffff); }

/* --- arrays --- */

BI(count) { UNUSED; return mk_int(arr_arg(a, 0, "count")->n); }

BI(in_array) {
	Arr *arr = arr_arg(a, 1, "in_array");
	int strict = n > 2 && to_bool(a[2]);
	for (int i = 0; i < arr->n; i++)
		if (strict ? strict_eq(arr->e[i].val, a[0]) : loose_cmp(arr->e[i].val, a[0]) == 0) return mk_bool(1);
	return mk_bool(0);
}

BI(array_search) {
	Arr *arr = arr_arg(a, 1, "array_search");
	int strict = n > 2 && to_bool(a[2]);
	for (int i = 0; i < arr->n; i++)
		if (strict ? strict_eq(arr->e[i].val, a[0]) : loose_cmp(arr->e[i].val, a[0]) == 0) {
			retain(arr->e[i].key);
			return arr->e[i].key;
		}
	return mk_bool(0);
}

BI(array_key_exists) { UNUSED; Arr *arr = arr_arg(a, 1, "array_key_exists"); return mk_bool(arr_slot(arr, a[0], 0) != NULL); }

BI(array_keys) {
	UNUSED;
	Arr *arr = arr_arg(a, 0, "array_keys");
	Value r = mk_arr();
	for (int i = 0; i < arr->n; i++) { retain(arr->e[i].key); arr_push(r.u.a, arr->e[i].key); }
	return r;
}

BI(array_values) {
	UNUSED;
	Arr *arr = arr_arg(a, 0, "array_values");
	Value r = mk_arr();
	for (int i = 0; i < arr->n; i++) { retain(arr->e[i].val); arr_push(r.u.a, arr->e[i].val); }
	return r;
}

BI(array_sum) {
	UNUSED;
	Arr *arr = arr_arg(a, 0, "array_sum");
	Value s = mk_int(0);
	for (int i = 0; i < arr->n; i++) s = arith(OP_ADD, s, arr->e[i].val);
	return s;
}

BI(array_reverse) {
	UNUSED;
	Arr *arr = arr_arg(a, 0, "array_reverse");
	Value r = mk_arr();
	for (int i = arr->n - 1; i >= 0; i--) {
		retain(arr->e[i].val);
		if (arr->e[i].key.t == T_INT) arr_push(r.u.a, arr->e[i].val);
		else arr_set(r.u.a, arr->e[i].key, arr->e[i].val);
	}
	return r;
}

BI(array_slice) {
	Arr *arr = arr_arg(a, 0, "array_slice");
	int off = to_int(a[1]), cnt = arr->n;
	if (off < 0) { off += cnt; if (off < 0) off = 0; }
	if (off > cnt) off = cnt;
	int len = cnt - off;
	if (n > 2 && a[2].t != T_NULL) {
		int l = to_int(a[2]);
		if (l < 0) len = cnt - off + l; else if (l < len) len = l;
		if (len < 0) len = 0;
	}
	Value r = mk_arr();
	for (int i = off; i < off + len; i++) {
		retain(arr->e[i].val);
		if (arr->e[i].key.t == T_INT) arr_push(r.u.a, arr->e[i].val);
		else arr_set(r.u.a, arr->e[i].key, arr->e[i].val);
	}
	return r;
}

BI(array_merge) {
	Value r = mk_arr();
	for (int k = 0; k < n; k++) {
		Arr *arr = arr_arg(a, k, "array_merge");
		for (int i = 0; i < arr->n; i++) {
			retain(arr->e[i].val);
			if (arr->e[i].key.t == T_INT) arr_push(r.u.a, arr->e[i].val);
			else arr_set(r.u.a, arr->e[i].key, arr->e[i].val);
		}
	}
	return r;
}

BI(array_fill) {
	UNUSED;
	int st = to_int(a[0]), cnt = to_int(a[1]);
	if (cnt < 0 || cnt > 100000) fail("array_fill(): bad count");
	Value r = mk_arr();
	for (int i = 0; i < cnt; i++) { retain(a[2]); arr_set(r.u.a, mk_int(st + i), a[2]); }
	return r;
}

BI(range) {
	Value r = mk_arr();
	if (a[0].t == T_STR && a[1].t == T_STR && a[0].u.s->len == 1 && a[1].u.s->len == 1 &&
	    !isdigit((unsigned char)a[0].u.s->s[0])) {
		int c0 = (unsigned char)a[0].u.s->s[0], c1 = (unsigned char)a[1].u.s->s[0];
		int st = c0 <= c1 ? 1 : -1;
		for (int c = c0;; c += st) { char ch = (char)c; arr_push(r.u.a, mk_str(&ch, 1)); if (c == c1) break; }
		return r;
	}
	Value lo = to_num(a[0]), hi = to_num(a[1]);
	Value stp = n > 2 ? to_num(a[2]) : mk_int(1);
	if (lo.t == T_INT && hi.t == T_INT && stp.t == T_INT) {
		int s = abs(stp.u.i);
		if (!s) s = 1;
		if ((hi.u.i > lo.u.i ? hi.u.i - lo.u.i : lo.u.i - hi.u.i) / s > 100000) { release(r); fail("range(): too many elements"); }
		if (lo.u.i <= hi.u.i) for (int v = lo.u.i; v <= hi.u.i; v += s) arr_push(r.u.a, mk_int(v));
		else for (int v = lo.u.i; v >= hi.u.i; v -= s) arr_push(r.u.a, mk_int(v));
		return r;
	}
	float fl = to_float(lo), fh = to_float(hi), fs = fabsf(to_float(stp));
	if (fs == 0) fs = 1;
	if (fabsf(fh - fl) / fs > 100000) { release(r); fail("range(): too many elements"); }
	int cnt = (int)(fabsf(fh - fl) / fs + 1e-4f);
	for (int i = 0; i <= cnt; i++) arr_push(r.u.a, mk_float(fl <= fh ? fl + i * fs : fl - i * fs));
	return r;
}

BI(array_push) {
	Arr *arr = ref_array("array_push");
	for (int i = 0; i < n; i++) { retain(a[i]); arr_push(arr, a[i]); }
	return mk_int(arr->n);
}

BI(array_pop) {
	UNUSED;
	Arr *arr = ref_array("array_pop");
	if (!arr->n) return mk_null();
	Value v = arr->e[arr->n - 1].val;
	retain(v);
	arr_remove_at(arr, arr->n - 1);
	arr->next = 0;
	for (int i = 0; i < arr->n; i++)
		if (arr->e[i].key.t == T_INT && arr->e[i].key.u.i >= arr->next) arr->next = arr->e[i].key.u.i + 1;
	return v;
}

BI(array_shift) {
	UNUSED;
	Arr *arr = ref_array("array_shift");
	if (!arr->n) return mk_null();
	Value v = arr->e[0].val;
	retain(v);
	arr_remove_at(arr, 0);
	arr_reindex(arr);
	return v;
}

BI(array_unshift) {
	Arr *arr = ref_array("array_unshift");
	for (int i = 0; i < n; i++) arr_grow(arr), arr_push(arr, mk_null());
	memmove(&arr->e[n], &arr->e[0], (arr->n - n) * sizeof(Entry));
	for (int i = 0; i < n; i++) { retain(a[i]); arr->e[i].key = mk_int(0); arr->e[i].val = a[i]; }
	arr_reindex(arr);
	return mk_int(arr->n);
}

static int cmp_entries(const void *x, const void *y) {
	return loose_cmp(((const Entry *)x)->val, ((const Entry *)y)->val);
}
static int cmp_entries_rev(const void *x, const void *y) { return -cmp_entries(x, y); }

static Value sort_impl(int rev, const char *fn) {
	Arr *arr = ref_array(fn);
	if (arr->n > 1) qsort(arr->e, arr->n, sizeof(Entry), rev ? cmp_entries_rev : cmp_entries);
	for (int i = 0; i < arr->n; i++) { release(arr->e[i].key); arr->e[i].key = mk_int(i); }
	arr->next = arr->n;
	return mk_bool(1);
}
BI(sort) { UNUSED; return sort_impl(0, "sort"); }
BI(rsort) { UNUSED; return sort_impl(1, "rsort"); }

BI(shuffle) {
	UNUSED;
	Arr *arr = ref_array("shuffle");
	for (int i = arr->n - 1; i > 0; i--) {
		int j = (int)(rnd_next() % (unsigned)(i + 1));
		Value t = arr->e[i].val; arr->e[i].val = arr->e[j].val; arr->e[j].val = t;
	}
	for (int i = 0; i < arr->n; i++) { release(arr->e[i].key); arr->e[i].key = mk_int(i); }
	arr->next = arr->n;
	return mk_bool(1);
}

/* --- callbacks --- */

static int is_callable_val(Value v) {
	if (v.t == T_CLO) return 1;
	if (v.t == T_STR) {
		const char *dc = strstr(v.u.s->s, "::");
		if (!dc) {
			int f = func_index(v.u.s->s, 0);
			return (f >= 0 && funcs[f].addr >= 0) || find_builtin(v.u.s->s) >= 0;
		}
		char cn[NAME_MAX_LEN + 1];
		int l = (int)(dc - v.u.s->s);
		if (l > NAME_MAX_LEN) return 0;
		memcpy(cn, v.u.s->s, l);
		cn[l] = 0;
		int c = find_class(cn);
		return c >= 0 && classes[c].defined && find_method(c, dc + 2) != NULL;
	}
	if (v.t == T_OBJ) return find_method(v.u.o->cls, "__invoke") != NULL;
	if (v.t == T_ARR && v.u.a->n == 2 && v.u.a->e[1].val.t == T_STR) {
		Value t = v.u.a->e[0].val;
		int c = t.t == T_OBJ ? t.u.o->cls : t.t == T_STR ? find_class(t.u.s->s) : -1;
		return c >= 0 && classes[c].defined && find_method(c, v.u.a->e[1].val.u.s->s) != NULL;
	}
	return 0;
}

static void need_callable(Value v, const char *fn) {
	if (!is_callable_val(v)) fail("%s(): argument is not callable", fn);
}

BI(is_callable) { UNUSED; return mk_bool(is_callable_val(a[0])); }
BI(function_exists) {
	UNUSED;
	const char *name = S(0, NULL);
	int f = func_index(name, 0);
	return mk_bool((f >= 0 && funcs[f].addr >= 0) || find_builtin(name) >= 0);
}
BI(call_user_func) { need_callable(a[0], "call_user_func"); return call_now(a[0], a + 1, n - 1); }
BI(call_user_func_array) {
	UNUSED;
	need_callable(a[0], "call_user_func_array");
	Arr *args = arr_arg(a, 1, "call_user_func_array");
	if (args->n > PARAMS_MAX + 8) fail("call_user_func_array(): too many arguments");
	Value v[PARAMS_MAX + 8];
	for (int i = 0; i < args->n; i++) v[i] = args->e[i].val;
	return call_now(a[0], v, args->n);
}

BI(array_map) {
	Value cb = a[0];
	if (cb.t != T_NULL) need_callable(cb, "array_map");
	for (int i = 1; i < n; i++) arr_arg(a, i, "array_map");
	Value r = mk_arr();
	scratch_push(NULL, r);
	if (n == 2) {
		/* one array: keys are kept */
		Arr *src = a[1].u.a;
		for (int i = 0; i < src->n; i++) {
			Value v = src->e[i].val;
			if (cb.t == T_NULL) retain(v);
			else v = call_now(cb, &src->e[i].val, 1);
			arr_set(r.u.a, src->e[i].key, v);
		}
		scratch_pop(1);
		return r;
	}
	/* several arrays: walk them side by side, missing values are null */
	if (n - 1 > PARAMS_MAX) fail("array_map(): too many arrays");
	int len = 0;
	for (int i = 1; i < n; i++) if (a[i].u.a->n > len) len = a[i].u.a->n;
	for (int k = 0; k < len; k++) {
		Value args[PARAMS_MAX];
		for (int i = 1; i < n; i++) args[i - 1] = k < a[i].u.a->n ? a[i].u.a->e[k].val : mk_null();
		if (cb.t != T_NULL) arr_push(r.u.a, call_now(cb, args, n - 1));
		else {
			Value t = mk_arr();
			for (int i = 0; i < n - 1; i++) { retain(args[i]); arr_push(t.u.a, args[i]); }
			arr_push(r.u.a, t);
		}
	}
	scratch_pop(1);
	return r;
}

BI(array_filter) {
	Arr *src = arr_arg(a, 0, "array_filter");
	int mode = n > 2 ? to_int(a[2]) : 0;
	Value cb = n > 1 ? a[1] : mk_null();
	if (cb.t != T_NULL) need_callable(cb, "array_filter");
	Value r = mk_arr();
	scratch_push(NULL, r);
	for (int i = 0; i < src->n; i++) {
		Entry *e = &src->e[i];
		int keep;
		if (cb.t == T_NULL) keep = to_bool(e->val);
		else {
			Value args[2] = { mode == 2 ? e->key : e->val, e->key };
			Value t = call_now(cb, args, mode == 1 ? 2 : 1);
			keep = to_bool(t);
			release(t);
		}
		if (keep) { retain(e->val); arr_set(r.u.a, e->key, e->val); }
	}
	scratch_pop(1);
	return r;
}

BI(array_reduce) {
	Arr *src = arr_arg(a, 0, "array_reduce");
	need_callable(a[1], "array_reduce");
	Value acc = n > 2 ? a[2] : mk_null();
	retain(acc);
	for (int i = 0; i < src->n; i++) {
		Value args[2] = { acc, src->e[i].val };
		scratch_push(NULL, acc);
		Value t = call_now(a[1], args, 2);
		scratch_pop(1);
		release(acc);
		acc = t;
	}
	return acc;
}

/* array_walk($arr, fn($value, $key) => ...): the callback cannot change
 * the array (no references), only look at it */
BI(array_walk) {
	need_callable(a[0], "array_walk");
	if (refArg->t != T_ARR) fail("array_walk() expects an array");
	Value arr = *refArg;
	retain(arr);
	scratch_push(NULL, arr);
	for (int i = 0; i < arr.u.a->n; i++) {
		Value args[3] = { arr.u.a->e[i].val, arr.u.a->e[i].key, n > 1 ? a[1] : mk_null() };
		release(call_now(a[0], args, n > 1 ? 3 : 2));
	}
	scratch_pop(1);
	release(arr);
	return mk_bool(1);
}

/* user sorting: stable merge sort over indexes, so a failing callback
 * leaves the array untouched */
typedef struct { Value cb; Entry *e; int keys; } SortCtx;

static int user_cmp(SortCtx *c, int i, int j) {
	Value args[2] = { c->keys ? c->e[i].key : c->e[i].val, c->keys ? c->e[j].key : c->e[j].val };
	Value r = call_now(c->cb, args, 2);
	int res;
	if (r.t == T_BOOL) {
		/* comparators like fn($a, $b) => $a > $b: ask again the other way round */
		if (r.u.i) res = 1;
		else {
			Value sw[2] = { args[1], args[0] };
			Value r2 = call_now(c->cb, sw, 2);
			res = to_bool(r2) ? -1 : 0;
			release(r2);
		}
	} else res = sign(to_float(r));
	release(r);
	return res;
}

static void merge_sort(SortCtx *c, int *idx, int *tmp, int n) {
	if (n < 2) return;
	int h = n / 2, i = 0, j = h, k = 0;
	merge_sort(c, idx, tmp, h);
	merge_sort(c, idx + h, tmp, n - h);
	while (i < h && j < n) tmp[k++] = user_cmp(c, idx[j], idx[i]) < 0 ? idx[j++] : idx[i++];
	while (i < h) tmp[k++] = idx[i++];
	while (j < n) tmp[k++] = idx[j++];
	memcpy(idx, tmp, n * sizeof(int));
}

static Value usort_impl(Value cb, int keepKeys, int byKey, const char *fn) {
	need_callable(cb, fn);
	Arr *arr = ref_array(fn);
	Value hold;                                /* keeps the array alive during callbacks */
	hold.t = T_ARR;
	hold.u.a = arr;
	retain(hold);
	scratch_push(NULL, hold);
	int nn = arr->n;
	if (nn > 1) {
		int *idx = malloc(nn * 2 * sizeof(int));
		if (!idx) fail("Out of memory");
		scratch_push(idx, mk_null());
		for (int i = 0; i < nn; i++) idx[i] = i;
		SortCtx c = { cb, arr->e, byKey };
		merge_sort(&c, idx, idx + nn, nn);
		scratch_pop(1);
		Entry *sorted = malloc(nn * sizeof(Entry));
		if (!sorted) { free(idx); fail("Out of memory"); }
		if (arr->n == nn) {                    /* unless the callback changed it */
			for (int i = 0; i < nn; i++) sorted[i] = arr->e[idx[i]];
			memcpy(arr->e, sorted, nn * sizeof(Entry));
		}
		free(sorted);
		free(idx);
	}
	if (!keepKeys) {
		for (int i = 0; i < arr->n; i++) { release(arr->e[i].key); arr->e[i].key = mk_int(i); }
		arr->next = arr->n;
	}
	scratch_pop(1);
	release(hold);
	return mk_bool(1);
}
BI(usort) { UNUSED; return usort_impl(a[0], 0, 0, "usort"); }
BI(uasort) { UNUSED; return usort_impl(a[0], 1, 0, "uasort"); }
BI(uksort) { UNUSED; return usort_impl(a[0], 1, 1, "uksort"); }

/* --- date and time ---
 * The DS clock (RTC) has no time zone: it holds the local time, and
 * timestamps here count seconds of that local time since 1970-01-01
 * (as if the clock were UTC). The host build does the same with its
 * local time. */

static int now_ts(void) {
	time_t t = time(NULL);
#ifndef ARM9
	struct tm lt;
	localtime_r(&t, &lt);
	t += lt.tm_gmtoff;
#endif
	return (int)t;
}

static long long days_from_civil(long long y, int m, int d) {
	y -= m <= 2;
	long long era = (y >= 0 ? y : y - 399) / 400;
	long long yoe = y - era * 400;
	long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, int *y, int *m, int *d) {
	z += 719468;
	long long era = (z >= 0 ? z : z - 146096) / 146097;
	long long doe = z - era * 146097;
	long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long long mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yoe + era * 400 + (*m <= 2));
}

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(int y, int m) {
	static const char md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	return m == 2 && is_leap(y) ? 29 : md[m - 1];
}

typedef struct { int y, m, d, h, i, s, wday, yday; long long days; } DateParts;

static void split_ts(long long ts, DateParts *p) {
	long long days = ts >= 0 ? ts / 86400 : -((-ts + 86399) / 86400);
	int secs = (int)(ts - days * 86400);
	civil_from_days(days, &p->y, &p->m, &p->d);
	p->h = secs / 3600;
	p->i = secs / 60 % 60;
	p->s = secs % 60;
	p->wday = (int)(((days + 4) % 7 + 7) % 7);          /* 0 = Sunday */
	p->yday = (int)(days - days_from_civil(p->y, 1, 1));
	p->days = days;
}

BI(time) { UNUSED; return mk_int(now_ts()); }

BI(date) {
	static const char *const dayN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const monN[12] = { "January", "February", "March", "April", "May", "June", "July",
	                                       "August", "September", "October", "November", "December" };
	int fl;
	const char *f = S(0, &fl);
	long long ts = n > 1 && a[1].t != T_NULL ? to_int(a[1]) : now_ts();
	DateParts p;
	split_ts(ts, &p);
	/* ISO-8601 week: the week of the Thursday */
	int isoW = (p.wday + 6) % 7 + 1;
	DateParts th;
	split_ts((p.days + 4 - isoW) * 86400LL, &th);
	int week = th.yday / 7 + 1;
	Buf b = { 0 };
	char t[40];
	for (int k = 0; k < fl; k++) {
		char c = f[k];
		t[0] = 0;
		switch (c) {
		case 'd': sprintf(t, "%02d", p.d); break;
		case 'D': snprintf(t, sizeof t, "%.3s", dayN[p.wday]); break;
		case 'j': sprintf(t, "%d", p.d); break;
		case 'l': strcpy(t, dayN[p.wday]); break;
		case 'N': sprintf(t, "%d", isoW); break;
		case 'S': strcpy(t, (p.d % 10 == 1 && p.d != 11) ? "st" : (p.d % 10 == 2 && p.d != 12) ? "nd" :
		                    (p.d % 10 == 3 && p.d != 13) ? "rd" : "th"); break;
		case 'w': sprintf(t, "%d", p.wday); break;
		case 'z': sprintf(t, "%d", p.yday); break;
		case 'W': sprintf(t, "%02d", week); break;
		case 'o': sprintf(t, "%d", th.y); break;
		case 'F': strcpy(t, monN[p.m - 1]); break;
		case 'M': snprintf(t, sizeof t, "%.3s", monN[p.m - 1]); break;
		case 'm': sprintf(t, "%02d", p.m); break;
		case 'n': sprintf(t, "%d", p.m); break;
		case 't': sprintf(t, "%d", month_days(p.y, p.m)); break;
		case 'L': sprintf(t, "%d", is_leap(p.y)); break;
		case 'Y': sprintf(t, "%d", p.y); break;
		case 'y': sprintf(t, "%02d", (p.y % 100 + 100) % 100); break;
		case 'a': strcpy(t, p.h < 12 ? "am" : "pm"); break;
		case 'A': strcpy(t, p.h < 12 ? "AM" : "PM"); break;
		case 'g': sprintf(t, "%d", (p.h + 11) % 12 + 1); break;
		case 'G': sprintf(t, "%d", p.h); break;
		case 'h': sprintf(t, "%02d", (p.h + 11) % 12 + 1); break;
		case 'H': sprintf(t, "%02d", p.h); break;
		case 'i': sprintf(t, "%02d", p.i); break;
		case 's': sprintf(t, "%02d", p.s); break;
		case 'u': strcpy(t, "000000"); break;
		case 'v': strcpy(t, "000"); break;
		case 'e': case 'T': strcpy(t, "UTC"); break;
		case 'P': strcpy(t, "+00:00"); break;
		case 'O': strcpy(t, "+0000"); break;
		case 'Z': strcpy(t, "0"); break;
		case 'I': strcpy(t, "0"); break;
		case 'U': sprintf(t, "%lld", ts); break;
		case 'c': sprintf(t, "%04d-%02d-%02dT%02d:%02d:%02d+00:00", p.y, p.m, p.d, p.h, p.i, p.s); break;
		case 'r': snprintf(t, sizeof t, "%.3s, %02d %.3s %04d %02d:%02d:%02d +0000", dayN[p.wday], p.d,
		                   monN[p.m - 1], p.y, p.h, p.i, p.s); break;
		case '\\': if (k + 1 < fl) k++; buf_add(&b, &f[k], 1); continue;
		default: buf_add(&b, &c, 1); continue;
		}
		buf_adds(&b, t);
	}
	return buf_value(&b);
}

/* mktime(hour, minute, second, month, day, year): missing values are taken
 * from now, values out of range roll over like in PHP */
BI(mktime) {
	DateParts now;
	split_ts(now_ts(), &now);
	int v[6] = { now.h, now.i, now.s, now.m, now.d, now.y };
	for (int i = 0; i < n && i < 6; i++) if (a[i].t != T_NULL) v[i] = to_int(a[i]);
	long long y = v[5];
	if (n > 5 && y >= 0 && y < 70) y += 2000;
	else if (n > 5 && y >= 70 && y <= 100) y += 1900;
	long long m0 = (long long)v[3] - 1;
	y += m0 >= 0 ? m0 / 12 : -((11 - m0) / 12);
	int m = (int)(((m0 % 12) + 12) % 12) + 1;
	long long ts = (days_from_civil(y, m, 1) + v[4] - 1) * 86400LL + (long long)v[0] * 3600 + (long long)v[1] * 60 + v[2];
	if (ts > 2147483647LL || ts < -2147483647LL - 1) return mk_bool(0);
	return mk_int((int)ts);
}

BI(checkdate) {
	UNUSED;
	int m = to_int(a[0]), d = to_int(a[1]), y = to_int(a[2]);
	return mk_bool(m >= 1 && m <= 12 && y >= 1 && y <= 32767 && d >= 1 && d <= month_days(y, m));
}

/* --- DS: text and input --- */

BI(cls) { UNUSED; if (io->cls) io->cls(); col = 0; return mk_null(); }
BI(locate) {
	UNUSED;
	int x = to_int(a[0]), y = to_int(a[1]);
	if (x < 0 || y < 0) fail("locate(): position out of range");
	if (io->locate) io->locate(x, y);
	col = x;
	return mk_null();
}
BI(color) {
	UNUSED;
	int c = to_int(a[0]);
	if (c < 0 || c > 15) fail("color(): colour must be 0-15");
	if (io->color) io->color(c);
	return mk_null();
}
BI(input) {
	if (n) out_val(a[0]);
	state = BASIC_INPUT;            /* basic_input() replaces the result */
	return mk_null();
}
BI(inkey) {
	UNUSED;
	int k = io->inkey ? io->inkey() : 0;
	if (!k) return mk_str("", 0);
	char c = (char)k;
	return mk_str(&c, 1);
}
BI(joy) { UNUSED; return mk_int(io->joy ? io->joy() : 0); }

/* swap_screens(): program output to the touch screen and back; with an
 * argument true/false the state is set; returns the new state */
BI(swap_screens) {
	if (!io->swap) return mk_bool(0);
	return mk_bool(io->swap(n ? to_bool(a[0]) : -1));
}

/* touch(): ['x' => .., 'y' => ..] while the stylus is down, else null */
BI(touch) {
	UNUSED;
	int x, y;
	if (!io->touch || !io->touch(&x, &y)) return mk_null();
	Value r = mk_arr(), k = mk_cstr("x");
	arr_set(r.u.a, k, mk_int(x));
	release(k);
	k = mk_cstr("y");
	arr_set(r.u.a, k, mk_int(y));
	release(k);
	return r;
}
BI(pause) {
	UNUSED;
	int f = to_int(a[0]);
	if (f < 0) fail("pause(): frames must be >= 0");
	waitFrames = f;
	return mk_null();
}
BI(sleep) { UNUSED; int s = to_int(a[0]); if (s < 0) s = 0; if (s > 3600) s = 3600; waitFrames = s * 60; return mk_int(0); }
BI(usleep) { UNUSED; int u = to_int(a[0]); if (u < 0) u = 0; waitFrames = (int)((long long)u * 60 / 1000000); return mk_null(); }

/* --- DS: graphics --- */

BI(gcls) { UNUSED; need_gfx(); if (io->gcls) io->gcls(); return mk_null(); }
BI(gcolor) { UNUSED; int c = to_int(a[0]); if (c < -1 || c > 15) fail("gcolor(): colour must be -1 to 15"); gcolor = c; return mk_null(); }
BI(plot) { need_gfx(); pset(to_int(a[0]), to_int(a[1]), color_arg(a, n, 2)); return mk_null(); }
BI(line) {
	need_gfx();
	draw_line(to_int(a[0]), to_int(a[1]), to_int(a[2]), to_int(a[3]), color_arg(a, n, 4));
	return mk_null();
}
BI(rect) {
	need_gfx();
	int x1 = to_int(a[0]), y1 = to_int(a[1]), x2 = to_int(a[2]), y2 = to_int(a[3]);
	int c = color_arg(a, n, 4), f = n > 5 && to_bool(a[5]);
	if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
	if (f) {
		for (int y = y1 < 0 ? 0 : y1; y <= y2 && y < io->gh; y++) hline(x1, x2, y, c);
	} else {
		hline(x1, x2, y1, c);
		hline(x1, x2, y2, c);
		draw_line(x1, y1, x1, y2, c);
		draw_line(x2, y1, x2, y2, c);
	}
	return mk_null();
}
BI(circle) {
	need_gfx();
	int cx = to_int(a[0]), cy = to_int(a[1]), r = to_int(a[2]);
	if (r < 0 || r > 1000) fail("circle(): radius out of range");
	int c = color_arg(a, n, 3), f = n > 4 && to_bool(a[4]);
	int x = r, y = 0, err = 1 - r;
	while (x >= y) {
		if (f) {
			hline(cx - x, cx + x, cy + y, c);
			hline(cx - x, cx + x, cy - y, c);
			hline(cx - y, cx + y, cy + x, c);
			hline(cx - y, cx + y, cy - x, c);
		} else {
			pset(cx + x, cy + y, c); pset(cx - x, cy + y, c);
			pset(cx + x, cy - y, c); pset(cx - x, cy - y, c);
			pset(cx + y, cy + x, c); pset(cx - y, cy + x, c);
			pset(cx + y, cy - x, c); pset(cx - y, cy - x, c);
		}
		y++;
		if (err < 0) err += 2 * y + 1;
		else { x--; err += 2 * (y - x) + 1; }
	}
	return mk_null();
}
BI(paint) {
	static short st[4096][2];
	need_gfx();
	if (!io->pget) fail("No graphics available");
	int x = to_int(a[0]), y = to_int(a[1]), c = color_arg(a, n, 2);
	if (x < 0 || y < 0 || x >= io->gw || y >= io->gh) return mk_null();
	int target = io->pget(x, y);
	if (target == c) return mk_null();
	int top = 0;
	st[top][0] = (short)x; st[top][1] = (short)y; top++;
	while (top > 0) {
		top--;
		x = st[top][0]; y = st[top][1];
		if (io->pget(x, y) != target) continue;
		int lx = x, rx = x;
		while (lx > 0 && io->pget(lx - 1, y) == target) lx--;
		while (rx < io->gw - 1 && io->pget(rx + 1, y) == target) rx++;
		for (int i = lx; i <= rx; i++) io->pset(i, y, c);
		for (int ny = y - 1; ny <= y + 1; ny += 2) {
			if (ny < 0 || ny >= io->gh) continue;
			for (int i = lx; i <= rx; i++) {
				if (io->pget(i, ny) != target) continue;
				if (i > lx && io->pget(i - 1, ny) == target) continue;
				if (top >= 4096) fail("paint(): area too complex");
				st[top][0] = (short)i; st[top][1] = (short)ny; top++;
			}
		}
	}
	return mk_null();
}
BI(point) {
	UNUSED;
	need_gfx();
	int x = to_int(a[0]), y = to_int(a[1]);
	if (!io->pget || x < 0 || y < 0 || x >= io->gw || y >= io->gh) return mk_int(-1);
	return mk_int(io->pget(x, y));
}

/* --- DS: sprites --- */

static int sprite_slot(Value v) {
	int s = to_int(v);
	if (s < 0 || s >= BASIC_SPRITES) fail("Sprite slot must be 0-%d", BASIC_SPRITES - 1);
	return s;
}

BI(sprite) {
	int o = to_int(a[0]), sl = sprite_slot(a[1]), x = to_int(a[2]), y = to_int(a[3]);
	int sc = n > 4 ? to_int(a[4]) : 1;
	if (o < 0 || o >= BASIC_OBJECTS) fail("Sprite object must be 0-%d", BASIC_OBJECTS - 1);
	if (sc < 1 || sc > BASIC_OBJ_SCALE) fail("Sprite size must be 1-%d", BASIC_OBJ_SCALE);
	if (x < -9999) x = -9999; else if (x > 9999) x = 9999;
	if (y < -9999) y = -9999; else if (y > 9999) y = 9999;
	BasicObject *b = &objects[o];
	if (!b->visible || b->slot != sl || b->scale != sc) b->dirty = 1;
	b->visible = 1;
	b->slot = (unsigned char)sl;
	b->scale = (unsigned char)sc;
	b->x = x;
	b->y = y;
	return mk_null();
}

BI(sproff) {
	if (!n) { hide_objects(); return mk_null(); }
	int o = to_int(a[0]);
	if (o < 0 || o >= BASIC_OBJECTS) fail("Sprite object must be 0-%d", BASIC_OBJECTS - 1);
	objects[o].visible = 0;
	return mk_null();
}

BI(sprhit) {
	UNUSED;
	int i = to_int(a[0]), j = to_int(a[1]);
	if (i < 0 || i >= BASIC_OBJECTS || j < 0 || j >= BASIC_OBJECTS) fail("Sprite object must be 0-%d", BASIC_OBJECTS - 1);
	const BasicObject *p = &objects[i], *q = &objects[j];
	if (!p->visible || !q->visible || i == j) return mk_bool(0);
	int ps = 8 * p->scale, qs = 8 * q->scale;
	return mk_bool(p->x < q->x + qs && q->x < p->x + ps && p->y < q->y + qs && q->y < p->y + ps);
}

BI(stamp) {
	need_gfx();
	int sl = sprite_slot(a[0]), x = to_int(a[1]), y = to_int(a[2]), sc = n > 3 ? to_int(a[3]) : 1;
	if (sc < 1 || sc > 8) fail("stamp(): size must be 1-8");
	const signed char *d = sprites[sl];
	for (int py = 0; py < 8; py++)
		for (int px = 0; px < 8; px++) {
			int c = d[py * 8 + px];
			if (c < 0) continue;
			for (int j = 0; j < sc; j++)
				for (int i = 0; i < sc; i++) pset(x + px * sc + i, y + py * sc + j, c);
		}
	return mk_null();
}

BI(sprset) {
	UNUSED;
	int sl = sprite_slot(a[0]), x = to_int(a[1]), y = to_int(a[2]), c = to_int(a[3]);
	if (x < 0 || x > 7 || y < 0 || y > 7 || c < -1 || c > 15) fail("sprset(): value out of range");
	sprites[sl][y * 8 + x] = (signed char)c;
	slot_changed(sl);
	return mk_null();
}

/* --- DS: sound --- */

BI(sound) {
	int s = to_int(a[0]), w = n > 1 && to_bool(a[1]);
	if (s < -1 || s >= BASIC_SOUNDS) fail("Sound slot must be 0-%d or -1", BASIC_SOUNDS - 1);
	if (!io->sound) fail("No sound available");
	io->sound(s);
	if (w && s >= 0 && io->sound_busy) waitSound = 1;
	return mk_null();
}

/* tone()/noise()/sfx(): effect voice with a queue of frequency sweeps */
#define TONE_VOL   80
#define TONE_BLOCK 16          /* tone() waits while this many segments are queued */

static int ms_frames(int ms) { int f = (ms * 60 + 500) / 1000; return f < 1 ? 1 : f > 600 ? 600 : f; }

/* 440, "A4", "C#5", "Eb3" -> Hz */
static int tone_hz(Value v) {
	if (v.t != T_STR) {
		int hz = to_int(v);
		if (hz < 20 || hz > 8000) fail("tone(): frequency must be 20-8000 Hz");
		return hz;
	}
	const char *s = v.u.s->s;
	static const signed char semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
	char c = (char)toupper((unsigned char)*s);
	if (c < 'A' || c > 'G') fail("tone(): bad note \"%.10s\" (e.g. \"C4\", \"F#5\")", s);
	int semi = semis[c - 'A'];
	s++;
	if (*s == '#') { semi++; s++; } else if (*s == 'b') { semi--; s++; } else if (*s == '-') s++;
	if (!isdigit((unsigned char)*s) || s[1]) fail("tone(): bad note \"%.10s\" (e.g. \"C4\", \"F#5\")", v.u.s->s);
	int midi = (*s - '0' + 1) * 12 + semi;
	return (int)(440.0f * powf(2.0f, (midi - 69) / 12.0f) + 0.5f);
}

static void tone_seg(int wave, int hz0, int hz1, int frames, int v0, int v1, int replace) {
	if (io->tone) io->tone(wave, hz0, hz1, frames, v0, v1, replace);
}

static void queue_tone(int wave, int hz, int ms) {
	int f = ms_frames(ms);
	if (f >= 3) { tone_seg(wave, hz, hz, f - 1, TONE_VOL, TONE_VOL, 0); tone_seg(wave, hz, hz, 1, 0, 0, 0); }
	else tone_seg(wave, hz, hz, f, TONE_VOL, TONE_VOL, 0);
	if (io->tone_queued && io->tone_queued() >= TONE_BLOCK) waitTone = 1;
}

BI(tone) {
	int hz = tone_hz(a[0]), ms = n > 1 ? to_int(a[1]) : 200, w = n > 2 ? to_int(a[2]) : SND_SQ50;
	if (w < 0 || w > SND_NOISE) fail("tone(): wave must be 0-3 (WAVE_SQ12 .. WAVE_NOISE)");
	if (hz && ms > 0) queue_tone(w, hz, ms);
	return mk_null();
}

BI(noise) {
	int ms = n ? to_int(a[0]) : 200, hz = n > 1 ? tone_hz(a[1]) : 2000;
	if (ms > 0) queue_tone(SND_NOISE, hz, ms);
	return mk_null();
}

BI(sfx) {
	UNUSED;
	/* name, then segments: wave, hz0, hz1, frames, vol0, vol1 (wave -1 ends) */
	static const struct { const char *name; short seg[5][6]; } fx[] = {
		{ "blip",      { { 2, 1200, 1200, 3, 80, 40 }, { -1 } } },
		{ "select",    { { 2, 1568, 1568, 4, 80, 80 }, { 2, 2093, 2093, 7, 80, 0 }, { -1 } } },
		{ "coin",      { { 2, 988, 988, 5, 90, 90 }, { 2, 1319, 1319, 18, 90, 0 }, { -1 } } },
		{ "jump",      { { 1, 280, 820, 12, 90, 30 }, { -1 } } },
		{ "laser",     { { 0, 1800, 250, 10, 90, 20 }, { -1 } } },
		{ "hit",       { { 3, 900, 250, 9, 110, 0 }, { -1 } } },
		{ "explosion", { { 3, 700, 60, 45, 127, 0 }, { -1 } } },
		{ "powerup",   { { 2, 523, 523, 5, 80, 80 }, { 2, 659, 659, 5, 80, 80 }, { 2, 784, 784, 5, 80, 80 }, { 2, 1047, 1047, 14, 80, 0 }, { -1 } } },
		{ "error",     { { 2, 220, 220, 9, 90, 90 }, { 2, 220, 220, 3, 0, 0 }, { 2, 150, 150, 16, 90, 0 }, { -1 } } },
		{ NULL, { { -1 } } }
	};
	const char *name = S(0, NULL);
	for (int i = 0; fx[i].name; i++) {
		if (strcasecmp(fx[i].name, name)) continue;
		for (int k = 0; k < 5 && fx[i].seg[k][0] >= 0; k++) {
			const short *g = fx[i].seg[k];
			tone_seg(g[0], g[1], g[2], g[3], g[4], g[5], k == 0);
		}
		return mk_null();
	}
	fail("sfx(): unknown effect \"%.12s\" (e.g. coin, jump, hit)", name);
}

BI(sound_busy) {
	UNUSED;
	return mk_bool((io->sound_busy && io->sound_busy()) || (io->tone_queued && io->tone_queued() > 0));
}

/* --- files: data files of the program --- */

/* Files live in the program's folder (storage/NAME/), for unsaved programs
 * directly in the storage folder. Names: letters, digits, _ - . (no paths);
 * .php files can be read but not written, the editor owns them. */
static int data_path(Value v, char *path, int write, const char *fn) {
	int len;
	const char *name = sval(v, &len);
	if (len < 1 || len > 32 || name[0] == '.') fail("%s(): bad file name \"%.32s\"", fn, name);
	for (int i = 0; i < len; i++)
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.')
			fail("%s(): bad file name \"%.32s\" (no folders)", fn, name);
	if (write && len > 4 && !strcasecmp(name + len - 4, ".php")) fail("%s(): .php files belong to the editor", fn);
	if (!storageDir[0]) return 0;
	if (lastFile[0] && !strchr(lastFile, '.')) {
		snprintf(path, 200, "%.127s/%.15s", storageDir, lastFile);
		if (write) mkdir(path, 0777);
		snprintf(path, 200, "%.127s/%.15s/%.32s", storageDir, lastFile, name);
	} else snprintf(path, 200, "%.127s/%.32s", storageDir, name);
	return 1;
}

/* whole file into a Buf; 0 = cannot open */
static int read_all(const char *path, Buf *b) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return 0;
	char chunk[512];
	size_t r;
	while ((r = fread(chunk, 1, sizeof chunk, fp)) > 0) {
		if (b->n + (int)r > STR_LIMIT) { fclose(fp); free(b->p); b->p = NULL; fail("File too large (max. %d KB)", STR_LIMIT / 1024); }
		buf_add(b, chunk, (int)r);
	}
	fclose(fp);
	return 1;
}

BI(file_get_contents) {
	UNUSED;
	char path[200];
	Buf b = { 0 };
	if (!data_path(a[0], path, 0, "file_get_contents") || !read_all(path, &b)) return mk_bool(0);
	return buf_value(&b);
}

BI(file_put_contents) {
	char path[200];
	int flags = n > 2 ? to_int(a[2]) : 0;
	if (!data_path(a[0], path, 1, "file_put_contents")) return mk_bool(0);
	FILE *fp = fopen(path, (flags & 8) ? "ab" : "wb");
	if (!fp) return mk_bool(0);
	long total = 0;
	int ok = 1, len;
	if (a[1].t == T_ARR) {
		for (int i = 0; i < a[1].u.a->n && ok; i++) {
			const char *s = sval(a[1].u.a->e[i].val, &len);
			ok = fwrite(s, 1, len, fp) == (size_t)len;
			total += len;
		}
	} else {
		const char *s = sval(a[1], &len);
		ok = fwrite(s, 1, len, fp) == (size_t)len;
		total = len;
	}
	if (fclose(fp) != 0) ok = 0;
	return ok ? mk_int((int)total) : mk_bool(0);
}

BI(file) {
	char path[200];
	int flags = n > 1 ? to_int(a[1]) : 0;
	Buf b = { 0 };
	if (!data_path(a[0], path, 0, "file") || !read_all(path, &b)) return mk_bool(0);
	Value r = mk_arr();
	for (int i = 0; i < b.n;) {
		int j = i;
		while (j < b.n && b.p[j] != '\n') j++;
		int end = j < b.n ? j + 1 : j, e = end;
		if (flags & 2) { e = j; if (e > i && b.p[e - 1] == '\r') e--; }
		if (!((flags & 4) && (flags & 2) && e == i)) arr_push(r.u.a, mk_str(b.p + i, e - i));
		i = end;
	}
	free(b.p);
	return r;
}

BI(file_exists) {
	UNUSED;
	char path[200];
	struct stat st;
	return mk_bool(data_path(a[0], path, 0, "file_exists") && stat(path, &st) == 0);
}

BI(unlink) {
	UNUSED;
	char path[200];
	return mk_bool(data_path(a[0], path, 1, "unlink") && remove(path) == 0);
}

/* --- JSON --- */

static int json_str(Buf *b, const char *s, int len, int flags) {
	buf_add(b, "\"", 1);
	for (int i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		char t[8];
		switch (c) {
		case '"':  buf_add(b, "\\\"", 2); break;
		case '\\': buf_add(b, "\\\\", 2); break;
		case '/':  if (flags & 64) buf_add(b, "/", 1); else buf_add(b, "\\/", 2); break;
		case '\n': buf_add(b, "\\n", 2); break;
		case '\r': buf_add(b, "\\r", 2); break;
		case '\t': buf_add(b, "\\t", 2); break;
		case '\b': buf_add(b, "\\b", 2); break;
		case '\f': buf_add(b, "\\f", 2); break;
		default:
			if (c < 32) { sprintf(t, "\\u%04x", c); buf_adds(b, t); }
			else buf_add(b, (const char *)&c, 1);
		}
	}
	buf_add(b, "\"", 1);
	return 1;
}

static void json_nl(Buf *b, int flags, int depth) {
	if (!(flags & 128)) return;
	buf_add(b, "\n", 1);
	for (int i = 0; i < depth; i++) buf_add(b, "    ", 4);
}

static int json_enc(Buf *b, Value v, int flags, int depth) {
	char t[48];
	int l;
	if (depth > 32) return 0;
	switch (v.t) {
	case T_NULL: buf_adds(b, "null"); return 1;
	case T_BOOL: buf_adds(b, v.u.i ? "true" : "false"); return 1;
	case T_INT: sprintf(t, "%d", v.u.i); buf_adds(b, t); return 1;
	case T_FLOAT:
		if (isnan(v.u.f) || isinf(v.u.f)) return 0;
		l = fmt_float(t, v.u.f);
		if (!strpbrk(t, ".EN")) strcpy(t + l, ".0");
		buf_adds(b, t);
		return 1;
	case T_STR: return json_str(b, v.u.s->s, v.u.s->len, flags);
	case T_CLO: buf_adds(b, "{}"); return 1;
	case T_ARR: {
		Arr *a = v.u.a;
		int list = 1;
		for (int i = 0; i < a->n && list; i++) list = a->e[i].key.t == T_INT && a->e[i].key.u.i == i;
		if (!a->n) { buf_adds(b, "[]"); return 1; }
		buf_add(b, list ? "[" : "{", 1);
		for (int i = 0; i < a->n; i++) {
			if (i) buf_add(b, ",", 1);
			json_nl(b, flags, depth + 1);
			if (!list) {
				const char *k = sval(a->e[i].key, &l);
				json_str(b, k, l, flags);
				buf_adds(b, (flags & 128) ? ": " : ":");
			}
			if (!json_enc(b, a->e[i].val, flags, depth + 1)) return 0;
		}
		json_nl(b, flags, depth);
		buf_add(b, list ? "]" : "}", 1);
		return 1;
	}
	case T_OBJ: {
		/* public properties as a JSON object */
		Obj *o = v.u.o;
		Class *k = &classes[o->cls];
		int n = o->nprops + (o->dyn ? o->dyn->n : 0), first = 1;
		buf_add(b, "{", 1);
		for (int i = 0; i < n; i++) {
			const char *pn;
			Value pv;
			if (i < o->nprops) { if (k->props[i].vis != VIS_PUBLIC) continue; pn = k->props[i].name; pv = o->props[i]; }
			else { Entry *e = &o->dyn->e[i - o->nprops]; pn = sval(e->key, NULL); pv = e->val; }
			if (!first) buf_add(b, ",", 1);
			first = 0;
			json_nl(b, flags, depth + 1);
			json_str(b, pn, (int)strlen(pn), flags);
			buf_adds(b, (flags & 128) ? ": " : ":");
			if (!json_enc(b, pv, flags, depth + 1)) return 0;
		}
		if (!first) json_nl(b, flags, depth);
		buf_add(b, "}", 1);
		return 1;
	}
	default: return 0;
	}
}

BI(json_encode) {
	Buf b = { 0 };
	if (!json_enc(&b, a[0], n > 1 ? to_int(a[1]) : 0, 0)) { free(b.p); return mk_bool(0); }
	return buf_value(&b);
}

/* JSON parser; objects become associative arrays */
typedef struct { const char *p, *end; int err; } Json;

static void js_ws(Json *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }

static int js_lit(Json *j, const char *w) {
	int l = (int)strlen(w);
	if (j->end - j->p < l || memcmp(j->p, w, l)) return 0;
	j->p += l;
	return 1;
}

static int js_hex4(Json *j) {
	if (j->end - j->p < 4) return -1;
	int u = 0;
	for (int i = 0; i < 4; i++) {
		char c = *j->p++;
		u = u * 16 + (isdigit((unsigned char)c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -100);
		if (u < 0) return -1;
	}
	return u;
}

static Value js_string(Json *j) {
	Buf b = { 0 };
	j->p++;
	while (j->p < j->end && *j->p != '"') {
		char c = *j->p++;
		if ((unsigned char)c < 32) { j->err = 1; break; }
		if (c != '\\') { buf_add(&b, &c, 1); continue; }
		if (j->p >= j->end) { j->err = 1; break; }
		c = *j->p++;
		const char *m = strchr("\"\"\\\\//b\bf\fn\nr\rt\t", c);
		if (c && m && ((m - "\"\"\\\\//b\bf\fn\nr\rt\t") & 1) == 0) { buf_add(&b, m + 1, 1); continue; }
		if (c != 'u') { j->err = 1; break; }
		int u = js_hex4(j);
		if (u < 0) { j->err = 1; break; }
		if (u >= 0xD800 && u < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
			j->p += 2;
			int lo = js_hex4(j);
			if (lo < 0xDC00 || lo > 0xDFFF) { j->err = 1; break; }
			u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
		}
		char t[4];
		int l;
		if (u < 0x80) { t[0] = (char)u; l = 1; }
		else if (u < 0x800) { t[0] = (char)(0xC0 | u >> 6); t[1] = (char)(0x80 | (u & 63)); l = 2; }
		else if (u < 0x10000) { t[0] = (char)(0xE0 | u >> 12); t[1] = (char)(0x80 | (u >> 6 & 63)); t[2] = (char)(0x80 | (u & 63)); l = 3; }
		else { t[0] = (char)(0xF0 | u >> 18); t[1] = (char)(0x80 | (u >> 12 & 63)); t[2] = (char)(0x80 | (u >> 6 & 63)); t[3] = (char)(0x80 | (u & 63)); l = 4; }
		buf_add(&b, t, l);
	}
	if (j->p >= j->end) j->err = 1;
	else j->p++;
	return buf_value(&b);
}

static Value js_value(Json *j, int depth) {
	js_ws(j);
	if (j->p >= j->end || depth > 64) { j->err = 1; return mk_null(); }
	char c = *j->p;
	if (c == '"') return js_string(j);
	if (c == '[' || c == '{') {
		int obj = c == '{';
		Value r = mk_arr();
		j->p++;
		js_ws(j);
		if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; return r; }
		while (!j->err) {
			Value key = mk_null();
			if (obj) {
				js_ws(j);
				if (j->p >= j->end || *j->p != '"') { j->err = 1; break; }
				key = js_string(j);
				js_ws(j);
				if (j->err || j->p >= j->end || *j->p != ':') { release(key); j->err = 1; break; }
				j->p++;
			}
			Value v = js_value(j, depth + 1);
			if (obj) { arr_set(r.u.a, key, v); release(key); }
			else arr_push(r.u.a, v);
			if (j->err) break;
			js_ws(j);
			if (j->p < j->end && *j->p == ',') { j->p++; continue; }
			if (j->p < j->end && *j->p == (obj ? '}' : ']')) { j->p++; return r; }
			j->err = 1;
		}
		return r;
	}
	if (js_lit(j, "true")) return mk_bool(1);
	if (js_lit(j, "false")) return mk_bool(0);
	if (js_lit(j, "null")) return mk_null();
	/* number: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)? */
	const char *s = j->p;
	int isFloat = 0;
	if (j->p < j->end && *j->p == '-') j->p++;
	if (j->p >= j->end || !isdigit((unsigned char)*j->p)) { j->err = 1; return mk_null(); }
	if (*j->p == '0') j->p++;
	else while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
	if (j->p < j->end && *j->p == '.') {
		j->p++;
		isFloat = 1;
		if (j->p >= j->end || !isdigit((unsigned char)*j->p)) { j->err = 1; return mk_null(); }
		while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
		j->p++;
		isFloat = 1;
		if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
		if (j->p >= j->end || !isdigit((unsigned char)*j->p)) { j->err = 1; return mk_null(); }
		while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
	}
	char t[40];
	int l = (int)(j->p - s);
	if (l >= (int)sizeof t) l = sizeof t - 1;
	memcpy(t, s, l);
	t[l] = 0;
	if (!isFloat) {
		long long v = strtoll(t, NULL, 10);
		if (v >= -2147483647LL - 1 && v <= 2147483647LL) return mk_int((int)v);
	}
	return mk_float(strtof(t, NULL));
}

BI(json_decode) {
	UNUSED;
	int len;
	const char *s = S(0, &len);
	Json j = { s, s + len, 0 };
	Value v = js_value(&j, 0);
	js_ws(&j);
	if (j.err || j.p != j.end) { release(v); return mk_null(); }
	return v;
}

#undef S

typedef struct { const char *name; BuiltinFn fn; signed char minA, maxA, ref; } Builtin;

static const Builtin builtins[] = {
	{ "print_r", bi_print_r, 1, 2, 0 }, { "var_dump", bi_var_dump, 1, -1, 0 },
	{ "sprintf", bi_sprintf, 1, -1, 0 }, { "printf", bi_printf, 1, -1, 0 },
	{ "number_format", bi_number_format, 1, 2, 0 },
	{ "vsprintf", bi_vsprintf, 2, 2, 0 }, { "vprintf", bi_vprintf, 2, 2, 0 },
	{ "time", bi_time, 0, 0, 0 }, { "date", bi_date, 1, 2, 0 }, { "gmdate", bi_date, 1, 2, 0 },
	{ "mktime", bi_mktime, 0, 6, 0 }, { "gmmktime", bi_mktime, 0, 6, 0 }, { "checkdate", bi_checkdate, 3, 3, 0 },
	{ "strlen", bi_strlen, 1, 1, 0 }, { "strtoupper", bi_strtoupper, 1, 1, 0 },
	{ "strtolower", bi_strtolower, 1, 1, 0 }, { "ucfirst", bi_ucfirst, 1, 1, 0 },
	{ "lcfirst", bi_lcfirst, 1, 1, 0 }, { "strrev", bi_strrev, 1, 1, 0 },
	{ "trim", bi_trim, 1, 2, 0 }, { "ltrim", bi_ltrim, 1, 2, 0 }, { "rtrim", bi_rtrim, 1, 2, 0 },
	{ "substr", bi_substr, 2, 3, 0 }, { "strpos", bi_strpos, 2, 3, 0 },
	{ "str_contains", bi_str_contains, 2, 2, 0 }, { "str_starts_with", bi_str_starts_with, 2, 2, 0 },
	{ "str_ends_with", bi_str_ends_with, 2, 2, 0 }, { "str_repeat", bi_str_repeat, 2, 2, 0 }, { "strcmp", bi_strcmp, 2, 2, 0 }, { "strcasecmp", bi_strcasecmp, 2, 2, 0 },
	{ "str_pad", bi_str_pad, 2, 4, 0 }, { "str_replace", bi_str_replace, 3, 3, 0 },
	{ "explode", bi_explode, 2, 3, 0 }, { "implode", bi_implode, 1, 2, 0 },
	{ "join", bi_implode, 1, 2, 0 },
	{ "chr", bi_chr, 1, 1, 0 }, { "ord", bi_ord, 1, 1, 0 },
	{ "intval", bi_intval, 1, 1, 0 }, { "floatval", bi_floatval, 1, 1, 0 },
	{ "strval", bi_strval, 1, 1, 0 }, { "boolval", bi_boolval, 1, 1, 0 },
	{ "is_numeric", bi_is_numeric, 1, 1, 0 }, { "is_int", bi_is_int, 1, 1, 0 },
	{ "is_float", bi_is_float, 1, 1, 0 }, { "is_string", bi_is_string, 1, 1, 0 },
	{ "is_bool", bi_is_bool, 1, 1, 0 }, { "is_array", bi_is_array, 1, 1, 0 },
	{ "is_null", bi_is_null, 1, 1, 0 }, { "gettype", bi_gettype, 1, 1, 0 },
	{ "is_object", bi_is_object, 1, 1, 0 }, { "get_class", bi_get_class, 1, 1, 0 },
	{ "get_parent_class", bi_get_parent_class, 1, 1, 0 }, { "class_exists", bi_class_exists, 1, 1, 0 },
	{ "interface_exists", bi_interface_exists, 1, 1, 0 }, { "method_exists", bi_method_exists, 2, 2, 0 },
	{ "property_exists", bi_property_exists, 2, 2, 0 }, { "get_object_vars", bi_get_object_vars, 1, 1, 0 },
	{ "abs", bi_abs, 1, 1, 0 }, { "floor", bi_floor, 1, 1, 0 }, { "ceil", bi_ceil, 1, 1, 0 },
	{ "round", bi_round, 1, 2, 0 }, { "sqrt", bi_sqrt, 1, 1, 0 }, { "pow", bi_pow, 2, 2, 0 },
	{ "sin", bi_sin, 1, 1, 0 }, { "cos", bi_cos, 1, 1, 0 }, { "tan", bi_tan, 1, 1, 0 },
	{ "asin", bi_asin, 1, 1, 0 }, { "acos", bi_acos, 1, 1, 0 },
	{ "atan", bi_atan, 1, 1, 0 }, { "atan2", bi_atan2, 2, 2, 0 }, { "exp", bi_exp, 1, 1, 0 },
	{ "log", bi_log, 1, 2, 0 }, { "log10", bi_log10, 1, 1, 0 }, { "pi", bi_pi, 0, 0, 0 },
	{ "fmod", bi_fmod, 2, 2, 0 }, { "intdiv", bi_intdiv, 2, 2, 0 },
	{ "deg2rad", bi_deg2rad, 1, 1, 0 }, { "rad2deg", bi_rad2deg, 1, 1, 0 },
	{ "min", bi_min, 1, -1, 0 }, { "max", bi_max, 1, -1, 0 },
	{ "rand", bi_rand, 0, 2, 0 }, { "mt_rand", bi_rand, 0, 2, 0 },
	{ "srand", bi_srand, 0, 1, 0 }, { "mt_srand", bi_srand, 0, 1, 0 },
	{ "getrandmax", bi_getrandmax, 0, 0, 0 }, { "mt_getrandmax", bi_getrandmax, 0, 0, 0 },
	{ "count", bi_count, 1, 1, 0 }, { "sizeof", bi_count, 1, 1, 0 },
	{ "in_array", bi_in_array, 2, 3, 0 }, { "array_search", bi_array_search, 2, 3, 0 },
	{ "array_key_exists", bi_array_key_exists, 2, 2, 0 },
	{ "array_keys", bi_array_keys, 1, 1, 0 }, { "array_values", bi_array_values, 1, 1, 0 },
	{ "array_sum", bi_array_sum, 1, 1, 0 }, { "array_reverse", bi_array_reverse, 1, 1, 0 },
	{ "array_slice", bi_array_slice, 2, 3, 0 }, { "array_merge", bi_array_merge, 1, -1, 0 },
	{ "array_fill", bi_array_fill, 3, 3, 0 }, { "range", bi_range, 2, 3, 0 },
	{ "array_push", bi_array_push, 2, -1, 1 }, { "array_pop", bi_array_pop, 1, 1, 1 },
	{ "array_shift", bi_array_shift, 1, 1, 1 }, { "array_unshift", bi_array_unshift, 2, -1, 1 },
	{ "sort", bi_sort, 1, 1, 1 }, { "rsort", bi_rsort, 1, 1, 1 }, { "shuffle", bi_shuffle, 1, 1, 1 },
	{ "usort", bi_usort, 2, 2, 1 }, { "uasort", bi_uasort, 2, 2, 1 }, { "uksort", bi_uksort, 2, 2, 1 },
	{ "array_walk", bi_array_walk, 2, 3, 1 },
	{ "array_map", bi_array_map, 2, -1, 0 }, { "array_filter", bi_array_filter, 1, 3, 0 },
	{ "array_reduce", bi_array_reduce, 2, 3, 0 },
	{ "call_user_func", bi_call_user_func, 1, -1, 0 }, { "call_user_func_array", bi_call_user_func_array, 2, 2, 0 },
	{ "is_callable", bi_is_callable, 1, 1, 0 }, { "function_exists", bi_function_exists, 1, 1, 0 },
	{ "cls", bi_cls, 0, 0, 0 }, { "locate", bi_locate, 2, 2, 0 }, { "color", bi_color, 1, 1, 0 },
	{ "input", bi_input, 0, 1, 0 }, { "inkey", bi_inkey, 0, 0, 0 }, { "joy", bi_joy, 0, 0, 0 },
	{ "swap_screens", bi_swap_screens, 0, 1, 0 }, { "touch", bi_touch, 0, 0, 0 },
	{ "pause", bi_pause, 1, 1, 0 }, { "sleep", bi_sleep, 1, 1, 0 }, { "usleep", bi_usleep, 1, 1, 0 },
	{ "gcls", bi_gcls, 0, 0, 0 }, { "gcolor", bi_gcolor, 1, 1, 0 }, { "plot", bi_plot, 2, 3, 0 },
	{ "line", bi_line, 4, 5, 0 }, { "rect", bi_rect, 4, 6, 0 }, { "circle", bi_circle, 3, 5, 0 },
	{ "paint", bi_paint, 2, 3, 0 }, { "point", bi_point, 2, 2, 0 },
	{ "sprite", bi_sprite, 4, 5, 0 }, { "sproff", bi_sproff, 0, 1, 0 }, { "sprhit", bi_sprhit, 2, 2, 0 },
	{ "stamp", bi_stamp, 3, 4, 0 }, { "sprset", bi_sprset, 4, 4, 0 },
	{ "sound", bi_sound, 1, 2, 0 }, { "sound_busy", bi_sound_busy, 0, 0, 0 },
	{ "tone", bi_tone, 1, 3, 0 }, { "noise", bi_noise, 0, 2, 0 }, { "sfx", bi_sfx, 1, 1, 0 },
	{ "file_get_contents", bi_file_get_contents, 1, 1, 0 }, { "file_put_contents", bi_file_put_contents, 2, 3, 0 },
	{ "file", bi_file, 1, 2, 0 }, { "file_exists", bi_file_exists, 1, 1, 0 }, { "unlink", bi_unlink, 1, 1, 0 },
	{ "json_encode", bi_json_encode, 1, 2, 0 }, { "json_decode", bi_json_decode, 1, 3, 0 },
	{ NULL, NULL, 0, 0, 0 }
};

static int find_builtin(const char *name) {
	for (int i = 0; builtins[i].name; i++) if (!strcasecmp(builtins[i].name, name)) return i;
	return -1;
}
static int builtin_takes_ref(int b) { return builtins[b].ref; }
static void builtin_arity(int b, int *mn, int *mx) { *mn = builtins[b].minA; *mx = builtins[b].maxA; }
static BuiltinFn builtin_fn(int b) { return builtins[b].fn; }

/* constants declared with "const NAME = value;" */
static struct { char name[NAME_MAX_LEN + 1]; Value v; } userConsts[64];
static int nuserConsts;

static void clear_user_consts(void) {
	for (int i = 0; i < nuserConsts; i++) release(userConsts[i].v);
	nuserConsts = 0;
}

static void define_user_const(const char *name, Value v) {
	for (int i = 0; i < nuserConsts; i++)
		if (!strcmp(userConsts[i].name, name)) { release(v); fail("Constant %s already defined", name); }
	if (nuserConsts >= 64) { release(v); fail("Too many constants"); }
	snprintf(userConsts[nuserConsts].name, sizeof userConsts[0].name, "%s", name);
	userConsts[nuserConsts++].v = v;
}

static int find_constant(const char *name, Value *out) {
	for (int i = 0; i < nuserConsts; i++)
		if (!strcmp(userConsts[i].name, name)) { *out = userConsts[i].v; retain(*out); return 1; }
	static const struct { const char *n; int v; } ints[] = {
		{ "PHP_INT_MAX", 2147483647 }, { "PHP_INT_MIN", -2147483647 - 1 }, { "PHP_INT_SIZE", 4 },
		{ "ARRAY_FILTER_USE_BOTH", 1 }, { "ARRAY_FILTER_USE_KEY", 2 },
		{ "STR_PAD_LEFT", 0 }, { "STR_PAD_RIGHT", 1 }, { "STR_PAD_BOTH", 2 },
		{ "JOY_UP", 1 }, { "JOY_DOWN", 2 }, { "JOY_LEFT", 4 }, { "JOY_RIGHT", 8 }, { "JOY_A", 16 },
		{ "JOY_B", 32 }, { "JOY_X", 64 }, { "JOY_Y", 128 }, { "JOY_L", 256 }, { "JOY_R", 512 },
		{ "WAVE_SQ12", 0 }, { "WAVE_SQ25", 1 }, { "WAVE_SQ50", 2 }, { "WAVE_NOISE", 3 },
		{ "FILE_IGNORE_NEW_LINES", 2 }, { "FILE_SKIP_EMPTY_LINES", 4 }, { "FILE_APPEND", 8 },
		{ "JSON_UNESCAPED_SLASHES", 64 }, { "JSON_PRETTY_PRINT", 128 },
		{ NULL, 0 }
	};
	for (int i = 0; ints[i].n; i++) if (!strcmp(ints[i].n, name)) { *out = mk_int(ints[i].v); return 1; }
	if (!strcmp(name, "PHP_EOL")) { *out = mk_str("\n", 1); return 1; }
	if (!strcmp(name, "M_PI")) { *out = mk_float(3.14159265f); return 1; }
	if (!strcmp(name, "PHP_VERSION")) { *out = mk_cstr("8.0-ds"); return 1; }
	return 0;
}

/* ------------------------------------------------------------------ */
/* program lines                                                       */
/* ------------------------------------------------------------------ */

/* built-in classes, compiled in front of every program: the exception
 * hierarchy of PHP 8 */
static const char preludeSrc[] =
	"interface Throwable {}\n"
	"trait ThrowableMethods {\n"
	"  protected $message = ''; protected $code = 0; protected $file = ''; protected $line = 0;\n"
	"  protected $previous = null;\n"
	"  public function __construct($message = '', $code = 0, $previous = null) {\n"
	"    $this->message = $message; $this->code = $code; $this->previous = $previous; }\n"
	"  public function getMessage() { return $this->message; }\n"
	"  public function getCode() { return $this->code; }\n"
	"  public function getPrevious() { return $this->previous; }\n"
	"  public function getFile() { return $this->file; }\n"
	"  public function getLine() { return $this->line; }\n"
	"  public function getTrace() { return []; }\n"
	"  public function getTraceAsString() { return '#0 {main}'; }\n"
	"  public function __toString() { return get_class($this) . ': ' . $this->message; }\n"
	"}\n"
	"class Exception implements Throwable { use ThrowableMethods; }\n"
	"class Error implements Throwable { use ThrowableMethods; }\n"
	"class ErrorException extends Exception {}\n"
	"class TypeError extends Error {}\n"
	"class ValueError extends Error {}\n"
	"class ArithmeticError extends Error {}\n"
	"class DivisionByZeroError extends ArithmeticError {}\n"
	"class ArgumentCountError extends TypeError {}\n"
	"class JsonException extends Exception {}\n"
	"class LogicException extends Exception {}\n"
	"class BadFunctionCallException extends LogicException {}\n"
	"class BadMethodCallException extends BadFunctionCallException {}\n"
	"class DomainException extends LogicException {}\n"
	"class InvalidArgumentException extends LogicException {}\n"
	"class LengthException extends LogicException {}\n"
	"class OutOfRangeException extends LogicException {}\n"
	"class RuntimeException extends Exception {}\n"
	"class OutOfBoundsException extends RuntimeException {}\n"
	"class OverflowException extends RuntimeException {}\n"
	"class RangeException extends RuntimeException {}\n"
	"class UnderflowException extends RuntimeException {}\n"
	"class UnexpectedValueException extends RuntimeException {}\n";

static void compile_prelude(void) {
	jmp_buf outer;
	memcpy(outer, errjmp, sizeof outer);
	int savedCompiling = compiling, savedErrLine = errLine;
	if (!setjmp(errjmp)) {
		compiling = 1;
		compilingPrelude = 1;
		errLine = -3;
		compile_source(preludeSrc, -1);
	}
	compilingPrelude = 0;
	compiling = savedCompiling;
	errLine = savedErrLine;
	memcpy(errjmp, outer, sizeof outer);
	throwableClass = find_class("Throwable");
}

static void reset_code(void) {
	for (int i = 0; i < nconst; i++) release(consts[i]);
	nconst = 0;
	ncode = 0;
	for (int i = 0; i < nfuncs; i++)
		for (int k = 0; k < funcs[i].nparams; k++) release(funcs[i].defs[k]);
	nfuncs = 0;
	for (int i = 0; i < nclasses; i++) free_class(&classes[i]);
	nclasses = 0;
	clear_user_consts();
	compile_prelude();
	progCode = keepCode = ncode;
	progConst = keepConst = nconst;
	progFuncs = keepFuncs = nfuncs;
	progClasses = nclasses;
}

/* back to the state right after the RUN-compiled program */
static void reset_direct_code(void) {
	for (int i = keepConst; i < nconst; i++) release(consts[i]);
	nconst = keepConst;
	ncode = keepCode;
	for (int i = keepFuncs; i < nfuncs; i++)
		for (int k = 0; k < funcs[i].nparams; k++) release(funcs[i].defs[k]);
	nfuncs = keepFuncs;
	for (int i = progClasses; i < nclasses; i++) free_class(&classes[i]);
	nclasses = progClasses;
}

static void clear_globals(int forgetNames) {
	for (int i = 0; i < nglobals; i++) { release(globals[i]); globals[i] = mk_null(); }
	if (forgetNames) nglobals = 0;
}

static void clear_files(void) {
	for (int f = 0; f < MAX_SRC; f++) {
		for (int i = 0; i < fileCount[f]; i++) free(fileLines[f][i].text);
		fileCount[f] = 0;
	}
	nsrc = 1;
	curFile = 0;
	strcpy(fileNames[0], "main.php");
	filesVersion++;
	modified = 0;
}

static void clear_program(void) {
	clear_files();
	vm_reset();
	reset_code();
	clear_globals(1);
	hide_objects();
}

/* line numbers are positions: line i has number i + 1 */
static void renumber(int from) {
	for (int i = from; i < nlines; i++) lines[i].num = i + 1;
}

static char *dup_line(const char *text) {
	size_t n = strlen(text);
	if (n > LINE_MAX_LEN) n = LINE_MAX_LEN;
	char *c = malloc(n + 1);
	if (!c) return NULL;
	memcpy(c, text, n);
	c[n] = 0;
	return c;
}

int basic_insert_line(int i, const char *text) {
	if (state != BASIC_IDLE || i < 0 || i > nlines || nlines >= MAX_LINES) return 0;
	char *c = dup_line(text);
	if (!c) return 0;
	memmove(&lines[i + 1], &lines[i], (nlines - i) * sizeof(Line));
	lines[i].text = c;
	nlines++;
	renumber(i);
	modified = 1;
	return 1;
}

int basic_set_line(int i, const char *text) {
	if (state != BASIC_IDLE || i < 0 || i >= nlines) return 0;
	char *c = dup_line(text);
	if (!c) return 0;
	free(lines[i].text);
	lines[i].text = c;
	modified = 1;
	return 1;
}

void basic_delete_line(int i) {
	if (state != BASIC_IDLE || i < 0 || i >= nlines) return;
	free(lines[i].text);
	memmove(&lines[i], &lines[i + 1], (nlines - i - 1) * sizeof(Line));
	nlines--;
	renumber(i);
	modified = 1;
}

/* tabs to two spaces, trailing spaces removed; leading ones only if !keepIndent */
static void normalize_line(const char *src, char *dst, int keepIndent) {
	int n = 0;
	if (!keepIndent) while (*src == ' ' || *src == '\t') src++;
	for (; *src && n < LINE_MAX_LEN; src++) {
		if (*src == '\t') { dst[n++] = ' '; if (n < LINE_MAX_LEN) dst[n++] = ' '; }
		else dst[n++] = *src;
	}
	while (n > 0 && dst[n - 1] == ' ') n--;
	dst[n] = 0;
}
static void normalize(const char *src, char *dst) { normalize_line(src, dst, 0); }

/* ------------------------------------------------------------------ */
/* storage                                                             */
/* ------------------------------------------------------------------ */

static void message(const char *m) {
	if (io->message) io->message(m);
	else { outs(m); out_ch('\n'); }
}

/* NAME, "NAME" or 'name' -> project NAME; "NAME.PHP" = old single file.
 * Without argument the last name is used. Returns 1 for a single file. */
static int parse_target(const char *arg, char *name, int optional) {
	if (!storageDir[0]) fail("No storage (SD card) available");
	while (*arg == ' ') arg++;
	char q = 0;
	if (*arg == '"' || *arg == '\'') q = *arg++;
	if (!*arg || *arg == q) {
		if (optional && lastFile[0]) { strcpy(name, lastFile); return 0; }
		fail("Name missing");
	}
	int n = 0, base = 0, dot = 0;
	for (; *arg && *arg != q && !(q == 0 && *arg == ' '); arg++) {
		char c = (char)toupper((unsigned char)*arg);
		if (c == '.') { if (dot || !base) fail("Bad name"); dot = 1; }
		else if (!isalnum((unsigned char)c) && c != '_' && c != '-') fail("Bad name");
		else if (!dot && ++base > 8) fail("Bad name (max. 8 characters)");
		if (n >= 12) fail("Bad name");
		name[n++] = c;
	}
	name[n] = 0;
	if (!base) fail("Bad name");
	return dot;
}

/* file name inside a project: lower case, max. 8 characters + ".php" */
static void parse_php_name(const char *arg, char *name) {
	while (*arg == ' ' || *arg == '"' || *arg == '\'') arg++;
	int n = 0, base = 0;
	for (; *arg && *arg != '"' && *arg != '\'' && *arg != ' ' && *arg != '.'; arg++) {
		char c = (char)tolower((unsigned char)*arg);
		if (!isalnum((unsigned char)c) && c != '_' && c != '-') fail("Bad file name");
		if (++base > 8) fail("Bad file name (max. 8 characters)");
		name[n++] = c;
	}
	if (*arg == '.' && strncasecmp(arg, ".php", 4)) fail("Files must end in .php");
	if (!base) fail("File name missing");
	strcpy(name + n, ".php");
}

static void file_path(char *path, const char *name) {
	snprintf(path, 160, "%.127s/%.15s", storageDir, name);
}

static int is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* write lines of file f (and for main.php the sprites and sounds) */
static int write_file(const char *path, int f, int withAssets) {
	FILE *fp = fopen(path, "w");
	if (!fp) return 0;
	int ok = fputs("<?php\n", fp) >= 0;
	for (int i = 0; i < fileCount[f] && ok; i++)
		ok = fprintf(fp, "%s\n", fileLines[f][i].text) > 0;
	/* sprites and sounds as PHP comments: "#SPR n" + 64 chars, "#SND n inst tempo note:len ..." */
	for (int sl = 0; withAssets && sl < BASIC_SPRITES && ok; sl++) {
		char hex[65];
		int used = 0;
		for (int k = 0; k < 64; k++) {
			int c = sprites[sl][k];
			hex[k] = c < 0 ? '.' : "0123456789ABCDEF"[c];
			used |= c >= 0;
		}
		hex[64] = 0;
		if (used) ok = fprintf(fp, "#SPR %d %s\n", sl, hex) > 0;
	}
	for (int sl = 0; withAssets && sl < BASIC_SOUNDS && ok; sl++) {
		const BasicSound *sd = &sounds[sl];
		if (!sd->count) continue;
		ok = fprintf(fp, "#SND %d %d %d", sl, sd->inst, sd->tempo) > 0;
		for (int k = 0; k < sd->count && ok; k++) ok = fprintf(fp, " %d:%d", sd->note[k], sd->len[k]) > 0;
		if (ok) ok = fputc('\n', fp) != EOF;
	}
	if (fclose(fp) != 0) ok = 0;
	return ok;
}

static void cmd_save(const char *arg) {
	char name[16], path[160], fpath[200], msg[48];
	int single = parse_target(arg, name, 1);
	file_path(path, name);
	if (single) {
		if (nsrc > 1) fail("Programs with several files are saved as a folder: SAVE \"NAME\"");
		if (!write_file(path, 0, 1)) fail("Disk error while saving %s", name);
		strcpy(lastFile, name);
		modified = 0;
		snprintf(msg, sizeof msg, "SAVED %s", name);
		message(msg);
		return;
	}
	mkdir(path, 0777);
	if (!is_dir(path)) fail("Cannot create the folder %s", name);
	for (int f = 0; f < nsrc; f++) {
		snprintf(fpath, sizeof fpath, "%.150s/%.15s", path, fileNames[f]);
		if (!write_file(fpath, f, f == 0)) fail("Disk error while saving %s", fileNames[f]);
	}
	/* remove .php files that are no longer part of the program */
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		char stale[MAX_SRC * 2][16];
		int ns = 0;
		while ((e = readdir(d)) && ns < MAX_SRC * 2) {
			size_t l = strlen(e->d_name);
			if (l < 5 || l > 12 || strcasecmp(e->d_name + l - 4, ".php")) continue;
			if (find_src(e->d_name) < 0) strcpy(stale[ns++], e->d_name);
		}
		closedir(d);
		for (int i = 0; i < ns; i++) {
			snprintf(fpath, sizeof fpath, "%.150s/%.15s", path, stale[i]);
			remove(fpath);
		}
	}
	strcpy(lastFile, name);
	modified = 0;
	snprintf(msg, sizeof msg, "SAVED %s (%d FILE%s)", name, nsrc, nsrc == 1 ? "" : "S");
	message(msg);
}

/* read a .php file into project file f; sprites/sounds only into main.php */
static void read_file(FILE *fp, int f) {
	char raw[512], buf[LINE_MAX_LEN + 1];
	int spritesSeen = 0, soundsSeen = 0, first = 1;
	while (fgets(raw, sizeof raw, fp)) {
		raw[strcspn(raw, "\r\n")] = 0;
		if (f == 0 && !strncmp(raw, "#SND ", 5)) {
			if (!soundsSeen++) for (int k = 0; k < BASIC_SOUNDS; k++) basic_sound_clear(k);
			char *e;
			long sl = strtol(raw + 5, &e, 10);
			if (sl < 0 || sl >= BASIC_SOUNDS) continue;
			BasicSound *sd = &sounds[sl];
			long inst = strtol(e, &e, 10), tempo = strtol(e, &e, 10);
			sd->inst = (unsigned char)(inst >= 0 && inst < SND_INSTRUMENTS ? inst : SND_SQ50);
			sd->tempo = (unsigned char)(tempo >= 40 && tempo <= 250 ? tempo : 120);
			sd->count = 0;
			while (sd->count < BASIC_SOUND_LEN) {
				long nt = strtol(e, &e, 10);
				if (*e != ':') break;
				long ln = strtol(e + 1, &e, 10);
				if (nt != 0 && (nt < 24 || nt > 108)) nt = 0;
				if (ln < 1 || ln > 16) ln = 4;
				sd->note[sd->count] = (unsigned char)nt;
				sd->len[sd->count] = (unsigned char)ln;
				sd->count++;
			}
			continue;
		}
		if (f == 0 && !strncmp(raw, "#SPR ", 5)) {
			if (!spritesSeen++) {
				memset(sprites, -1, sizeof sprites);
				for (int k = 0; k < BASIC_OBJECTS; k++) objects[k].dirty = 1;
			}
			char *e;
			long sl = strtol(raw + 5, &e, 10);
			while (*e == ' ') e++;
			if (sl < 0 || sl >= BASIC_SPRITES || strlen(e) < 64) continue;
			for (int k = 0; k < 64; k++) {
				char c = (char)toupper((unsigned char)e[k]);
				sprites[sl][k] = (signed char)(isdigit((unsigned char)c) ? c - '0' :
				                 (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1);
			}
			continue;
		}
		normalize_line(raw, buf, 1);
		if (first && !strcasecmp(buf + strspn(buf, " "), "<?php")) { first = 0; continue; }
		first = 0;
		if (fileCount[f] >= MAX_LINES) break;
		Line *ln = &fileLines[f][fileCount[f]];
		ln->text = dup_line(buf);
		if (!ln->text) break;
		ln->num = fileCount[f] + 1;
		fileCount[f]++;
	}
	while (fileCount[f] > 0) {
		Line *ln = &fileLines[f][fileCount[f] - 1];
		if (ln->text[strspn(ln->text, " ")]) break;
		free(ln->text);
		fileCount[f]--;
	}
}

static int cmp_names(const void *x, const void *y) { return strcmp((const char *)x, (const char *)y); }

static void cmd_load(const char *arg) {
	char name[16], path[160], fpath[200], msg[48];
	int single = parse_target(arg, name, 0);
	file_path(path, name);
	if (!single && is_dir(path)) {
		static char names[MAX_SRC][16];
		int n = 0;
		DIR *d = opendir(path);
		if (!d) fail("Cannot read %s", name);
		struct dirent *e;
		while ((e = readdir(d))) {
			size_t l = strlen(e->d_name);
			if (l < 5 || l > 12 || strcasecmp(e->d_name + l - 4, ".php") || !strcasecmp(e->d_name, "main.php")) continue;
			if (n >= MAX_SRC - 1) break;
			for (size_t k = 0; k <= l; k++) names[n][k] = (char)tolower((unsigned char)e->d_name[k]);
			n++;
		}
		closedir(d);
		qsort(names, n, 16, cmp_names);
		clear_program();
		for (int i = 0; i < n; i++) strcpy(fileNames[1 + i], names[i]);
		nsrc = 1 + n;
		for (int f = 0; f < nsrc; f++) {
			snprintf(fpath, sizeof fpath, "%.150s/%.15s", path, fileNames[f]);
			FILE *fp = fopen(fpath, "r");
			if (!fp) continue;           /* a missing main.php stays empty */
			read_file(fp, f);
			fclose(fp);
		}
		filesVersion++;
		modified = 0;
		strcpy(lastFile, name);
		snprintf(msg, sizeof msg, "LOADED %s (%d FILE%s)", name, nsrc, nsrc == 1 ? "" : "S");
		message(msg);
		return;
	}
	char shown[16];
	strcpy(shown, name);
	if (!single) {
		strcat(name, ".PHP");
		file_path(path, name);
	}
	FILE *fp = fopen(path, "r");
	if (!fp) fail("Program %s not found", shown);
	clear_program();
	read_file(fp, 0);
	fclose(fp);
	char *dot = strchr(name, '.');
	if (dot) *dot = 0;
	strcpy(lastFile, name);
	modified = 0;
	snprintf(msg, sizeof msg, "LOADED %s (%d LINE%s)", name, fileCount[0], fileCount[0] == 1 ? "" : "S");
	message(msg);
}

/* saved programs: project folders ("NAME") and old single files
 * ("NAME.PHP", hidden when a folder of that name exists), sorted */
int basic_list_files(char names[][16], int max) {
	int count = 0;
	if (!storageDir[0]) return -1;
	DIR *d = opendir(storageDir);
	if (!d) return -1;
	struct dirent *e;
	char path[200];
	while ((e = readdir(d)) && count < max) {
		size_t l = strlen(e->d_name);
		if (e->d_name[0] == '.' || l > 12) continue;
		snprintf(path, sizeof path, "%.127s/%.15s", storageDir, e->d_name);
		if (is_dir(path)) {
			if (l > 8) continue;
			snprintf(path, sizeof path, "%.127s/%.15s/main.php", storageDir, e->d_name);
			FILE *fp = fopen(path, "r");
			if (!fp) continue;
			fclose(fp);
		} else if (l < 5 || strcasecmp(e->d_name + l - 4, ".PHP")) continue;
		for (size_t k = 0; k <= l; k++) names[count][k] = (char)toupper((unsigned char)e->d_name[k]);
		count++;
	}
	closedir(d);
	/* hide NAME.PHP when the folder NAME exists */
	for (int i = 0; i < count; i++) {
		char *dot = strchr(names[i], '.');
		if (!dot) continue;
		for (int j = 0; j < count; j++)
			if (j != i && !strchr(names[j], '.') && (int)strlen(names[j]) == dot - names[i] &&
			    !strncmp(names[j], names[i], dot - names[i])) {
				memmove(names[i], names[count - 1], 16);
				count--;
				i--;
				break;
			}
	}
	qsort(names, count, 16, cmp_names);
	return count;
}

static void cmd_dir(void) {
	static char names[BASIC_MAX_FILES][16];
	if (!storageDir[0]) fail("No storage (SD card) available");
	int count = basic_list_files(names, BASIC_MAX_FILES);
	if (count < 0) fail("Cannot read the directory");
	for (int i = 0; i < count; i++) {
		char path[200], line[48];
		struct stat st;
		file_path(path, names[i]);
		if (!strchr(names[i], '.')) {
			int nf = 0;
			DIR *d = opendir(path);
			if (d) {
				struct dirent *e;
				while ((e = readdir(d))) {
					size_t l = strlen(e->d_name);
					if (l >= 5 && !strcasecmp(e->d_name + l - 4, ".php")) nf++;
				}
				closedir(d);
			}
			snprintf(line, sizeof line, "%-12.12s %2d FILE%s", names[i], nf, nf == 1 ? "" : "S");
		} else {
			long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
			snprintf(line, sizeof line, "%-12.12s %6ld", names[i], size);
		}
		outs(line);
		out_ch('\n');
	}
	char b[24];
	snprintf(b, sizeof b, "%d PROGRAM%s", count, count == 1 ? "" : "S");
	outs(b);
	out_ch('\n');
}

static void cmd_delete(const char *arg) {
	char name[16], path[160], fpath[200], msg[40];
	int single = parse_target(arg, name, 0);
	file_path(path, name);
	if (!single && is_dir(path)) {
		DIR *d = opendir(path);
		char names[MAX_SRC * 2][16];
		int n = 0;
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) && n < MAX_SRC * 2)
				if (e->d_name[0] != '.' && strlen(e->d_name) < 16) strcpy(names[n++], e->d_name);
			closedir(d);
		}
		for (int i = 0; i < n; i++) {
			snprintf(fpath, sizeof fpath, "%.150s/%.15s", path, names[i]);
			remove(fpath);
		}
		if (rmdir(path) != 0) fail("Cannot delete the folder %s", name);
	} else {
		if (!single) { strcat(name, ".PHP"); file_path(path, name); }
		if (remove(path) != 0) fail("%s not found", name);
		char *dot = strchr(name, '.');
		if (dot) *dot = 0;
	}
	if (!strcmp(name, lastFile)) lastFile[0] = 0;
	snprintf(msg, sizeof msg, "DELETED %s", name);
	message(msg);
}

static void cmd_list(const char *arg) {
	int from = atoi(arg);
	if (nsrc > 1) { outs("--- "); outs(fileNames[curFile]); out_ch('\n'); }
	for (int i = 0; i < nlines; i++) {
		if (lines[i].num < from) continue;
		char b[12];
		snprintf(b, sizeof b, "%d ", lines[i].num);
		outs(b);
		outs(lines[i].text);
		out_ch('\n');
	}
}

/* --- files of the program --- */

static void cmd_edit(const char *arg) {
	char name[16];
	parse_php_name(arg, name);
	int f = find_src(name);
	if (f < 0) {
		if (nsrc >= MAX_SRC) fail("Too many files (max. %d)", MAX_SRC);
		f = nsrc++;
		strcpy(fileNames[f], name);
		fileCount[f] = 0;
		filesVersion++;
		modified = 1;
	}
	curFile = f;
	editRequest = f;
}

static void cmd_files(void) {
	for (int f = 0; f < nsrc; f++) {
		char b[40];
		snprintf(b, sizeof b, "%c %-12.12s %4d LINE%s", f == curFile ? '*' : ' ', fileNames[f],
		         fileCount[f], fileCount[f] == 1 ? "" : "S");
		outs(b);
		out_ch('\n');
	}
}

static void cmd_rmfile(const char *arg) {
	char name[16], msg[40];
	parse_php_name(arg, name);
	int f = find_src(name);
	if (f < 0) fail("File %s not found", name);
	if (f == 0) fail("main.php cannot be removed");
	for (int i = 0; i < fileCount[f]; i++) free(fileLines[f][i].text);
	for (int k = f; k < nsrc - 1; k++) {
		memcpy(fileLines[k], fileLines[k + 1], sizeof fileLines[k]);
		fileCount[k] = fileCount[k + 1];
		strcpy(fileNames[k], fileNames[k + 1]);
	}
	nsrc--;
	fileCount[nsrc] = 0;
	if (curFile >= f) curFile = curFile > 0 ? curFile - 1 : 0;
	filesVersion++;
	modified = 1;
	snprintf(msg, sizeof msg, "REMOVED %s", name);
	message(msg);
}

/* RENAME "new" renames the file open in the editor, RENAME "old" "new" another one */
static void cmd_rename(const char *arg) {
	char a[16], b[16], msg[48];
	parse_php_name(arg, a);
	const char *second = arg;
	while (*second == ' ' || *second == '"' || *second == '\'') second++;
	while (*second && *second != ' ' && *second != '"' && *second != '\'') second++;
	while (*second == '"' || *second == '\'' || *second == ' ' || *second == ',') second++;
	int f;
	if (*second) { parse_php_name(second, b); f = find_src(a); if (f < 0) fail("File %s not found", a); }
	else { strcpy(b, a); f = curFile; }
	if (f == 0) fail("main.php cannot be renamed");
	if (find_src(b) >= 0) fail("%s already exists", b);
	snprintf(msg, sizeof msg, "RENAMED %s TO %s", fileNames[f], b);
	strcpy(fileNames[f], b);
	filesVersion++;
	modified = 1;
	message(msg);
}

/* ------------------------------------------------------------------ */
/* running                                                             */
/* ------------------------------------------------------------------ */

static char pendingCmd[LINE_MAX_LEN + 1];   /* editor command or direct code */

static void start_vm(int addr) {
	vm_reset();
	nhandlers = 0;
	frames[0].retpc = -1;
	frames[0].base = 0;
	frames[0].func = -1;
	frames[0].thisv = mk_null();
	frames[0].cls = frames[0].scls = -1;
	frames[0].ctor = 0;
	nframes = 1;
	pc = curPc = addr;
	state = BASIC_RUNNING;
}

static void cmd_run(void) {
	clear_globals(1);
	hide_objects();
	reset_code();
	free_src_bufs();
	compiling = 1;
	errLine = -3;
	int start = compile_source(file_source(0), 0);
	compiling = 0;
	free_src_bufs();
	progCode = ncode;
	progConst = nconst;
	progFuncs = nfuncs;
	progClasses = nclasses;
	keepCode = ncode;
	keepConst = nconst;
	keepFuncs = nfuncs;
	start_vm(start);
}

static void cmd_direct(const char *src) {
	reset_direct_code();
	free_src_bufs();
	int start = ncode;
	compiling = 1;
	errLine = -3;
	directCompile = 1;
	closuresMade = 0;
	compile_source(src, -1);
	directCompile = 0;
	compiling = 0;
	free_src_bufs();
	if (closuresMade) { keepCode = ncode; keepConst = nconst; keepFuncs = nfuncs; }
	start_vm(start);
}

/* first word of the line is an editor command? returns its argument */
static const char *command_arg(const char *s, const char *cmd) {
	size_t n = strlen(cmd);
	if (strncasecmp(s, cmd, n)) return NULL;
	char c = s[n];
	if (c == 0) return s + n;
	if (c == ' ' || c == '"' || c == '\'') return s + n;
	return NULL;
}

/* commands without arguments: only the bare word (so "new Foo()" is PHP) */
static int bare_command(const char *s, const char *cmd) {
	size_t n = strlen(cmd);
	if (strncasecmp(s, cmd, n)) return 0;
	s += n;
	while (*s == ' ') s++;
	return *s == 0;
}

static void run_pending(void) {
	const char *s = pendingCmd, *arg;
	state = BASIC_IDLE;
	if (bare_command(s, "RUN")) { cmd_run(); return; }
	if (bare_command(s, "NEW")) { clear_program(); return; }
	if (bare_command(s, "CLR")) { clear_globals(0); return; }
	if ((arg = command_arg(s, "LIST"))) { cmd_list(arg); return; }
	if ((arg = command_arg(s, "SAVE"))) { cmd_save(arg); return; }
	if ((arg = command_arg(s, "LOAD"))) { cmd_load(arg); return; }
	if (bare_command(s, "DIR")) { cmd_dir(); return; }
	if ((arg = command_arg(s, "DELETE"))) { cmd_delete(arg); return; }
	if (bare_command(s, "EXIT")) { hide_objects(); if (io->exit) io->exit(); return; }
	if ((arg = command_arg(s, "EDIT"))) { cmd_edit(arg); return; }
	if (bare_command(s, "FILES")) { cmd_files(); return; }
	if ((arg = command_arg(s, "RMFILE"))) { cmd_rmfile(arg); return; }
	if ((arg = command_arg(s, "RENAME"))) { cmd_rename(arg); return; }
	cmd_direct(s);
}

static void report_error(void) {
	compiling = 0;
	directCompile = 0;
	int line = errIsParse ? errLine : (curPc >= 0 && curPc < ncode && state == BASIC_RUNNING ? codeLine[curPc] : 0);
	if (excPending) {
		/* uncaught exception: "Uncaught Exception: message" at the line of new */
		Value e = pendingExc;
		int mi = find_prop(e.u.o->cls, "message"), li = find_prop(e.u.o->cls, "line");
		int ml = 0;
		const char *m = mi >= 0 && e.u.o->props[mi].t == T_STR ? e.u.o->props[mi].u.s->s : "";
		ml = (int)strlen(m);
		snprintf(errText, sizeof errText, ml ? "Uncaught %s: %s" : "Uncaught %s%s", classes[e.u.o->cls].name, m);
		if (li >= 0 && e.u.o->props[li].t == T_INT && e.u.o->props[li].u.i > 0) {
			int f = 0;
			int fi = find_prop(e.u.o->cls, "file");
			if (fi >= 0 && e.u.o->props[fi].t == T_STR)
				for (int k = 0; k < nsrc; k++) if (!strcmp(fileNames[k], e.u.o->props[fi].u.s->s)) f = k;
			line = (f << 10) | e.u.o->props[li].u.i;
		}
		release(pendingExc);
		excPending = 0;
	}
	const char *kind = errIsParse ? "Parse error" : "Error";
	lastErrorLine = line > 0 ? (line & 1023) : -1;
	lastErrorFile = line > 0 ? (line >> 10) : 0;
	if (lastErrorFile >= nsrc) lastErrorFile = 0;
	if (line > 0 && nsrc > 1)
		snprintf(lastError, sizeof lastError, "%s in %s:%d: %s", kind, fileNames[lastErrorFile], line & 1023, errText);
	else if (line > 0) snprintf(lastError, sizeof lastError, "%s in line %d: %s", kind, line & 1023, errText);
	else snprintf(lastError, sizeof lastError, "%s: %s", kind, errText);
	if (errIsParse && !(pendingCmd[0] && !bare_command(pendingCmd, "RUN"))) {
		/* broken program: drop the half compiled code */
		reset_code();
	} else if (errIsParse) reset_direct_code();
	if (col) out_ch('\n');
	outs(lastError);
	out_ch('\n');
	vm_reset();
	state = BASIC_IDLE;
	waitFrames = waitSound = waitTone = 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void basic_init(const BasicIO *bio) {
	io = bio;
	clear_program();
	memset(sprites, -1, sizeof sprites);
	for (int k = 0; k < BASIC_SOUNDS; k++) basic_sound_clear(k);
	state = BASIC_IDLE;
	lastError[0] = 0;
}

BasicState basic_state(void) { return state; }
const char *basic_last_error(void) { return lastError; }
int basic_error_line(void) { return lastErrorLine; }
int basic_error_file(void) { return lastErrorFile; }

int basic_file_count(void) { return nsrc; }
const char *basic_file_name(int i) { return (i >= 0 && i < nsrc) ? fileNames[i] : ""; }
int basic_current_file(void) { return curFile; }
void basic_select_file(int i) { if (i >= 0 && i < nsrc && state == BASIC_IDLE) curFile = i; }
int basic_files_version(void) { return filesVersion; }
int basic_edit_request(void) { int r = editRequest; editRequest = -1; return r; }
void basic_seed(unsigned s) { rndState = s ? s : 1; }
int basic_line_count(void) { return nlines; }
int basic_line_number(int i) { return (i >= 0 && i < nlines) ? lines[i].num : -1; }
const char *basic_line_text(int i) { return (i >= 0 && i < nlines) ? lines[i].text : ""; }

int basic_find_index(int num) {
	int lo = 0, hi = nlines;
	while (lo < hi) { int m = (lo + hi) / 2; if (lines[m].num < num) lo = m + 1; else hi = m; }
	return lo;
}

int basic_current_line(void) {
	if (state == BASIC_IDLE || pendingCmd[0] || curPc < 0 || curPc >= ncode) return -1;
	int l = codeLine[curPc] & 1023;
	return l > 0 && curPc < progCode ? l : -1;
}

int basic_enter(const char *src, int *lineNum) {
	char buf[LINE_MAX_LEN + 1];
	if (state != BASIC_IDLE) return BASIC_ENTER_ERROR;
	normalize(src, buf);
	(void)lineNum;
	if (!buf[0]) return BASIC_ENTER_EMPTY;
	strcpy(pendingCmd, buf);
	lastError[0] = 0;
	lastErrorLine = -1;
	waitFrames = waitSound = waitTone = 0;
	state = BASIC_RUNNING;
	return BASIC_ENTER_EXEC;
}

int basic_run_slice(int maxStatements) {
	volatile int done = 0;
	if (state != BASIC_RUNNING) return 0;
	if (setjmp(errjmp) && !handle_throw(1)) { report_error(); pendingCmd[0] = 0; return 0; }
	if (pendingCmd[0]) {
		run_pending();
		pendingCmd[0] = 0;
		return 1;
	}
	if (waitSound) {
		if (io->sound_busy && io->sound_busy()) return 0;
		waitSound = 0;
	}
	if (waitTone) {
		if (io->tone_queued && io->tone_queued() >= TONE_BLOCK) return 0;
		waitTone = 0;
	}
	int limit = maxStatements * 32;
	while (done < limit && state == BASIC_RUNNING && waitFrames == 0 && !waitSound && !waitTone) {
		exec_one();
		done++;
	}
	return done;
}

void basic_frame(void) {
	if (waitFrames > 0) waitFrames--;
}

void basic_input(const char *line) {
	if (state != BASIC_INPUT) return;
	if (sp > 0) { release(stack[sp - 1]); stack[sp - 1] = mk_cstr(line); }
	col = 0;
	state = BASIC_RUNNING;
}

void basic_break(void) {
	if (state == BASIC_IDLE) return;
	char b[40];
	int ln = basic_current_line();
	lastErrorLine = ln;
	lastErrorFile = ln > 0 ? (codeLine[curPc] >> 10) : 0;
	if (lastErrorFile >= nsrc) lastErrorFile = 0;
	if (ln > 0 && nsrc > 1) snprintf(b, sizeof b, "Break in %s:%d", fileNames[lastErrorFile], ln);
	else if (ln > 0) snprintf(b, sizeof b, "Break in line %d", ln);
	else strcpy(b, "Break");
	if (col) out_ch('\n');
	outs(b);
	out_ch('\n');
	strcpy(lastError, b);
	vm_reset();
	pendingCmd[0] = 0;
	state = BASIC_IDLE;
	waitFrames = waitSound = waitTone = 0;
}

int basic_sprite_get(int sl, int x, int y) {
	if (sl < 0 || sl >= BASIC_SPRITES || x < 0 || x > 7 || y < 0 || y > 7) return -1;
	return sprites[sl][y * 8 + x];
}

void basic_sprite_set(int sl, int x, int y, int c) {
	if (sl < 0 || sl >= BASIC_SPRITES || x < 0 || x > 7 || y < 0 || y > 7 || c < -1 || c > 15) return;
	if (sprites[sl][y * 8 + x] == c) return;
	sprites[sl][y * 8 + x] = (signed char)c;
	slot_changed(sl);
}

BasicObject *basic_object(int o) { return (o >= 0 && o < BASIC_OBJECTS) ? &objects[o] : NULL; }
BasicSound *basic_sound(int sl) { return (sl >= 0 && sl < BASIC_SOUNDS) ? &sounds[sl] : NULL; }

void basic_sound_clear(int sl) {
	if (sl < 0 || sl >= BASIC_SOUNDS) return;
	memset(&sounds[sl], 0, sizeof sounds[sl]);
	sounds[sl].inst = SND_SQ50;
	sounds[sl].tempo = 120;
}

void basic_set_storage(const char *dir) {
	if (dir) { strncpy(storageDir, dir, sizeof storageDir - 1); storageDir[sizeof storageDir - 1] = 0; }
	else storageDir[0] = 0;
}

int basic_has_storage(void) { return storageDir[0] != 0; }
const char *basic_storage_dir(void) { return storageDir; }
const char *basic_program_name(void) { return lastFile; }
int basic_modified(void) { return modified && (nsrc > 1 || fileCount[0] > 0); }
