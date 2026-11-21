/* complete.c - autocompletion for the editor and the REPL (see complete.h)
 *
 * The program's own names (functions, classes, methods, properties,
 * constants, enum cases, variables) come from a quick scan of all its
 * files, done for every lookup; even long programs scan in a few ms.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "complete.h"
#include "basic.h"

/* ------------------------------------------------------------------ */
/* the program's names                                                 */
/* ------------------------------------------------------------------ */

enum { S_FUNC, S_CLASS, S_VAR, S_METHOD, S_PROP, S_CCONST, S_CASE };

#define SYMS_MAX    320
#define CLASSES_MAX 48

typedef struct { char name[32]; char extra[32]; signed char cls; unsigned char kind; } Sym;
static Sym  syms[SYMS_MAX];
static int  nsyms;
static char clsName[CLASSES_MAX][32];
static char clsEnum[CLASSES_MAX];
static int  nclasses;

static void add_sym(const char *name, int kind, int cls, const char *extra) {
	for (int i = 0; i < nsyms; i++)
		if (syms[i].kind == kind && syms[i].cls == cls && !strcmp(syms[i].name, name)) return;
	if (nsyms >= SYMS_MAX) return;
	Sym *s = &syms[nsyms++];
	snprintf(s->name, sizeof s->name, "%s", name);
	snprintf(s->extra, sizeof s->extra, "%s", extra ? extra : "");
	s->cls = (signed char)cls;
	s->kind = (unsigned char)kind;
}

static int ident_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void scan_program(void) {
	nsyms = nclasses = 0;
	int depth = 0, curClass = -1, classDepth = -1, pendingClass = -1, inComment = 0, vis = 0;
	char last[16] = "";
	for (int f = 0; f < basic_file_count(); f++) {
		for (int ln = 0; ln < basic_file_line_count(f); ln++) {
			const char *t = basic_file_line_text(f, ln);
			int l = (int)strlen(t);
			for (int i = 0; i < l;) {
				char c = t[i];
				if (inComment) {
					if (c == '*' && t[i + 1] == '/') { inComment = 0; i += 2; } else i++;
					continue;
				}
				if (c == '/' && t[i + 1] == '*') { inComment = 1; i += 2; continue; }
				if (c == '#' || (c == '/' && t[i + 1] == '/')) break;
				if (c == '"' || c == '\'') {
					for (i++; i < l && t[i] != c; i++) if (t[i] == '\\') i++;
					i++;
					last[0] = 0;
					continue;
				}
				if (c == '$' && i + 1 < l && (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_')) {
					char name[32];
					int n = 0;
					for (i++; i < l && ident_ch(t[i]); i++) if (n < 31) name[n++] = t[i];
					name[n] = 0;
					add_sym(name, S_VAR, -1, NULL);
					if (curClass >= 0 && vis) add_sym(name, S_PROP, curClass, NULL);
					vis = 0;
					last[0] = 0;
					continue;
				}
				if (isalpha((unsigned char)c) || c == '_') {
					char w[32];
					int n = 0;
					for (; i < l && ident_ch(t[i]); i++) if (n < 31) w[n++] = t[i];
					w[n] = 0;
					if (!strcasecmp(last, "function")) {
						/* parameters on the same line as a hint */
						char params[32] = "";
						int j = i;
						while (j < l && t[j] == ' ') j++;
						if (j < l && t[j] == '(') {
							int k = j, d = 0, m = 0;
							for (; k < l; k++) {
								if (t[k] == '(') d++;
								if (m < 31) params[m++] = t[k];
								if (t[k] == ')' && --d == 0) break;
							}
							params[m] = 0;
						}
						if (curClass >= 0 && depth == classDepth) add_sym(w, S_METHOD, curClass, params);
						else add_sym(w, S_FUNC, -1, params);
					} else if (!strcasecmp(last, "class") || !strcasecmp(last, "interface") ||
					           !strcasecmp(last, "trait") || !strcasecmp(last, "enum")) {
						if (nclasses < CLASSES_MAX) {
							snprintf(clsName[nclasses], 32, "%s", w);
							clsEnum[nclasses] = !strcasecmp(last, "enum");
							pendingClass = nclasses++;
							add_sym(w, S_CLASS, -1, NULL);
						}
					} else if (curClass >= 0 && depth == classDepth && !strcasecmp(last, "const")) {
						add_sym(w, S_CCONST, curClass, NULL);
					} else if (curClass >= 0 && depth == classDepth && !strcasecmp(last, "case")) {
						add_sym(w, S_CASE, curClass, NULL);
					}
					if (!strcasecmp(w, "public") || !strcasecmp(w, "protected") || !strcasecmp(w, "private") ||
					    !strcasecmp(w, "var") || !strcasecmp(w, "readonly")) vis = 1;
					if (!strcasecmp(w, "function")) vis = 0;
					snprintf(last, sizeof last, "%.15s", w);
					continue;
				}
				if (c == '{') {
					depth++;
					if (pendingClass >= 0) { curClass = pendingClass; classDepth = depth; pendingClass = -1; }
				} else if (c == '}') {
					if (curClass >= 0 && depth == classDepth) curClass = -1;
					if (depth > 0) depth--;
				}
				if (c == ';' || c == '{' || c == ',' || c == '=') vis = 0;
				if (c != ' ' && c != '(' && c != '?' && c != '|' && c != ':') last[0] = 0;
				i++;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* fixed lists                                                         */
/* ------------------------------------------------------------------ */

static const char *const keywords[] = {
	"echo", "print", "if", "else", "elseif", "while", "do", "for", "foreach", "as", "switch",
	"case", "default", "break", "continue", "function", "return", "global", "true", "false",
	"null", "and", "or", "xor", "isset", "unset", "empty", "exit", "die", "array",
	"require", "require_once", "include", "include_once", "class", "interface", "trait",
	"extends", "implements", "new", "clone", "instanceof", "public", "protected", "private",
	"static", "abstract", "final", "const", "use", "self", "parent", "readonly",
	"try", "catch", "finally", "throw", "match", "enum", "fn", NULL
};

static const char *const builtinClasses[] = {
	"Exception", "Error", "ErrorException", "TypeError", "ValueError", "ArithmeticError",
	"DivisionByZeroError", "ArgumentCountError", "JsonException", "LogicException",
	"BadFunctionCallException", "BadMethodCallException", "DomainException",
	"InvalidArgumentException", "LengthException", "OutOfRangeException", "RuntimeException",
	"OutOfBoundsException", "OverflowException", "RangeException", "UnderflowException",
	"UnexpectedValueException", "UnhandledMatchError", "Throwable", NULL
};

/* snippets: label, abbreviation, text, cursor position in the text */
typedef struct { const char *label, *abbr, *text; int cursor; } Snippet;
static const Snippet snippets[] = {
	{ "foreach()", "fe", "foreach ($ as $v) {", 10 },
	{ "for()", "fo", "for ($i = 0; $i < ; $i++) {", 18 },
	{ "while()", "wh", "while () {", 7 },
	{ "if()", "if", "if () {", 4 },
	{ "switch()", "sw", "switch () {", 8 },
	{ "match()", "ma", "match () {", 7 },
	{ "function()", "fn", "function () {", 10 },
	{ "try/catch", "tc", "try {  } catch (Exception $e) {  }", 6 },
	{ "class{}", "cl", "class  {", 6 },
	{ "__construct()", "co", "public function __construct(public $) {}", 36 },
	{ "public function", "pf", "public function () {", 16 },
	{ NULL, NULL, NULL, 0 }
};

/* ------------------------------------------------------------------ */
/* candidates                                                          */
/* ------------------------------------------------------------------ */

static const char *const *helpLines;
static int helpCount;
void complete_set_help(const char *const *lines, int count) { helpLines = lines; helpCount = count; }

#define RECENT_MAX 12
static char recent[RECENT_MAX][24];

void complete_used(const char *label) {
	int i;
	for (i = 0; i < RECENT_MAX - 1 && strcmp(recent[i], label); i++) {}
	memmove(recent[1], recent[0], i * sizeof recent[0]);
	snprintf(recent[0], sizeof recent[0], "%s", label);
}

static int recent_rank(const char *label) {
	for (int i = 0; i < RECENT_MAX && recent[i][0]; i++) if (!strcmp(recent[i], label)) return i;
	return -1;
}

/* signature from the help page: a line whose code part is "name(...)" */
static int help_sig(const char *name, char *out, int size) {
	int n = (int)strlen(name);
	for (int i = 0; i < helpCount; i++) {
		const char *h = helpLines[i];
		if (strncasecmp(h, name, n) || h[n] != '(') continue;
		const char *bt = strchr(h, '`');
		int l = bt ? (int)(bt - h) : (int)strlen(h);
		if (l >= size) l = size - 1;
		memcpy(out, h, l);
		out[l] = 0;
		return 1;
	}
	return 0;
}

#define POOL_MAX 64
static Completion pool[POOL_MAX];      /* all candidates, sorted, then the best are copied */
static Completion *res = pool;
static int nres, maxres = POOL_MAX;
static const char *word;
static int wlen;

static int matches(const char *name) { return !strncasecmp(name, word, wlen); }

static void add(const char *label, const char *insert, int back, const char *hint, int kind) {
	for (int i = 0; i < nres; i++) if (!strcmp(res[i].label, label)) return;
	if (nres >= maxres) return;
	Completion *c = &res[nres++];
	snprintf(c->label, sizeof c->label, "%s", label);
	snprintf(c->insert, sizeof c->insert, "%s", insert);
	c->back = back;
	snprintf(c->hint, sizeof c->hint, "%s", hint ? hint : "");
	c->kind = (unsigned char)kind;
}

static void add_func(const char *name, const char *hint, int kind) {
	char ins[48];
	snprintf(ins, sizeof ins, "%.40s(", name);
	add(name, ins, 0, hint, kind);
}

static int rank(const Completion *c) {
	int r = recent_rank(c->label);
	return r >= 0 ? r : RECENT_MAX + c->kind;
}

static int cmp_comp(const void *a, const void *b) {
	const Completion *x = a, *y = b;
	int d = rank(x) - rank(y);
	return d ? d : strcasecmp(x->label, y->label);
}

static void add_members(int cls, int arrow) {
	char hint[40];
	for (int i = 0; i < nsyms; i++) {
		Sym *s = &syms[i];
		if (cls >= 0 && s->cls != cls) continue;
		if (!matches(s->name)) continue;
		if (s->kind == S_METHOD) {
			snprintf(hint, sizeof hint, "%.20s%.19s", s->name, s->extra);
			add_func(s->name, hint, CK_OWN);
		} else if (arrow && s->kind == S_PROP) add(s->name, s->name, 0, NULL, CK_OWN);
		else if (!arrow && (s->kind == S_CCONST || s->kind == S_CASE))
			add(s->name, s->name, 0, s->kind == S_CASE ? "enum case" : "const", CK_OWN);
	}
}

/* is the cursor inside a string or a comment? */
static int in_string(const char *t, int l) {
	char q = 0;
	for (int i = 0; i < l; i++) {
		if (q) { if (t[i] == '\\') i++; else if (t[i] == q) q = 0; continue; }
		if (t[i] == '"' || t[i] == '\'') q = t[i];
		else if (t[i] == '#' || (t[i] == '/' && t[i + 1] == '/')) return 1;
	}
	return q != 0;
}

int complete_find(const char *before, Completion *out, int max, int *wordLen) {
	int l = (int)strlen(before), s = l;
	while (s > 0 && ident_ch(before[s - 1])) s--;
	word = before + s;
	wlen = l - s;
	*wordLen = wlen;
	nres = 0;
	if (in_string(before, l)) return 0;
	if (wlen > 0 && isdigit((unsigned char)word[0])) return 0;
	scan_program();
	char hint[40];

	/* $variable */
	if (s > 0 && before[s - 1] == '$') {
		for (int i = 0; i < nsyms; i++)
			if (syms[i].kind == S_VAR && matches(syms[i].name) && strcmp(syms[i].name, word))
				add(syms[i].name, syms[i].name, 0, NULL, CK_OWN);
		goto done;
	}
	/* ->member */
	if (s >= 2 && before[s - 1] == '>' && before[s - 2] == '-') { add_members(-1, 1); goto done; }
	/* Class::member */
	if (s >= 2 && before[s - 1] == ':' && before[s - 2] == ':') {
		int e = s - 2, b = e;
		while (b > 0 && ident_ch(before[b - 1])) b--;
		char cn[32];
		snprintf(cn, sizeof cn, "%.*s", e - b, before + b);
		int cls = -1;
		for (int i = 0; i < nclasses; i++) if (!strcasecmp(clsName[i], cn)) cls = i;
		add_members(cls, 0);
		if (cls < 0 || clsEnum[cls]) {
			if (matches("cases")) add_func("cases", "cases(): alle F\x84lle / all cases", CK_OWN);
			if (matches("from")) add_func("from", "from($wert)", CK_OWN);
			if (matches("tryFrom")) add_func("tryFrom", "tryFrom($wert)", CK_OWN);
		}
		if (matches("class")) add("class", "class", 0, "Name der Klasse / class name", CK_KEYWORD);
		goto done;
	}
	/* class names after new, instanceof, extends, implements, catch ( */
	{
		int p = s;
		while (p > 0 && before[p - 1] == ' ') p--;
		int catchParen = p > 0 && before[p - 1] == '(';
		if (catchParen) { p--; while (p > 0 && before[p - 1] == ' ') p--; }
		int e = p;
		while (p > 0 && ident_ch(before[p - 1])) p--;
		char prev[16];
		snprintf(prev, sizeof prev, "%.*s", e - p < 15 ? e - p : 15, before + p);
		int wantClass = catchParen ? !strcasecmp(prev, "catch") :
		                (!strcasecmp(prev, "new") || !strcasecmp(prev, "instanceof") ||
		                 !strcasecmp(prev, "extends") || !strcasecmp(prev, "implements"));
		if (wantClass) {
			for (int i = 0; i < nsyms; i++)
				if (syms[i].kind == S_CLASS && matches(syms[i].name)) add(syms[i].name, syms[i].name, 0, NULL, CK_OWN);
			for (int i = 0; builtinClasses[i]; i++)
				if (matches(builtinClasses[i])) add(builtinClasses[i], builtinClasses[i], 0, NULL, CK_BUILTIN);
			goto done;
		}
	}
	/* a word: functions, keywords, constants, classes, snippets */
	if (wlen < 2) return 0;
	for (int i = 0; i < nsyms; i++) {
		Sym *sy = &syms[i];
		if (!matches(sy->name)) continue;
		if (sy->kind == S_FUNC) {
			snprintf(hint, sizeof hint, "%.20s%.19s", sy->name, sy->extra);
			add_func(sy->name, hint, CK_OWN);
		} else if (sy->kind == S_CLASS && strcmp(sy->name, word)) add(sy->name, sy->name, 0, NULL, CK_OWN);
	}
	for (int i = 0; snippets[i].label; i++) {
		const Snippet *sn = &snippets[i];
		if (!strcasecmp(word, sn->abbr) || (wlen >= 2 && !strncasecmp(sn->label, word, wlen)))
			add(sn->label, sn->text, (int)strlen(sn->text) - sn->cursor, sn->text, CK_SNIPPET);
	}
	for (int i = 0; keywords[i]; i++)
		if (matches(keywords[i]) && strcmp(keywords[i], word)) add(keywords[i], keywords[i], 0, NULL, CK_KEYWORD);
	for (int i = 0; i < basic_builtin_count(); i++) {
		const char *n = basic_builtin_name(i);
		if (n[0] == '_' || !matches(n)) continue;
		if (!help_sig(n, hint, sizeof hint)) {
			int mn, mx;
			basic_builtin_args(i, &mn, &mx);
			if (mx < 0) snprintf(hint, sizeof hint, "%s(%d+)", n, mn);
			else if (mn == mx) snprintf(hint, sizeof hint, "%s(%d)", n, mn);
			else snprintf(hint, sizeof hint, "%s(%d-%d)", n, mn, mx);
		}
		add_func(n, hint, CK_BUILTIN);
	}
	for (int i = 0; i < basic_constant_count(); i++) {
		const char *n = basic_constant_name(i);
		if (matches(n) && strcmp(n, word)) add(n, n, 0, NULL, CK_CONST);
	}
done:
	qsort(pool, nres, sizeof *pool, cmp_comp);
	if (nres > max) nres = max;
	memcpy(out, pool, nres * sizeof *out);
	return nres;
}
