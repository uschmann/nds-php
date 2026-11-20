/* kbd.h - retro on-screen keyboard for the bottom screen (16-bit bitmap) */
#ifndef KBD_H
#define KBD_H

#include <stdbool.h>

/* key codes returned by kbd_poll(); printable ASCII is returned as is */
#define KC_NONE  0
#define KC_DEL   8
#define KC_RET   13
#define KC_LEFT  0x11
#define KC_RIGHT 0x12
#define KC_RUN   0x13
#define KC_SCRN  0x14
#define KC_CLR   0x15
#define KC_UP    0x16
#define KC_DOWN  0x17
#define KC_TAB    0x80   /* + index: file tab tapped        */
#define KC_TABNEW 0x9F   /* "+" tab tapped                  */
#define KC_BROWSER 0x9E  /* "<>" tile tapped: file browser  */
#define KC_DEBUG   0x9D  /* "DBG" tile tapped: debugger     */

void kbd_init(void);
/* call once per frame; extShift = shoulder button held */
int  kbd_poll(bool touching, int px, int py, bool extShift);
void kbd_set_running(bool running);
void kbd_redraw(void);   /* repaint after another screen used the bitmap */
void kbd_set_status(const char *line1, const char *line2);
/* file tabs in the header; names without ".php" are shown */
void kbd_set_tabs(const char *const *names, int count, int current);

#endif
