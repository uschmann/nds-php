/* complete.h - autocompletion for the editor and the REPL
 *
 * Plain C without DS dependencies (the host tests use it too). The
 * candidates depend on the text before the cursor: functions, keywords
 * and constants for a word, variables after "$", methods and properties
 * after "->", class members after "Name::" and class names after "new".
 */
#ifndef COMPLETE_H
#define COMPLETE_H

#define COMPLETE_MAX 24

enum { CK_RECENT, CK_OWN, CK_SNIPPET, CK_KEYWORD, CK_BUILTIN, CK_CONST };

typedef struct {
	char label[24];          /* shown in the suggestion bar             */
	char insert[48];         /* replaces the word typed so far          */
	int  back;               /* then move the cursor this far left      */
	char hint[36];           /* signature or explanation for the bar    */
	unsigned char kind;      /* CK_*                                    */
} Completion;

/* help lines ("CODE`text") used for function signatures */
void complete_set_help(const char *const *lines, int count);
/* candidates for `before` (the text left of the cursor); *wordLen is the
 * number of characters at its end that `insert` replaces */
int  complete_find(const char *before, Completion *out, int max, int *wordLen);
/* remember an accepted suggestion: recently used ones come first */
void complete_used(const char *label);

#endif
