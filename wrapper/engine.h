/* The engine interface vst2_wrap.c drives: any synth/effect core that provides mpc_engine().
 * Contract: 44100 Hz, interleaved int16 stereo, rendered in 128-frame blocks. Parameters are
 * string key/value pairs; the keys and their ranges come from the port's generated params.h.
 * An engine written for another host plugs in through a small adapter (see adapters/). */
#pragma once
#include <stdint.h>

typedef struct {
    void *(*create)(const char *data_dir);    /* data_dir: MODULE_DIR define, or NULL */
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);   /* > 0 on success */
    void (*render)(void *inst, int16_t *out_lr, int frames);
    /* Effects only: transform an input block into an output block (same 44100 Hz, 128-frame,
     * interleaved int16 stereo contract as render). NULL for instruments -- the wrapper then uses
     * render(). A port declares itself an effect with "effect": true in vst.json (the wrapper then
     * advertises numInputs=2 and drops effFlagsIsSynth). in_lr may alias nothing; do not write it. */
    void (*process)(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames);
} mpc_engine_t;

const mpc_engine_t *mpc_engine(void);
