/* sound.c - simple PSG sequencer for BASIC sound slots
 *
 * One note at a time on a PSG (square wave) or noise channel. Timing is
 * driven by snd_frame() at 60 Hz; a 16th note lasts 900/tempo frames.
 */
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include "sound.h"

#define VOLUME 80

static BasicSound seq;
static bool playing;
static int pos = -1, framesLeft, channel = -1;
static int previewFrames;

/* effect voice for tone()/sfx(): a queue of segments, each a linear sweep
 * of frequency and volume over a number of frames; independent of the
 * sequencer so effects can play over music */
typedef struct { unsigned char wave, vol0, vol1; unsigned short hz0, hz1, frames; } FxSeg;
#define FX_QUEUE 32
static FxSeg fxq[FX_QUEUE];
static int fxHead, fxCount, fxFrame, fxChannel = -1, fxWave = -1;

static void fx_frame(void);

static void fx_off(void) {
	if (fxChannel >= 0) soundKill(fxChannel);
	fxChannel = fxWave = -1;
}

static void note_off(void) {
	if (channel >= 0) soundKill(channel);
	channel = -1;
}

static void note_on(int note, int inst) {
	note_off();
	if (note <= 0) return;
	float hz = 440.0f * powf(2.0f, (note - 69) / 12.0f);
	if (inst == SND_NOISE) {
		channel = soundPlayNoise((u16)(hz * 8), VOLUME, 64);
	} else {
		static const DutyCycle duty[3] = { DutyCycle_12, DutyCycle_25, DutyCycle_50 };
		/* the PSG timer runs 8 steps per period: pass 8x the tone frequency */
		channel = soundPlayPSG(duty[inst < 3 ? inst : 2], (u16)(hz * 8), VOLUME, 64);
	}
}

static int note_frames(int len, int tempo) {
	int f = (len * 900 + tempo / 2) / (tempo ? tempo : 120);
	return f < 2 ? 2 : f;
}

static void start_note(void) {
	note_on(seq.note[pos], seq.inst);
	framesLeft = note_frames(seq.len[pos], seq.tempo);
}

void snd_init(void) { soundEnable(); }

void snd_play(const BasicSound *s) {
	snd_stop();
	if (!s || !s->count) return;
	seq = *s;
	playing = true;
	pos = 0;
	start_note();
}

void snd_stop(void) {
	note_off();
	snd_fx_stop();
	playing = false;
	previewFrames = 0;
	pos = -1;
}

void snd_preview(int note, int inst, int frames) {
	if (playing) snd_stop();
	note_on(note, inst);
	previewFrames = frames;
}

void snd_frame(void) {
	fx_frame();
	if (previewFrames > 0 && --previewFrames == 0) note_off();
	if (!playing) return;
	framesLeft--;
	if (framesLeft == 1) note_off();          /* short gap between notes */
	if (framesLeft > 0) return;
	if (++pos >= seq.count) { snd_stop(); return; }
	start_note();
}

void snd_fx_stop(void) {
	fx_off();
	fxHead = fxCount = fxFrame = 0;
}

int snd_tone(int wave, int hz0, int hz1, int frames, int vol0, int vol1, int replace) {
	if (replace) snd_fx_stop();
	if (fxCount >= FX_QUEUE) return fxCount;
	FxSeg *g = &fxq[(fxHead + fxCount++) % FX_QUEUE];
	g->wave = (unsigned char)wave;
	g->hz0 = (unsigned short)hz0;
	g->hz1 = (unsigned short)hz1;
	g->frames = (unsigned short)(frames < 1 ? 1 : frames);
	g->vol0 = (unsigned char)vol0;
	g->vol1 = (unsigned char)vol1;
	return fxCount;
}

int snd_tone_queued(void) { return fxCount; }

static void fx_frame(void) {
	if (!fxCount) { if (fxChannel >= 0) fx_off(); return; }
	FxSeg *g = &fxq[fxHead];
	int t = fxFrame, n = g->frames;
	int hz = g->hz0 + (g->hz1 - g->hz0) * t / n;
	int vol = g->vol0 + (g->vol1 - g->vol0) * t / n;
	if (hz < 20) hz = 20;
	if (hz > 8000) hz = 8000;
	u16 freq = (u16)(hz * 8);      /* 8 timer steps per period, see note_on */
	if (vol <= 0) fx_off();
	else if (fxChannel < 0 || fxWave != g->wave) {
		fx_off();
		if (g->wave == SND_NOISE) fxChannel = soundPlayNoise(freq, (u8)vol, 64);
		else {
			static const DutyCycle duty[3] = { DutyCycle_12, DutyCycle_25, DutyCycle_50 };
			fxChannel = soundPlayPSG(duty[g->wave < 3 ? g->wave : 2], freq, (u8)vol, 64);
		}
		fxWave = fxChannel >= 0 ? g->wave : -1;
	} else {
		soundSetFreq(fxChannel, freq);
		soundSetVolume(fxChannel, (u8)vol);
	}
	if (++fxFrame >= n) {
		fxFrame = 0;
		fxHead = (fxHead + 1) % FX_QUEUE;
		fxCount--;
	}
}

bool snd_busy(void) { return playing; }
int  snd_position(void) { return playing ? pos : -1; }

const char *snd_note_name(int note) {
	static const char *names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
	static char buf[12];
	if (note <= 0) return "---";
	snprintf(buf, sizeof buf, "%s%d", names[note % 12], note / 12 - 1);
	return buf;
}

const char *snd_inst_name(int inst) {
	static const char *n[SND_INSTRUMENTS] = { "SQ12", "SQ25", "SQ50", "NOISE" };
	return (inst >= 0 && inst < SND_INSTRUMENTS) ? n[inst] : "?";
}
