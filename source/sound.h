/* sound.h - simple PSG sequencer for BASIC sound slots */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include "basic.h"

void snd_init(void);
void snd_play(const BasicSound *s);       /* starts playing a copy of s   */
void snd_stop(void);
void snd_frame(void);                     /* call once per video frame    */
bool snd_busy(void);
/* effect voice: queue a sweep from hz0 to hz1 and vol0 to vol1 (0-127)
 * over frames; replace = clear the queue first. Returns queued segments. */
int  snd_tone(int wave, int hz0, int hz1, int frames, int vol0, int vol1, int replace);
int  snd_tone_queued(void);
void snd_fx_stop(void);
int  snd_position(void);                  /* index of current note or -1  */
void snd_preview(int note, int inst, int frames);   /* single note      */
const char *snd_note_name(int note);      /* "C#4", "---" for rest       */
const char *snd_inst_name(int inst);

#endif
