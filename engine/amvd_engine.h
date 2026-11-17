/*
 * amvd_engine.h - Answering Machine / Voicemail Detection engine
 *
 * Portable C99, no dependencies. Used by mod_amvd (FreeSWITCH), amvd_cli
 * (offline corpus runs / feature extraction) and anything else that can
 * hand it 16-bit mono PCM.
 *
 * Pipeline per 10 ms frame:
 *   adaptive-noise VAD -> words -> segments (utterances)
 *   ZCR-guided Goertzel tone detector -> beep / fax CED / SIT
 *   fast-path rules (short greeting + silence => human, long speech => machine)
 *   window-end classifier (built-in rules, or trained tree ensemble)
 *   optional message-end tracking (beep / end-of-greeting silence) for VM drop
 */
#ifndef AMVD_ENGINE_H
#define AMVD_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMVD_VERSION "1.0.0"

typedef enum {
	AMVD_RES_NONE = 0,
	AMVD_RES_HUMAN,
	AMVD_RES_MACHINE,
	AMVD_RES_FAX,
	AMVD_RES_UNAVAILABLE, /* SIT tone: number not in service / unreachable */
	AMVD_RES_UNKNOWN      /* silence / timeout with no usable evidence */
} amvd_result_t;

typedef enum {
	AMVD_END_NONE = 0,
	AMVD_END_BEEP,     /* tone heard after the greeting: recording has started */
	AMVD_END_SILENCE,  /* greeting ended, no beep, silence held */
	AMVD_END_OTHER     /* message timeout hit mid-greeting */
} amvd_end_t;

/* Event bits returned by amvd_engine_feed() */
#define AMVD_EV_RESULT   0x01  /* classification is ready */
#define AMVD_EV_BEEP     0x02  /* a beep-like tone completed (informational) */
#define AMVD_EV_MSG_END  0x04  /* message end detected (only if wait_message_end) */
#define AMVD_EV_DONE     0x08  /* engine needs no more audio */

typedef enum { AMVD_DEFAULT_HUMAN = 0, AMVD_DEFAULT_MACHINE, AMVD_DEFAULT_UNKNOWN } amvd_default_t;

typedef struct {
	/* --- VAD --- */
	double vad_floor_dbfs;       /* absolute minimum level for speech (default -45) */
	double vad_margin_db;        /* speech must exceed noise floor by this (default 9) */
	int    min_word_ms;          /* voiced run needed to open a word (default 100) */
	int    word_gap_ms;          /* silence that closes a word (default 80) */
	int    segment_gap_ms;       /* silence that closes an utterance (default 300) */

	/* --- Timing rules (all ms) --- */
	int    initial_silence_ms;   /* no speech by then -> default/unknown (default 3500) */
	int    human_max_greeting_ms;/* fast human path only if first utterance <= this (1800) */
	int    human_silence_base_ms;/* silence required after a very short "hello" (600) */
	double human_silence_slope;  /* extra silence per ms of greeting (0.4 => 1.5s greet -> +600) */
	int    human_silence_max_ms; /* cap on required silence (1400) */
	int    business_silence_ms;  /* silence after a 1.8-2.8 s greeting before human (1500) */
	int    machine_speech_ms;    /* voiced time in greeting that forces machine (2800) */
	int    machine_max_words;    /* words in greeting before silence that forces machine (6) */
	int    window_ms;            /* feature window / classifier checkpoint (3000) */
	int    total_analysis_ms;    /* hard stop for classification (5000) */
	amvd_default_t default_verdict; /* when evidence runs out (human) */
	double machine_threshold;    /* P(machine) needed to say machine (0.85) */
	double human_threshold;      /* P(machine) below which model says human (0.35) */

	/* --- Tone detection --- */
	int    tones_enabled;        /* 1 */
	double tone_purity;          /* min tonal purity 0..1 (0.80) */
	int    beep_min_hz, beep_max_hz; /* 400..2000 */
	int    beep_min_ms, beep_max_ms; /* 70..2000 */
	double beep_freq_spread;     /* max (fmax-fmin)/f inside one beep (0.015) */
	int    beep_pre_quiet_ms;    /* no speech for this long before the tone (100) */
	int    fax_enabled;          /* detect 2100 Hz CED (1) */
	int    sit_enabled;          /* detect SIT triplets (1) */
	int    beep_is_machine;      /* beep during analysis => machine (1) */

	/* --- Model / corpus --- */
	int    human_defer_to_model; /* with a model: hold fast-human verdicts until window (0) */
	int    features_only;        /* extraction mode: no rules, verdict at window (0) */

	/* --- Message end (voicemail drop) --- */
	int    wait_message_end;     /* keep listening after machine verdict (0) */
	int    end_silence_ms;       /* silence that ends a greeting without beep (1800) */
	int    beep_post_ms;         /* wait after beep tone stops before firing (120) */
	int    message_timeout_ms;   /* give up waiting for end, from analysis start (45000) */
} amvd_config_t;

#define AMVD_NUM_FEATURES 16
extern const char *amvd_feature_names[AMVD_NUM_FEATURES];

typedef struct amvd_model amvd_model_t;
typedef struct amvd_engine amvd_engine_t;

typedef struct {
	amvd_result_t result;
	const char   *reason;        /* short machine-readable reason */
	double        p_machine;     /* score at decision (-1 if rule-only and not computed) */
	int           decision_ms;   /* ms of audio consumed when decided */
	int           first_speech_ms; /* -1 if none */
	double        features[AMVD_NUM_FEATURES];
	int           features_ms;   /* audio length the feature vector covers */

	amvd_end_t    end_type;
	int           end_ms;        /* ms when message end fired */
	double        beep_hz;       /* frequency of last beep (0 if none) */
	int           beep_count;
} amvd_report_t;

/* Config */
void        amvd_config_defaults(amvd_config_t *c);
/* Apply one key=value. Keys use '-' or '_'. Returns 0 on success, -1 unknown key, -2 bad value. */
int         amvd_config_set(amvd_config_t *c, const char *key, const char *val);
/* Apply "k=v,k=v" or "k=v k=v" strings. Returns count of errors. */
int         amvd_config_apply_string(amvd_config_t *c, const char *s, char *err, size_t errlen);

/* Model (gradient-boosted depth-limited trees, text format written by train_model.py) */
amvd_model_t *amvd_model_load(const char *path, char *err, size_t errlen);
void          amvd_model_free(amvd_model_t *m);
double        amvd_model_predict(const amvd_model_t *m, const double *features);

/* Engine. model may be NULL (built-in rules). The model must outlive the engine. */
amvd_engine_t *amvd_engine_create(const amvd_config_t *cfg, int sample_rate, const amvd_model_t *model);
void           amvd_engine_destroy(amvd_engine_t *e);
/* Feed any number of samples; returns OR of AMVD_EV_* bits raised during this call. */
int            amvd_engine_feed(amvd_engine_t *e, const int16_t *pcm, size_t nsamples);
/* Force a verdict now (e.g. caller hung up / external stop). Returns AMVD_EV_* bits. */
int            amvd_engine_finish(amvd_engine_t *e);
const amvd_report_t *amvd_engine_report(const amvd_engine_t *e);
int            amvd_engine_elapsed_ms(const amvd_engine_t *e);

const char *amvd_result_str(amvd_result_t r);
const char *amvd_end_str(amvd_end_t t);
/* Write features as CSV (no newline) into buf. */
void        amvd_features_csv(const double *f, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
