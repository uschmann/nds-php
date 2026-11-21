/* refactor.c - rename, go to definition, find usages (see refactor.h) */
#include "refactor.h"
#include "basic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */
/* ------------------------------------------------------------------ */

enum { K_VAR, K_ID, K_TEXT };               /* $x, name, word in string/comment */
/* the significant token before a name */
enum { A_NONE, A_ARROW, A_DCOLON, A_FUNCTION, A_CONST, A_CASE, A_CLASSKW };

typedef struct {
	int file, line, col, len;               /* name without "$"                 */
	int kind, after, scope;
	int classBody;                          /* directly in a class body         */
	int promoted;                           /* constructor property promotion   */
	int dollar;                             /* K_TEXT: "$" before the word      */
	const char *name;                       /* not terminated: len characters   */
} Tok;

typedef void (*TokFn)(const Tok *t, void *ctx);

enum { BT_OTHER, BT_CLASS, BT_FUNC };
#define DEPTH_MAX 64

static int is_id_start(char c) { return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
static int is_id(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static int word_is(const char *w, int n, const char *kw) { return (int)strlen(kw) == n && !strncasecmp(w, kw, n); }

static const char *const keywords[] = {
	"echo", "print", "if", "else", "elseif", "while", "do", "for", "foreach", "as", "switch",
	"case", "default", "break", "continue", "function", "return", "global", "true", "false",
	"null", "and", "or", "xor", "isset", "unset", "empty", "exit", "die", "array", "list",
	"require", "require_once", "include", "include_once", "class", "interface", "trait",
	"extends", "implements", "new", "clone", "instanceof", "public", "protected", "private",
	"static", "abstract", "final", "const", "use", "self", "parent", "readonly", "var",
	"try", "catch", "finally", "throw", "match", "enum", "fn", "declare", "int", "float",
	"string", "bool", "mixed", "void", "never", "callable", "iterable", "object", NULL
};

static int is_keyword(const char *w, int n) {
	for (int i = 0; keywords[i]; i++) if (word_is(w, n, keywords[i])) return 1;
	return 0;
}

/* walks one file and reports every name; function scopes get ids that are
 * unique over all files (file * 4096 + n) */
static void scan_file(int f, TokFn fn, void *ctx) {
	int btype[DEPTH_MAX], bscope[DEPTH_MAX], bparen[DEPTH_MAX], depth = 0;
	int paren = 0, scope = 0, nscopes = 0;
	int after = A_NONE, prevNew = 0;
	int pendClass = 0;                       /* class keyword seen, waiting for { */
	int pendFunc = 0, pendSaved = 0;         /* named function: waiting for { or ; */
	int funcParen = -1, funcInClass = 0, paramVis = 0;
	int inComment = 0, inStr = 0;            /* block comment / open quote across lines */
	int nl = basic_file_line_count(f);
	(void)prevNew;
	for (int ln = 0; ln < nl; ln++) {
		const char *s = basic_file_line_text(f, ln);
		int l = (int)strlen(s), i = 0;
		while (i < l) {
			char c = s[i];
			/* --- comments and strings: only words --- */
			if (inComment || inStr == '\'' || inStr == '"') {
				if (inComment && c == '*' && s[i + 1] == '/') { inComment = 0; i += 2; continue; }
				if (inStr && c == '\\') { i += 2; continue; }
				if (inStr && c == inStr) { inStr = 0; i++; after = A_NONE; continue; }
				if (inStr == '"' && c == '$' && i + 1 < l && is_id_start(s[i + 1])) {
					/* "$x" and "$x->y" are code */
					int b = i + 1, e = b;
					while (e < l && is_id(s[e])) e++;
					Tok t = { f, ln, b, e - b, K_VAR, A_NONE, scope, 0, 0, 0, s + b };
					fn(&t, ctx);
					i = e;
					if (i + 2 < l && s[i] == '-' && s[i + 1] == '>' && is_id_start(s[i + 2])) {
						b = i + 2; e = b;
						while (e < l && is_id(s[e])) e++;
						Tok m = { f, ln, b, e - b, K_ID, A_ARROW, scope, 0, 0, 0, s + b };
						fn(&m, ctx);
						i = e;
					}
					continue;
				}
				if (is_id_start(c) && (i == 0 || !is_id(s[i - 1]))) {
					int e = i;
					while (e < l && is_id(s[e])) e++;
					Tok t = { f, ln, i, e - i, K_TEXT, A_NONE, scope, 0, 0, i > 0 && s[i - 1] == '$', s + i };
					fn(&t, ctx);
					i = e;
					continue;
				}
				i++;
				continue;
			}
			if (c == '#' || (c == '/' && s[i + 1] == '/')) {      /* line comment */
				i += c == '#' ? 1 : 2;
				while (i < l) {
					if (is_id_start(s[i]) && !is_id(s[i - 1])) {
						int e = i;
						while (e < l && is_id(s[e])) e++;
						Tok t = { f, ln, i, e - i, K_TEXT, A_NONE, scope, 0, 0, s[i - 1] == '$', s + i };
						fn(&t, ctx);
						i = e;
					} else i++;
				}
				break;
			}
			if (c == '/' && s[i + 1] == '*') { inComment = 1; i += 2; continue; }
			if (c == '\'' || c == '"') { inStr = c; i++; continue; }
			if (c == ' ' || c == '\t') { i++; continue; }

			int top = depth ? btype[depth - 1] : BT_OTHER;
			int classBody = top == BT_CLASS && paren == bparen[depth - 1];

			/* --- variables --- */
			if (c == '$' && i + 1 < l && is_id_start(s[i + 1])) {
				int b = i + 1, e = b;
				while (e < l && is_id(s[e])) e++;
				int promoted = pendFunc && funcInClass && paren == funcParen + 1 && paramVis;
				Tok t = { f, ln, b, e - b, K_VAR, after == A_DCOLON ? A_DCOLON : A_NONE, scope,
				          classBody, promoted, 0, s + b };
				fn(&t, ctx);
				after = A_NONE;
				i = e;
				continue;
			}
			/* --- names --- */
			if (is_id_start(c) || (c == '\\' && i + 1 < l && is_id_start(s[i + 1]))) {
				if (c == '\\') i++;
				int b = i, e = b;
				while (e < l && is_id(s[e])) e++;
				const char *w = s + b;
				int n = e - b;
				if (after != A_ARROW && after != A_DCOLON && is_keyword(w, n)) {
					if (word_is(w, n, "function")) after = A_FUNCTION;
					else if (word_is(w, n, "const")) after = classBody || !depth ? A_CONST : A_NONE;
					else if (word_is(w, n, "case")) after = classBody ? A_CASE : A_NONE;
					else if (word_is(w, n, "class") || word_is(w, n, "interface") || word_is(w, n, "trait") ||
					         word_is(w, n, "enum")) { after = A_CLASSKW; pendClass = 1; }
					else if (word_is(w, n, "public") || word_is(w, n, "protected") || word_is(w, n, "private") ||
					         word_is(w, n, "readonly")) {
						if (pendFunc && paren == funcParen + 1) paramVis = 1;
					} else if (after != A_CONST && !(after == A_FUNCTION && word_is(w, n, "static"))) after = A_NONE;
					i = e;
					continue;
				}
				/* "enum Suit: string" is not a call; a named function opens a scope */
				Tok t = { f, ln, b, n, K_ID, after, scope, classBody, 0, 0, w };
				fn(&t, ctx);
				if (after == A_FUNCTION) {
					pendFunc = 1;
					pendSaved = scope;
					funcInClass = classBody;
					funcParen = paren;
					paramVis = 0;
					scope = f * 4096 + ++nscopes;
				}
				after = after == A_CONST ? A_CONST : A_NONE;   /* const A = 1, B = 2 */
				i = e;
				continue;
			}
			/* --- operators and brackets --- */
			if (c == '-' && s[i + 1] == '>') { after = A_ARROW; i += 2; continue; }
			if (c == '?' && s[i + 1] == '-' && s[i + 2] == '>') { after = A_ARROW; i += 3; continue; }
			if (c == ':' && s[i + 1] == ':') { after = A_DCOLON; i += 2; continue; }
			if (c == '&' && after == A_FUNCTION) { i++; continue; }  /* function &name() */
			if (c == '(') paren++;
			else if (c == ')') { if (paren > 0) paren--; }
			else if (c == ',') { if (pendFunc && paren == funcParen + 1) paramVis = 0; }
			else if (c == '{') {
				if (depth < DEPTH_MAX) {
					btype[depth] = pendClass ? BT_CLASS : (pendFunc && paren == funcParen) ? BT_FUNC : BT_OTHER;
					bscope[depth] = pendSaved;
					bparen[depth] = paren;
					if (btype[depth] == BT_FUNC) pendFunc = 0;
					depth++;
				}
				pendClass = 0;
			} else if (c == '}') {
				if (depth > 0) {
					depth--;
					if (btype[depth] == BT_FUNC) scope = bscope[depth];
				}
			} else if (c == ';') {
				if (pendFunc && paren == funcParen) { pendFunc = 0; scope = pendSaved; }   /* abstract */
				if (after == A_CONST) after = A_NONE;
			}
			if (c != ',' || after != A_CONST) after = A_NONE;
			i++;
		}
		if (inStr) { /* strings may span lines */ }
	}
}

static void scan_all(TokFn fn, void *ctx) {
	int nf = basic_file_count();
	for (int f = 0; f < nf; f++) scan_file(f, fn, ctx);
}

/* ------------------------------------------------------------------ */
/* what a token means                                                  */
/* ------------------------------------------------------------------ */

static int tok_eq(const Tok *t, const char *name, int ci) {
	return (int)strlen(name) == t->len && !(ci ? strncasecmp(t->name, name, t->len) : strncmp(t->name, name, t->len));
}

static int member_decl(const Tok *t) {
	return t->promoted || (t->classBody && (t->kind == K_VAR ||
	       (t->kind == K_ID && (t->after == A_FUNCTION || t->after == A_CONST || t->after == A_CASE))));
}

static int is_member(const Tok *t) {
	if (t->kind == K_TEXT) return 0;
	if (t->after == A_ARROW || t->after == A_DCOLON) return 1;
	return member_decl(t);
}

/* is this token one of the target's places in code? */
static int matches(const RefTarget *r, const Tok *t) {
	if (t->kind == K_TEXT) return 0;
	switch (r->kind) {
	case RK_VAR:
		return t->kind == K_VAR && !is_member(t) && t->scope == r->scope && tok_eq(t, r->name, 0);
	case RK_MEMBER:
		return is_member(t) && tok_eq(t, r->name, 0);
	case RK_CLASS:
		return t->kind == K_ID && !is_member(t) && t->after != A_FUNCTION && tok_eq(t, r->name, 1);
	case RK_NAME:
		return t->kind == K_ID && !is_member(t) && tok_eq(t, r->name, 0);
	}
	return 0;
}

static int is_decl(const RefTarget *r, const Tok *t) {
	switch (r->kind) {
	case RK_MEMBER: return member_decl(t);
	case RK_CLASS: return t->after == A_CLASSKW;
	case RK_NAME: return t->after == A_FUNCTION || t->after == A_CONST;
	}
	return 0;
}

/* a word in a string or comment that probably means the target */
static int text_match(const RefTarget *r, const Tok *t) {
	if (t->kind != K_TEXT) return 0;
	if (r->kind == RK_VAR) return t->dollar && tok_eq(t, r->name, 0);
	return tok_eq(t, r->name, r->kind == RK_CLASS);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

typedef struct { int file, line, col; int found; Tok tok; char name[32]; } AtCtx;

static void at_fn(const Tok *t, void *vc) {
	AtCtx *c = vc;
	if (c->found || t->file != c->file || t->line != c->line) return;
	int start = t->col - (t->kind == K_VAR ? 1 : 0);
	if (c->col < start || c->col > t->col + t->len) return;
	/* the cursor right behind a word: only if nothing starts there */
	c->tok = *t;
	snprintf(c->name, sizeof c->name, "%.*s", t->len, t->name);
	c->tok.name = c->name;
	c->found = c->col < t->col + t->len ? 2 : 1;
}

typedef struct { const RefTarget *r; int found, file, line, col; } DefCtx;

static void def_fn(const Tok *t, void *vc) {
	DefCtx *c = vc;
	if (c->found || !matches(c->r, t)) return;
	if (c->r->kind != RK_VAR && !is_decl(c->r, t)) return;
	c->found = 1;
	c->file = t->file;
	c->line = t->line;
	c->col = t->col;
}

int ref_definition(const RefTarget *t, int *file, int *line, int *col) {
	if (t->kind == RK_NONE) return 0;
	DefCtx c = { t, 0, 0, 0, 0 };
	scan_all(def_fn, &c);
	if (!c.found) return 0;
	*file = c.file;
	*line = c.line;
	*col = c.col;
	return 1;
}

int ref_target(int file, int line, int col, RefTarget *r) {
	memset(r, 0, sizeof *r);
	AtCtx c;
	memset(&c, 0, sizeof c);
	c.file = file;
	c.line = line;
	c.col = col;
	scan_file(file, at_fn, &c);
	if (!c.found || c.tok.kind == K_TEXT || c.tok.len >= (int)sizeof r->name) return RK_NONE;
	const Tok *t = &c.tok;
	snprintf(r->name, sizeof r->name, "%s", c.name);
	if (t->kind == K_VAR) {
		if (!strcmp(r->name, "this")) return r->kind = RK_NONE;
		if (is_member(t)) r->kind = RK_MEMBER;
		else { r->kind = RK_VAR; r->scope = t->scope; return r->kind; }
	} else if (is_member(t)) r->kind = RK_MEMBER;
	else {
		RefTarget k = { RK_CLASS, "", 0 };
		strcpy(k.name, r->name);
		int f, l, cc;
		r->kind = ref_definition(&k, &f, &l, &cc) ? RK_CLASS : RK_NAME;
	}
	/* only names the program declares: not strlen(), Exception, getMessage() */
	int f, l, cc;
	if (!ref_definition(r, &f, &l, &cc)) r->kind = RK_NONE;
	return r->kind;
}

typedef struct { const RefTarget *r; RefOcc *out; int n, max; } FindCtx;

static void find_fn(const Tok *t, void *vc) {
	FindCtx *c = vc;
	int code = matches(c->r, t);
	if (!code && !text_match(c->r, t)) return;
	if (c->n >= c->max) return;
	RefOcc *o = &c->out[c->n++];
	o->file = (short)t->file;
	o->line = (short)t->line;
	o->col = (short)t->col;
	o->len = (short)t->len;
	o->code = (unsigned char)code;
	o->decl = (unsigned char)(code && is_decl(c->r, t));
}

int ref_find(const RefTarget *t, RefOcc *out, int max) {
	if (t->kind == RK_NONE) return 0;
	FindCtx c = { t, out, 0, max };
	scan_all(find_fn, &c);
	return c.n;                              /* the scan already goes in order */
}

int ref_rename(const RefTarget *t, const char *newName, char *err, int errSize) {
	static RefOcc occ[REF_MAX];
	int nn = (int)strlen(newName);
	if (!nn || nn > 31 || !is_id_start(newName[0])) { snprintf(err, errSize, "invalid name"); return -1; }
	for (int i = 0; i < nn; i++) if (!is_id(newName[i])) { snprintf(err, errSize, "invalid name"); return -1; }
	if (is_keyword(newName, nn) && t->kind != RK_VAR) { snprintf(err, errSize, "%s is a keyword", newName); return -1; }
	int n = ref_find(t, occ, REF_MAX);
	if (n >= REF_MAX) { snprintf(err, errSize, "too many places"); return -1; }
	/* lines must not get too long: check everything first */
	for (int i = 0; i < n; ) {
		int f = occ[i].file, ln = occ[i].line, grow = 0, j = i;
		for (; j < n && occ[j].file == f && occ[j].line == ln; j++) if (occ[j].code) grow += nn - occ[j].len;
		if ((int)strlen(basic_file_line_text(f, ln)) + grow > BASIC_LINE_MAX) {
			snprintf(err, errSize, "line %d too long", ln + 1);
			return -1;
		}
		i = j;
	}
	int cur = basic_current_file(), changed = 0;
	for (int i = 0; i < n; ) {
		int f = occ[i].file, ln = occ[i].line, j = i;
		while (j < n && occ[j].file == f && occ[j].line == ln) j++;
		char buf[BASIC_LINE_MAX + 1];
		const char *s = basic_file_line_text(f, ln);
		int o = 0, p = 0;
		for (int k = i; k < j; k++) {
			if (!occ[k].code) continue;
			memcpy(buf + o, s + p, occ[k].col - p);
			o += occ[k].col - p;
			memcpy(buf + o, newName, nn);
			o += nn;
			p = occ[k].col + occ[k].len;
			changed++;
		}
		strcpy(buf + o, s + p);
		if (basic_current_file() != f) basic_select_file(f);
		basic_set_line(ln, buf);
		i = j;
	}
	if (basic_current_file() != cur) basic_select_file(cur);
	return changed;
}

int ref_word(const char *s, int col, int *start, int *end) {
	int l = (int)strlen(s);
	if (col > l) col = l;
	int b = col;
	if (b < l && is_id(s[b])) { while (b > 0 && is_id(s[b - 1])) b--; }
	else if (b < l && s[b] == '$' && b + 1 < l && is_id_start(s[b + 1])) b++;
	else if (b > 0 && is_id(s[b - 1])) { while (b > 0 && is_id(s[b - 1])) b--; }
	else return 0;
	int e = b;
	while (e < l && is_id(s[e])) e++;
	if (b > 0 && s[b - 1] == '$') b--;
	if (e <= b) return 0;
	*start = b;
	*end = e;
	return 1;
}
