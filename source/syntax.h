/* syntax.h - colours for PHP code (editor, REPL, debugger) */
#ifndef SYNTAX_H
#define SYNTAX_H

/* colour (palette index 0-15 of basicPalette) for each of the l chars of t */
void syntax_highlight(const char *t, int l, unsigned char *col);

#endif
