/* syntax.c - colours for PHP code (editor, REPL, debugger) */
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include "syntax.h"

/* palette indexes as in main.c's basicPalette */
enum { BLACK, WHITE, RED, CYAN, PURPLE, GREEN, BLUE, YELLOW, ORANGE, BROWN,
       LRED, DGREY, GREY, LGREEN, LBLUE, LGREY };
#define TEXTCOL BLACK


static bool is_keyword(const char *w, int n) {
	static const char *const kw[] = {
		"echo", "print", "if", "else", "elseif", "while", "do", "for", "foreach", "as", "switch",
		"case", "default", "break", "continue", "function", "return", "global", "true", "false",
		"null", "and", "or", "xor", "isset", "unset", "empty", "exit", "die", "array",
		"require", "require_once", "include", "include_once", "class", "interface", "trait",
		"extends", "implements", "new", "clone", "instanceof", "public", "protected", "private",
		"static", "abstract", "final", "const", "use", "self", "parent", "readonly",
		"try", "catch", "finally", "throw", "match", "enum", "fn", "declare", NULL
	};
	for (int i = 0; kw[i]; i++)
		if ((int)strlen(kw[i]) == n && !strncasecmp(kw[i], w, n)) return true;
	return false;
}

static bool ident_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool is_type_name(const char *w, int n) {
	static const char *const ty[] = { "int", "float", "string", "bool", "array", "void", "mixed",
	                                  "callable", "iterable", "object", "never", "null", "false", "true",
	                                  "self", "static", NULL };
	for (int i = 0; ty[i]; i++)
		if ((int)strlen(ty[i]) == n && !strncasecmp(ty[i], w, n)) return true;
	return false;
}

/* colour of the word t[i..j) from its neighbours: keyword, Class (also
 * Enum, Exception), CONSTANT / Enum::Case, named argument "name:" */
static unsigned char word_color(const char *t, int l, int i, int j) {
	int n = j - i, k = j;
	while (k < l && t[k] == ' ') k++;
	int p = i - 1;
	while (p >= 0 && t[p] == ' ') p--;
	bool afterColons = i >= 2 && t[i - 1] == ':' && t[i - 2] == ':';
	bool beforeColons = k + 1 < l && t[k] == ':' && t[k + 1] == ':';
	bool call = k < l && t[k] == '(';
	if (afterColons) return call ? TEXTCOL : BLUE;            /* Suit::Hearts, X::CONST */
	/* types: "int $x", "?int", "int|string", "array $a", ": ?self" */
	if (!call && is_type_name(t + i, n) &&
	    ((k < l && (t[k] == '$' || (t[k] == '|' && t[k + 1] != '|'))) ||
	     (i > 0 && t[i - 1] == '?' && (i < 2 || t[i - 2] != '?')) ||
	     (p >= 0 && t[p] == '|' && (p == 0 || t[p - 1] != '|'))))
		return CYAN;
	if (is_keyword(t + i, n)) return RED;
	/* name: value in a call (not "a ? b : c", not "::") */
	if (k < l && t[k] == ':' && !beforeColons && p >= 0 && (t[p] == '(' || t[p] == ',')) return BROWN;
	if (beforeColons) return CYAN;                            /* Class::  */
	/* new Name(...), instanceof Name */
	if (p >= 2) {
		int e = p + 1, b = e;
		while (b > 0 && ident_ch(t[b - 1])) b--;
		if ((e - b == 3 && !strncasecmp(t + b, "new", 3)) || (e - b == 10 && !strncasecmp(t + b, "instanceof", 10))) return CYAN;
	}
	if (!call && p >= 0 && t[p] == ':' && (p == 0 || t[p - 1] != ':') && is_type_name(t + i, n)) return CYAN;
	bool upper = false, lower = false;
	for (int q = i; q < j; q++) {
		if (isupper((unsigned char)t[q])) upper = true;
		else if (islower((unsigned char)t[q])) lower = true;
	}
	if (upper && !lower && n > 1) return BLUE;                /* PHP_EOL, JOY_A */
	if (isupper((unsigned char)t[i]) && lower && !call) return CYAN;   /* Exception, Suit */
	return TEXTCOL;
}

void syntax_highlight(const char *t, int l, unsigned char *col) {
	int i = 0;
	while (i < l) {
		char c = t[i];
		if (c == '#' || (c == '/' && t[i + 1] == '/')) { while (i < l) col[i++] = GREY; break; }
		if (c == '/' && t[i + 1] == '*') {
			int j = i + 2;
			while (j < l && !(t[j] == '*' && t[j + 1] == '/')) j++;
			j = j < l ? j + 2 : l;
			while (i < j) col[i++] = GREY;
			continue;
		}
		if (c == '"' || c == '\'') {
			col[i++] = GREEN;
			while (i < l && t[i] != c) {
				if (t[i] == '\\' && i + 1 < l) col[i++] = GREEN;
				col[i++] = GREEN;
			}
			if (i < l) col[i++] = GREEN;
			continue;
		}
		if (c == '$' && i + 1 < l && (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_')) {
			col[i++] = PURPLE;
			while (i < l && ident_ch(t[i])) col[i++] = PURPLE;
			continue;
		}
		if (c == '\\' && i + 1 < l && (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '_')) {
			col[i++] = CYAN;                       /* \Exception */
			continue;
		}
		if (isalpha((unsigned char)c) || c == '_') {
			int j = i;
			while (j < l && ident_ch(t[j])) j++;
			unsigned char k = word_color(t, l, i, j);
			while (i < j) col[i++] = k;
			continue;
		}
		if (isdigit((unsigned char)c)) {
			while (i < l && (isalnum((unsigned char)t[i]) || t[i] == '.')) col[i++] = ORANGE;
			continue;
		}
		col[i++] = TEXTCOL;
	}
}

