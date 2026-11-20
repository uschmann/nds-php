/* dbg.h - debugger panel on the bottom screen: code with breakpoints
 * (tap a line), buttons to run, step and stop */
#ifndef DBG_H
#define DBG_H

#include <nds.h>
#include <stdbool.h>

enum { DBA_NONE, DBA_CLOSE, DBA_RUN, DBA_STEP, DBA_OVER, DBA_OUT, DBA_STOP };

void dbgp_open(int file);          /* show the panel with this file          */
void dbgp_update(void);            /* once per frame: redraws what changed   */
/* once per frame while open; returns DBA_* (the caller runs the action) */
int  dbgp_poll(bool touching, int px, int py, u32 down, u32 rep);

#endif
