/* refactor.h - rename, go to definition and find usages across the files
 * of the program
 *
 * Plain C without DS dependencies (the host tests use it too). A small
 * tokenizer walks the files and knows strings, comments, class bodies and
 * function scopes; what a name means is taken from where it stands:
 *
 *   $x            variable: the named function it is in, or the global
 *                 code of all files (closures belong to their surroundings)
 *   ->x  ::x      member: methods, properties, constants and enum cases of
 *                 all classes with that name (no type information)
 *   Name          class / interface / trait / enum declared in the program
 *   name          function or constant declared in the program
 *
 * Words in strings and comments are reported too (code = 0), but are not
 * changed by a rename.
 */
#ifndef REFACTOR_H
#define REFACTOR_H

#define REF_MAX 256

enum { RK_NONE, RK_VAR, RK_MEMBER, RK_CLASS, RK_NAME };

typedef struct {
	int  kind;                   /* RK_*                                   */
	char name[32];               /* without the "$"                        */
	int  scope;                  /* RK_VAR: 0 global, else function        */
} RefTarget;

typedef struct {
	short file, line, col, len;  /* position of the name (without "$")      */
	unsigned char code;          /* 1 code, 0 inside a string or comment    */
	unsigned char decl;          /* the declaration                          */
} RefOcc;

/* name under file:line:col (the cursor may also stand right behind it);
 * returns its kind, RK_NONE for keywords, strings, comments and names that
 * are not declared in the program (built-in functions and classes) */
int  ref_target(int file, int line, int col, RefTarget *t);
/* all places, sorted by file, line, column; returns the count */
int  ref_find(const RefTarget *t, RefOcc *out, int max);
/* where it is declared (variables: first use); 0 if not found */
int  ref_definition(const RefTarget *t, int *file, int *line, int *col);
/* rename every code place; returns the number of changes or -1 with err */
int  ref_rename(const RefTarget *t, const char *newName, char *err, int errSize);
/* word around col in a line ("$" belongs to it): start / end (exclusive);
 * 0 if there is none */
int  ref_word(const char *text, int col, int *start, int *end);

#endif
