/* vers.h - version list of the current program (bottom screen) */
#ifndef VERS_H
#define VERS_H

#include <nds.h>
#include <stdbool.h>

enum { VR_NONE, VR_EXIT, VR_DIFF, VR_CHECKOUT, VR_COMMIT };

void vers_open(void);
void vers_refresh(void);                 /* re-read the list (after COMMIT / CHECKOUT) */
void vers_set_message(const char *m);
/* CHECKOUT refused because of uncommitted changes: ask, a 2nd tap forces */
void vers_ask_force(void);
/* call once per frame while open; returns VR_* */
int  vers_poll(bool touching, int px, int py, u32 down, u32 rep);

int  vers_target(void);                  /* VR_DIFF / VR_CHECKOUT: version number */
bool vers_force(void);                   /* VR_CHECKOUT: discard changes          */

/* top screen: what changed from the selected version to the program */
const char *vers_preview_title(void);
int         vers_preview_count(void);
const char *vers_preview_line(int i);

#endif
