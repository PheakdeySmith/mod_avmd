/*
 * amvd_engine.c - Answering Machine / Voicemail Detection engine
 * See amvd_engine.h for the overview.
 */
#include "amvd_engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAME_MS        10
#define MAX_RATE        48000
#define WORK_RATE       8000            /* everything is analysed at telephony band */
#define MAX_FS          (WORK_RATE / 100)
#define DEC_MAX_TAPS    (12 * 6 + 1)    /* 48k -> 8k */
#define MAX_SEGS        128
#define TONE_HIST       4
#define TONE_DROPOUT    2      /* frames a tone may drop out and continue */
#define TONE_FREQ_TOL   0.04   /* relative frequency tolerance inside one tone */
#define NOISE_INIT_DB   -70.0
#define NOISE_WIN       150    /* 1.5 s minimum-statistics window */

const char *amvd_feature_names[AMVD_NUM_FEATURES] = {
	"init_silence_ms",     /* 0  delay before first speech                     */
	"first_seg_ms",        /* 1  first utterance length                        */
	"speech_spread",       /* 2  (last speech end - first speech start)/window */
	"speech_ratio",        /* 3  voiced utterance time / window                */
	"num_segments",        /* 4                                                */
	"num_words",           /* 5                                                */
	"longest_seg_ms",      /* 6                                                */
	"max_gap_ms",          /* 7  longest pause between utterances              */
	"mean_gap_ms",         /* 8                                                */
	"gap_after_first_ms",  /* 9  pause after the first utterance               */
	"speech_last_1s",      /* 10 fraction of the last second that is speech    */
	"trailing_silence_ms", /* 11 silence at window end                         */
	"speech_db_mean",      /* 12                                               */
	"speech_db_std",       /* 13 level consistency (recordings are compressed) */
	"noise_floor_db",      /* 14                                               */
	"tone_count"           /* 15 beep-like tones inside the window             */
};

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

void amvd_config_defaults(amvd_config_t *c)
{
	memset(c, 0, sizeof(*c));
	c->vad_floor_dbfs = -45.0;
	c->vad_margin_db = 9.0;
	c->min_word_ms = 100;
	c->word_gap_ms = 80;
	c->segment_gap_ms = 300;

	c->initial_silence_ms = 3500;
	c->human_max_greeting_ms = 1800;
	c->human_silence_base_ms = 600;
	c->human_silence_slope = 0.5;
	c->human_silence_max_ms = 1500;
	c->business_silence_ms = 1500;
	c->machine_speech_ms = 2800;
	c->machine_max_words = 6;
	c->window_ms = 3000;
	c->total_analysis_ms = 5000;
	c->default_verdict = AMVD_DEFAULT_HUMAN;
	c->machine_threshold = 0.85;
	c->human_threshold = 0.35;

	c->tones_enabled = 1;
	c->tone_purity = 0.80;
	c->beep_min_hz = 400;
	c->beep_max_hz = 2000;
	c->beep_min_ms = 70;
	c->beep_max_ms = 2000;
	c->beep_freq_spread = 0.015;
	c->beep_pre_quiet_ms = 100;
	c->fax_enabled = 1;
	c->sit_enabled = 1;
	c->beep_is_machine = 1;

	c->human_defer_to_model = 0;
	c->features_only = 0;
	c->wait_message_end = 0;
	c->end_silence_ms = 1800;
	c->beep_post_ms = 150;
	c->message_timeout_ms = 45000;
}

typedef enum { T_INT, T_DBL, T_BOOL, T_DEFAULT } ctype_t;
typedef struct { const char *name; ctype_t type; size_t off; } ckey_t;

#define K(n, t) { #n, t, offsetof(amvd_config_t, n) }
static const ckey_t ckeys[] = {
	K(vad_floor_dbfs, T_DBL), K(vad_margin_db, T_DBL), K(min_word_ms, T_INT),
	K(word_gap_ms, T_INT), K(segment_gap_ms, T_INT),
	K(initial_silence_ms, T_INT), K(human_max_greeting_ms, T_INT),
	K(human_silence_base_ms, T_INT), K(human_silence_slope, T_DBL),
	K(human_silence_max_ms, T_INT), K(business_silence_ms, T_INT), K(machine_speech_ms, T_INT),
	K(machine_max_words, T_INT), K(window_ms, T_INT), K(total_analysis_ms, T_INT),
	K(default_verdict, T_DEFAULT), K(machine_threshold, T_DBL), K(human_threshold, T_DBL),
	K(tones_enabled, T_BOOL), K(tone_purity, T_DBL), K(beep_min_hz, T_INT),
	K(beep_max_hz, T_INT), K(beep_min_ms, T_INT), K(beep_max_ms, T_INT), K(beep_freq_spread, T_DBL), K(beep_pre_quiet_ms, T_INT),
	K(fax_enabled, T_BOOL), K(sit_enabled, T_BOOL), K(beep_is_machine, T_BOOL),
	K(human_defer_to_model, T_BOOL), K(features_only, T_BOOL), K(wait_message_end, T_BOOL), K(end_silence_ms, T_INT), K(beep_post_ms, T_INT),
	K(message_timeout_ms, T_INT),
};
#undef K

static int parse_bool(const char *v, int *out)
{
	if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) { *out = 1; return 0; }
	if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) { *out = 0; return 0; }
	return -1;
}

int amvd_config_set(amvd_config_t *c, const char *key, const char *val)
{
	char k[64];
	size_t i;
	char *end;

	if (!key || !val) return -2;
	for (i = 0; key[i] && i < sizeof(k) - 1; i++) k[i] = key[i] == '-' ? '_' : (char)tolower((unsigned char)key[i]);
	k[i] = '\0';

	for (i = 0; i < sizeof(ckeys) / sizeof(ckeys[0]); i++) {
		if (strcmp(ckeys[i].name, k)) continue;
		void *p = (char *)c + ckeys[i].off;
		switch (ckeys[i].type) {
		case T_INT: {
			long v = strtol(val, &end, 10);
			if (end == val || *end) return -2;
			*(int *)p = (int)v;
			return 0;
		}
		case T_DBL: {
			double v = strtod(val, &end);
			if (end == val || *end) return -2;
			*(double *)p = v;
			return 0;
		}
		case T_BOOL: {
			int b;
			if (parse_bool(val, &b)) return -2;
			*(int *)p = b;
			return 0;
		}
		case T_DEFAULT:
			if (!strcasecmp(val, "human")) *(amvd_default_t *)p = AMVD_DEFAULT_HUMAN;
			else if (!strcasecmp(val, "machine")) *(amvd_default_t *)p = AMVD_DEFAULT_MACHINE;
			else if (!strcasecmp(val, "unknown")) *(amvd_default_t *)p = AMVD_DEFAULT_UNKNOWN;
			else return -2;
			return 0;
		}
	}
	return -1;
}

int amvd_config_apply_string(amvd_config_t *c, const char *s, char *err, size_t errlen)
{
	char tok[256];
	int errors = 0;

	if (err && errlen) err[0] = '\0';
	if (!s) return 0;
	while (*s) {
		size_t n = 0;
		char *eq;
		int rc;
		while (*s && strchr(", \t;", *s)) s++;            /* skip separators */
		while (*s && !strchr(", \t;", *s)) {
			if (n < sizeof(tok) - 1) tok[n++] = *s;
			s++;
		}
		tok[n] = '\0';
		if (!n) continue;
		if (!(eq = strchr(tok, '='))) { errors++; continue; }
		*eq = '\0';
		rc = amvd_config_set(c, tok, eq + 1);
		if (rc) {
			errors++;
			if (err && errlen) {
				size_t l = strlen(err);
				snprintf(err + l, errlen - l, "%s%s(%s)", l ? " " : "", tok, rc == -1 ? "unknown" : "bad value");
			}
		}
	}
	return errors;
}

/* ------------------------------------------------------------------ */
/* Model: boosted trees                                                */
/* ------------------------------------------------------------------ */

typedef struct { int feature; double thr; int left, right; double value; } node_t;
typedef struct { int n; node_t *nodes; } tree_t;
struct amvd_model { double base; int ntrees; tree_t *trees; };

void amvd_model_free(amvd_model_t *m)
{
	int i;
	if (!m) return;
	for (i = 0; i < m->ntrees; i++) free(m->trees[i].nodes);
	free(m->trees);
	free(m);
}

amvd_model_t *amvd_model_load(const char *path, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	char word[64];
	int ver = 0, nf = 0, i, j;
	amvd_model_t *m = NULL;

#define FAIL(msg) do { if (err) snprintf(err, errlen, "%s: %s", path, msg); goto fail; } while (0)
	if (!f) { if (err) snprintf(err, errlen, "%s: cannot open", path); return NULL; }
	if (fscanf(f, "%63s %d", word, &ver) != 2 || strcmp(word, "amvd-model") || ver != 1) FAIL("bad header");
	if (fscanf(f, "%63s %d", word, &nf) != 2 || strcmp(word, "features") || nf != AMVD_NUM_FEATURES) FAIL("feature count mismatch");
	for (i = 0; i < nf; i++) {
		if (fscanf(f, "%63s", word) != 1) FAIL("truncated feature list");
		if (strcmp(word, amvd_feature_names[i])) FAIL("feature order mismatch (retrain with this engine version)");
	}
	m = calloc(1, sizeof(*m));
	if (!m) FAIL("oom");
	if (fscanf(f, "%63s %lf", word, &m->base) != 2 || strcmp(word, "base")) FAIL("missing base");
	if (fscanf(f, "%63s %d", word, &m->ntrees) != 2 || strcmp(word, "trees") || m->ntrees <= 0 || m->ntrees > 10000) FAIL("bad tree count");
	m->trees = calloc((size_t)m->ntrees, sizeof(tree_t));
	if (!m->trees) FAIL("oom");
	for (i = 0; i < m->ntrees; i++) {
		tree_t *t = &m->trees[i];
		if (fscanf(f, "%63s %d", word, &t->n) != 2 || strcmp(word, "tree") || t->n <= 0 || t->n > 100000) FAIL("bad tree");
		t->nodes = calloc((size_t)t->n, sizeof(node_t));
		if (!t->nodes) FAIL("oom");
		for (j = 0; j < t->n; j++) {
			node_t *nd = &t->nodes[j];
			if (fscanf(f, "%d %lf %d %d %lf", &nd->feature, &nd->thr, &nd->left, &nd->right, &nd->value) != 5) FAIL("bad node");
			if (nd->feature >= AMVD_NUM_FEATURES) FAIL("node feature out of range");
			if (nd->feature >= 0 && (nd->left <= j || nd->right <= j || nd->left >= t->n || nd->right >= t->n)) FAIL("bad child index");
		}
	}
	fclose(f);
	return m;
fail:
#undef FAIL
	fclose(f);
	amvd_model_free(m);
	return NULL;
}

double amvd_model_predict(const amvd_model_t *m, const double *x)
{
	double raw = m->base;
	int i;
	for (i = 0; i < m->ntrees; i++) {
		const tree_t *t = &m->trees[i];
		int n = 0;
		while (t->nodes[n].feature >= 0)
			/* compare in float32 like the trainer (sklearn casts X to float32) */
			n = ((double)(float)x[t->nodes[n].feature] <= t->nodes[n].thr) ? t->nodes[n].left : t->nodes[n].right;
		raw += t->nodes[n].value;
	}
	return 1.0 / (1.0 + exp(-raw));
}

/* ------------------------------------------------------------------ */
/* Engine state                                                        */
/* ------------------------------------------------------------------ */

typedef struct { int start, end, words; } seg_t;          /* frame indices, inclusive */
typedef struct { double hz; int start, end; } tone_t;

enum { PH_ANALYZE = 0, PH_MSGEND, PH_DONE };

struct amvd_engine {
	amvd_config_t c;
	const amvd_model_t *model;
	int rate, fs;                 /* analysis rate (8000) and samples per 10 ms frame */

	/* decimator: input rate -> 8 kHz (windowed-sinc FIR, polyphase: one dot product per output) */
	int in_rate, dec_m, dec_taps, dec_pos, dec_phase;
	float dec_h[DEC_MAX_TAPS];
	float dec_buf[2 * DEC_MAX_TAPS];

	int16_t pend[MAX_FS];
	int pend_n;
	float win[2 * MAX_FS];        /* last 20 ms, for tone analysis */
	float hann[2 * MAX_FS];
	int f;                        /* index of the frame being processed */

	/* converted thresholds, frames */
	int min_word_f, word_gap_f, seg_gap_f, window_f;

	/* VAD */
	double noise_db;
	float  noise_ring[NOISE_WIN];
	int    noise_ring_n, noise_ring_pos;
	int cand_start, cand_voiced, cand_gap;
	int in_word, word_unvoiced, last_voiced;
	int words_total;

	/* segments */
	seg_t segs[MAX_SEGS];
	int nsegs, in_seg;
	double sp_db_sum, sp_db_sq; int sp_n;  /* inside window only */

	/* tones */
	int tone_active, tone_start, tone_last, tone_n, tone_miss;
	double tone_hz_sum, tone_hz_min, tone_hz_max;
	tone_t tone_hist[TONE_HIST];
	int tone_hist_n;
	int tone_count_window;
	int beep_pending;             /* frame index after which pending beep confirms, or -1 */
	double beep_pending_hz;
	int last_tone_end;
	int fax_fired;

	int phase;
	int window_taken;
	amvd_report_t rep;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int ms2f(int ms) { int f = (ms + FRAME_MS / 2) / FRAME_MS; return f < 1 ? 1 : f; }
static int f2ms(int f) { return f * FRAME_MS; }

const char *amvd_result_str(amvd_result_t r)
{
	switch (r) {
	case AMVD_RES_HUMAN: return "human";
	case AMVD_RES_MACHINE: return "machine";
	case AMVD_RES_FAX: return "fax";
	case AMVD_RES_UNAVAILABLE: return "unavailable";
	case AMVD_RES_UNKNOWN: return "unknown";
	default: return "none";
	}
}

const char *amvd_end_str(amvd_end_t t)
{
	switch (t) {
	case AMVD_END_BEEP: return "beep";
	case AMVD_END_SILENCE: return "silence";
	case AMVD_END_OTHER: return "other";
	default: return "none";
	}
}

void amvd_features_csv(const double *f, char *buf, size_t len)
{
	size_t o = 0;
	int i;
	buf[0] = '\0';
	for (i = 0; i < AMVD_NUM_FEATURES && o < len; i++)
		o += (size_t)snprintf(buf + o, len - o, "%s%.4g", i ? "," : "", f[i]);
}

amvd_engine_t *amvd_engine_create(const amvd_config_t *cfg, int sample_rate, const amvd_model_t *model)
{
	amvd_engine_t *e;
	int i, n;

	/* integer multiples of 8 kHz: 8, 16, 24, 32, 48 kHz (all FreeSWITCH call rates) */
	if (sample_rate < WORK_RATE || sample_rate > MAX_RATE || sample_rate % WORK_RATE) return NULL;
	e = calloc(1, sizeof(*e));
	if (!e) return NULL;
	e->c = *cfg;
	e->model = model;
	e->in_rate = sample_rate;
	e->dec_m = sample_rate / WORK_RATE;
	e->rate = WORK_RATE;
	e->fs = WORK_RATE / 100;
	if (e->dec_m > 1) {
		/* low-pass at 3.6 kHz, Hamming-windowed sinc, unity DC gain */
		double fc = 3600.0 / sample_rate, sum = 0;
		int k, mid;
		e->dec_taps = 8 * e->dec_m + 1; /* AMD tolerates a soft roll-off; keep it cheap */
		mid = e->dec_taps / 2;
		for (k = 0; k < e->dec_taps; k++) {
			int t = k - mid;
			double sinc = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
			double w = 0.54 - 0.46 * cos(2 * M_PI * k / (e->dec_taps - 1));
			e->dec_h[k] = (float)(sinc * w);
			sum += sinc * w;
		}
		for (k = 0; k < e->dec_taps; k++) e->dec_h[k] = (float)(e->dec_h[k] / sum);
	}
	n = 2 * e->fs;
	for (i = 0; i < n; i++) e->hann[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)));

	e->min_word_f = ms2f(e->c.min_word_ms);
	e->word_gap_f = ms2f(e->c.word_gap_ms);
	e->seg_gap_f = ms2f(e->c.segment_gap_ms);
	e->window_f = ms2f(e->c.window_ms);

	e->noise_db = NOISE_INIT_DB;
	e->cand_start = -1;
	e->last_voiced = -1;
	e->beep_pending = -1;
	e->last_tone_end = -1;
	e->rep.first_speech_ms = -1;
	e->rep.p_machine = -1;
	e->rep.reason = "";
	return e;
}

void amvd_engine_destroy(amvd_engine_t *e) { free(e); }
const amvd_report_t *amvd_engine_report(const amvd_engine_t *e) { return &e->rep; }
int amvd_engine_elapsed_ms(const amvd_engine_t *e) { return f2ms(e->f); }

/* ------------------------------------------------------------------ */
/* Tone analysis: ZCR picks the neighbourhood, Goertzel measures purity */
/* ------------------------------------------------------------------ */

/* Purity of a Hann-windowed Goertzel at hz: ~1.0 for a clean sinusoid, ~2/N for noise. */
static double goertzel_purity(const amvd_engine_t *e, int n, double hz, double ew)
{
	double w = 2.0 * M_PI * hz / e->rate, coeff = 2.0 * cos(w);
	double s1 = 0, s2 = 0, p;
	int i;
	for (i = 0; i < n; i++) {
		double s0 = e->win[i] * e->hann[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
	return ew > 0 ? 3.0 * p / (n * ew) : 0.0;
}

static int analyze_tone(amvd_engine_t *e, double frame_db, double *hz_out)
{
	int n = 2 * e->fs, i, zc = 0;
	double ew = 0, mean = 0, best = 0, best_hz = 0, est;
	int k;

	if (frame_db < e->c.vad_floor_dbfs) return 0;
	for (i = 0; i < n; i++) mean += e->win[i];
	mean /= n;
	for (i = 0; i < n; i++) {
		double v = e->win[i] - mean;
		double vw = v * e->hann[i];
		ew += vw * vw;
		if (i && ((e->win[i - 1] - mean) < 0) != (v < 0)) zc++;
	}
	est = (double)zc * e->rate / (2.0 * n);
	if (est < 300 || est > 2600) return 0;
	for (k = -6; k <= 6; k++) {
		double hz = est * (1.0 + 0.01 * k); /* 1% grid: <0.2 dB scalloping with a 20 ms Hann */
		double p = goertzel_purity(e, n, hz, ew);
		if (p > best) { best = p; best_hz = hz; }
	}
	if (best < e->c.tone_purity) return 0;
	*hz_out = best_hz;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Decisions                                                           */
/* ------------------------------------------------------------------ */

static int total_speech_f(const amvd_engine_t *e)
{
	int i, t = 0;
	for (i = 0; i < e->nsegs; i++) {
		int end = e->segs[i].end;
		if (i == e->nsegs - 1 && e->in_seg) end = e->last_voiced;
		t += end - e->segs[i].start + 1;
	}
	return t;
}

static int total_words(const amvd_engine_t *e) { return e->words_total; }

static void compute_features(amvd_engine_t *e, int upto_f, double *x)
{
	int W = upto_f, i, nseg = 0, words = 0, longest = 0, maxgap = 0, gapsum = 0, ngaps = 0;
	int first_start = -1, first_len = 0, last_end = -1, speech = 0, last1 = 0, gap_after_first = 0;
	int l1_start = W - ms2f(1000);

	memset(x, 0, sizeof(double) * AMVD_NUM_FEATURES);
	if (W <= 0) W = 1;
	if (l1_start < 0) l1_start = 0;

	for (i = 0; i < e->nsegs; i++) {
		int s = e->segs[i].start, en = e->segs[i].end, len, a, b;
		if (i == e->nsegs - 1 && e->in_seg) en = e->last_voiced;
		if (s >= W) break;
		if (en >= W) en = W - 1;
		len = en - s + 1;
		if (first_start < 0) { first_start = s; first_len = len; }
		else {
			int gap = s - last_end - 1;
			if (nseg == 1) gap_after_first = gap;
			if (gap > maxgap) maxgap = gap;
			gapsum += gap; ngaps++;
		}
		if (len > longest) longest = len;
		speech += len;
		words += e->segs[i].words;
		a = s > l1_start ? s : l1_start;
		b = en;
		if (b >= a) last1 += b - a + 1;
		last_end = en;
		nseg++;
	}
	if (nseg == 1) gap_after_first = W - 1 - last_end;

	x[0] = f2ms(first_start < 0 ? W : first_start);
	x[1] = f2ms(first_len);
	x[2] = first_start < 0 ? 0.0 : (double)(last_end - first_start + 1) / W;
	x[3] = (double)speech / W;
	x[4] = nseg;
	x[5] = words;
	x[6] = f2ms(longest);
	x[7] = f2ms(maxgap);
	x[8] = ngaps ? f2ms(gapsum) / (double)ngaps : 0.0;
	x[9] = f2ms(gap_after_first);
	x[10] = (double)last1 / (W - l1_start > 0 ? W - l1_start : 1);
	x[11] = f2ms(last_end < 0 ? W : W - 1 - last_end);
	if (e->sp_n) {
		double m = e->sp_db_sum / e->sp_n;
		double v = e->sp_db_sq / e->sp_n - m * m;
		x[12] = m;
		x[13] = v > 0 ? sqrt(v) : 0.0;
	} else {
		x[12] = -96.0;
		x[13] = 0.0;
	}
	x[14] = e->noise_db;
	x[15] = e->tone_count_window;
}

static int decide(amvd_engine_t *e, amvd_result_t r, const char *reason)
{
	int ev = AMVD_EV_RESULT;
	e->rep.result = r;
	e->rep.reason = reason;
	e->rep.decision_ms = f2ms(e->f + 1);
	if (!e->window_taken) {
		compute_features(e, e->f + 1, e->rep.features);
		e->rep.features_ms = f2ms(e->f + 1);
	}
	if (e->model && e->rep.p_machine < 0) e->rep.p_machine = amvd_model_predict(e->model, e->rep.features);

	if (r == AMVD_RES_MACHINE && e->c.wait_message_end) {
		e->phase = PH_MSGEND;
	} else {
		e->phase = PH_DONE;
		ev |= AMVD_EV_DONE;
	}
	return ev;
}

static amvd_result_t default_result(const amvd_engine_t *e)
{
	switch (e->c.default_verdict) {
	case AMVD_DEFAULT_MACHINE: return AMVD_RES_MACHINE;
	case AMVD_DEFAULT_UNKNOWN: return AMVD_RES_UNKNOWN;
	default: return AMVD_RES_HUMAN;
	}
}

static int msg_end(amvd_engine_t *e, amvd_end_t t)
{
	e->rep.end_type = t;
	e->rep.end_ms = f2ms(e->f + 1);
	e->phase = PH_DONE;
	return AMVD_EV_MSG_END | AMVD_EV_DONE;
}

/* Called when a tone run finishes. Returns event bits. */
static int tone_finished(amvd_engine_t *e)
{
	tone_t t;
	int dur_ms, ev = 0;

	t.hz = e->tone_hz_sum / e->tone_n;
	t.start = e->tone_start;
	t.end = e->tone_last;
	e->tone_active = 0;
	dur_ms = f2ms(t.end - t.start + 1);
	e->last_tone_end = t.end;

	if (e->tone_hist_n == TONE_HIST) memmove(e->tone_hist, e->tone_hist + 1, sizeof(tone_t) * (TONE_HIST - 1));
	else e->tone_hist_n++;
	e->tone_hist[e->tone_hist_n - 1] = t;

	/* SIT: ~950 / ~1400 / ~1776 Hz, each 270-380 ms, back to back */
	if (e->c.sit_enabled && e->phase == PH_ANALYZE && e->tone_hist_n >= 3) {
		tone_t *a = &e->tone_hist[e->tone_hist_n - 3], *b = a + 1, *c = a + 2;
		int da = f2ms(a->end - a->start + 1), db = f2ms(b->end - b->start + 1), dc = f2ms(c->end - c->start + 1);
		if (a->hz > 870 && a->hz < 1030 && b->hz > 1320 && b->hz < 1480 && c->hz > 1720 && c->hz < 1830 &&
			da >= 180 && da <= 480 && db >= 180 && db <= 480 && dc >= 180 && dc <= 480 &&
			b->start - a->end <= 8 && c->start - b->end <= 8) {
			e->beep_pending = -1;
			return decide(e, AMVD_RES_UNAVAILABLE, "sit_tone");
		}
	}

	/* Electronic beeps hold frequency to a fraction of a percent; voiced speech
	 * harmonics and sung/musical notes drift. */
	if (t.hz >= e->c.beep_min_hz && t.hz <= e->c.beep_max_hz && dur_ms >= e->c.beep_min_ms && dur_ms <= e->c.beep_max_ms &&
		(e->tone_hz_max - e->tone_hz_min) <= t.hz * e->c.beep_freq_spread &&
		/* a beep follows a pause; a steady vowel is glued to the speech before it */
		(e->last_voiced < 0 || t.start - e->last_voiced > ms2f(e->c.beep_pre_quiet_ms))) {
		/* Confirm only after beep_post_ms of quiet: real beeps are followed by recording silence,
		 * musical notes inside a greeting are followed by more greeting. */
		e->beep_pending = t.end + ms2f(e->c.beep_post_ms);
		e->beep_pending_hz = t.hz;
		if (t.end < e->window_f) e->tone_count_window++;
	}
	return ev;
}

static int process_frame(amvd_engine_t *e, const int16_t *s)
{
	int fs = e->fs, i, ev = 0, voiced, is_tone = 0;
	double sum = 0, db, tone_hz = 0;
	const int f = e->f;

	/* shift 20 ms window, append this frame */
	memmove(e->win, e->win + fs, sizeof(float) * fs);
	for (i = 0; i < fs; i++) {
		double v = s[i] / 32768.0;
		e->win[fs + i] = (float)v;
		sum += v * v;
	}
	db = sum > 0 ? 10.0 * log10(sum / fs) : -96.0;
	if (db < -96.0) db = -96.0;

	/* ---------- tones ---------- */
	if (e->c.tones_enabled && f >= 1 && analyze_tone(e, db, &tone_hz)) {
		if (e->tone_active) {
			double mean = e->tone_hz_sum / e->tone_n;
			if (fabs(tone_hz - mean) <= mean * TONE_FREQ_TOL) {
				is_tone = 1;
				e->tone_last = f;
				e->tone_hz_sum += tone_hz;
				if (tone_hz < e->tone_hz_min) e->tone_hz_min = tone_hz;
				if (tone_hz > e->tone_hz_max) e->tone_hz_max = tone_hz;
				e->tone_n++;
				e->tone_miss = 0;
			} else {
				ev |= tone_finished(e);
				if (e->phase == PH_DONE) goto out;
			}
		}
		if (!e->tone_active && !is_tone) {
			e->tone_active = 1;
			e->tone_start = e->tone_last = f;
			e->tone_hz_sum = tone_hz;
			e->tone_hz_min = e->tone_hz_max = tone_hz;
			e->tone_n = 1;
			e->tone_miss = 0;
			is_tone = 1;
		}
		e->beep_pending = -1; /* another tone: wait for the sequence to finish */
	} else if (e->tone_active) {
		if (++e->tone_miss > TONE_DROPOUT) {
			ev |= tone_finished(e);
			if (e->phase == PH_DONE) goto out;
		}
	}

	/* fax CED: 2100 Hz held >= 400 ms */
	if (e->c.fax_enabled && e->tone_active && !e->fax_fired && e->phase == PH_ANALYZE) {
		double mean = e->tone_hz_sum / e->tone_n;
		if (fabs(mean - 2100.0) <= 50.0 && f2ms(e->tone_last - e->tone_start + 1) >= 400) {
			e->fax_fired = 1;
			ev |= decide(e, AMVD_RES_FAX, "fax_ced");
			goto out;
		}
	}

	/* ---------- noise floor: minimum statistics over the last 1.5 s ----------
	 * The minimum of 10 ms frame energies tracks stationary line noise even when
	 * the line is noisy from the very first frame (an "only learn when not voiced"
	 * tracker never learns in that case). Speech has dips between syllables and
	 * words, so continuous greetings do not drag the floor up to speech level. */
	{
		int k, n;
		float mn;
		e->noise_ring[e->noise_ring_pos] = (float)db;
		e->noise_ring_pos = (e->noise_ring_pos + 1) % NOISE_WIN;
		if (e->noise_ring_n < NOISE_WIN) e->noise_ring_n++;
		n = e->noise_ring_n;
		mn = e->noise_ring[0];
		for (k = 1; k < n; k++) if (e->noise_ring[k] < mn) mn = e->noise_ring[k];
		e->noise_db = mn + 1.5; /* bias: min of chi-square frame energies sits ~1.5 dB under the mean */
		if (e->noise_db < -90) e->noise_db = -90;
	}

	/* ---------- VAD ---------- */
	voiced = !is_tone && !e->tone_active && db > e->c.vad_floor_dbfs && db > e->noise_db + e->c.vad_margin_db;

	if (voiced && f < e->window_f) { e->sp_db_sum += db; e->sp_db_sq += db * db; e->sp_n++; }

	if (!e->in_word) {
		if (voiced) {
			if (e->cand_start < 0) { e->cand_start = f; e->cand_voiced = 0; }
			e->cand_voiced++;
			e->cand_gap = 0;
			if (e->cand_voiced >= e->min_word_f) {
				/* open word */
				e->in_word = 1;
				e->word_unvoiced = 0;
				e->last_voiced = f;
				e->words_total++;
				if (!e->in_seg) {
					if (e->nsegs < MAX_SEGS) {
						e->segs[e->nsegs].start = e->cand_start;
						e->segs[e->nsegs].end = f;
						e->segs[e->nsegs].words = 0;
						e->nsegs++;
					}
					e->in_seg = 1;
					if (e->rep.first_speech_ms < 0) e->rep.first_speech_ms = f2ms(e->cand_start);
				}
				if (e->nsegs) e->segs[e->nsegs - 1].words++;
				e->cand_start = -1;
				e->beep_pending = -1; /* speech after a tone: it was not a beep */
			}
		} else if (e->cand_start >= 0 && ++e->cand_gap > 2) {
			e->cand_start = -1;
		}
	} else {
		if (voiced) { e->last_voiced = f; e->word_unvoiced = 0; }
		else if (++e->word_unvoiced >= e->word_gap_f) e->in_word = 0;
	}
	if (e->in_seg && e->nsegs) {
		e->segs[e->nsegs - 1].end = e->last_voiced;
		if (!e->in_word && f - e->last_voiced >= e->seg_gap_f) e->in_seg = 0;
	}

	/* ---------- confirmed beep ---------- */
	if (e->beep_pending >= 0 && f >= e->beep_pending && !e->tone_active) {
		e->rep.beep_hz = e->beep_pending_hz;
		e->rep.beep_count++;
		e->beep_pending = -1;
		ev |= AMVD_EV_BEEP;
		if (e->phase == PH_ANALYZE && e->c.beep_is_machine) {
			ev |= decide(e, AMVD_RES_MACHINE, "beep");
			/* the beep IS the end of the greeting */
			if (e->phase == PH_MSGEND) ev |= msg_end(e, AMVD_END_BEEP);
			goto out;
		}
		if (e->phase == PH_MSGEND) { ev |= msg_end(e, AMVD_END_BEEP); goto out; }
	}

	/* ---------- phase logic ---------- */
	if (e->phase == PH_ANALYZE) {
		int t_ms = f2ms(f + 1);
		int sp_f = total_speech_f(e);
		int sp_ms = f2ms(sp_f);
		int speaking = e->in_word || e->in_seg;
		int last_act = e->last_voiced > e->last_tone_end ? e->last_voiced : e->last_tone_end;
		int sil_ms = last_act >= 0 ? f2ms(f - last_act) : t_ms; /* tones/music are not "waiting" */

		/* window checkpoint: freeze the feature vector the model was trained on */
		if (!e->window_taken && f + 1 >= e->window_f) {
			compute_features(e, e->window_f, e->rep.features);
			e->rep.features_ms = f2ms(e->window_f);
			e->window_taken = 1;
			if (e->model) {
				double p = amvd_model_predict(e->model, e->rep.features);
				e->rep.p_machine = p;
				if (p >= e->c.machine_threshold) { ev |= decide(e, AMVD_RES_MACHINE, "model"); goto out; }
				if (p <= e->c.human_threshold && e->nsegs > 0) { ev |= decide(e, AMVD_RES_HUMAN, "model"); goto out; }
			}
		}

		/* corpus feature extraction: no early decisions, stop at the window */
		if (e->c.features_only) {
			if (e->window_taken) { ev |= decide(e, default_result(e), "features_only"); }
			goto out;
		}

		/* machine: sustained speech */
		if (sp_ms >= e->c.machine_speech_ms) { ev |= decide(e, AMVD_RES_MACHINE, "long_greeting"); goto out; }
		/* machine: many words before any real pause */
		if (e->c.machine_max_words > 0 && total_words(e) >= e->c.machine_max_words && e->nsegs <= 2) {
			ev |= decide(e, AMVD_RES_MACHINE, "max_words");
			goto out;
		}

		/* human: short greeting then the callee waits for us */
		/* with a model loaded, human_defer_to_model holds this path until the model has spoken */
		if (e->nsegs > 0 && !speaking && !e->tone_active && e->beep_pending < 0 &&
			!(e->model && e->c.human_defer_to_model && !e->window_taken)) {
			double req = e->c.human_silence_base_ms + e->c.human_silence_slope * sp_ms;
			if (req > e->c.human_silence_max_ms) req = e->c.human_silence_max_ms;
			if (sp_ms <= e->c.human_max_greeting_ms && sil_ms >= req) {
				ev |= decide(e, AMVD_RES_HUMAN, "short_greeting");
				goto out;
			}
			/* business-style greeting (1.8-2.8 s) followed by a long wait */
			if (sp_ms > e->c.human_max_greeting_ms && sil_ms >= e->c.business_silence_ms) {
				ev |= decide(e, AMVD_RES_HUMAN, "business_greeting");
				goto out;
			}
		}

		/* nobody said anything */
		if (e->nsegs == 0 && t_ms >= e->c.initial_silence_ms && !e->tone_active && e->beep_pending < 0) {
			ev |= decide(e, default_result(e), "initial_silence");
			goto out;
		}

		/* hard stop */
		if (t_ms >= e->c.total_analysis_ms) {
			if (e->nsegs == 0) ev |= decide(e, default_result(e), "no_speech");
			else if (speaking) ev |= decide(e, AMVD_RES_MACHINE, "speech_at_timeout");
			else ev |= decide(e, default_result(e), "timeout");
			goto out;
		}
	} else if (e->phase == PH_MSGEND) {
		int t_ms = f2ms(f + 1);
		int last_act = e->last_voiced > e->last_tone_end ? e->last_voiced : e->last_tone_end;
		int quiet_ms = f2ms(f - last_act);
		if (!e->in_word && !e->in_seg && !e->tone_active && e->beep_pending < 0 && quiet_ms >= e->c.end_silence_ms) {
			ev |= msg_end(e, AMVD_END_SILENCE);
			goto out;
		}
		if (t_ms >= e->c.message_timeout_ms) { ev |= msg_end(e, AMVD_END_OTHER); goto out; }
	}

out:
	e->f++;
	return ev;
}

static int feed_work(amvd_engine_t *e, const int16_t *pcm, size_t n)
{
	int ev = 0;
	size_t i = 0;

	if (e->phase == PH_DONE) return 0;
	if (e->pend_n) {
		size_t need = (size_t)(e->fs - e->pend_n);
		size_t take = n < need ? n : need;
		memcpy(e->pend + e->pend_n, pcm, take * sizeof(int16_t));
		e->pend_n += (int)take;
		i = take;
		if (e->pend_n == e->fs) {
			ev |= process_frame(e, e->pend);
			e->pend_n = 0;
		}
	}
	while (e->phase != PH_DONE && n - i >= (size_t)e->fs) {
		ev |= process_frame(e, pcm + i);
		i += (size_t)e->fs;
	}
	if (e->phase != PH_DONE && i < n) {
		memcpy(e->pend, pcm + i, (n - i) * sizeof(int16_t));
		e->pend_n = (int)(n - i);
	}
	return ev;
}

int amvd_engine_feed(amvd_engine_t *e, const int16_t *pcm, size_t n)
{
	int16_t out[256];
	size_t i, o = 0;
	int ev = 0;

	if (e->dec_m == 1) return feed_work(e, pcm, n);
	if (e->phase == PH_DONE) return 0;
	for (i = 0; i < n; i++) {
		/* doubled ring buffer: the newest dec_taps samples are always contiguous */
		e->dec_buf[e->dec_pos] = e->dec_buf[e->dec_pos + e->dec_taps] = pcm[i];
		if (++e->dec_pos == e->dec_taps) e->dec_pos = 0;
		if (++e->dec_phase == e->dec_m) {
			const float *x = e->dec_buf + e->dec_pos; /* oldest .. newest */
			float acc = 0, acc2 = 0;
			int k;
			e->dec_phase = 0;
			for (k = 0; k + 1 < e->dec_taps; k += 2) { acc += x[k] * e->dec_h[k]; acc2 += x[k + 1] * e->dec_h[k + 1]; }
			if (k < e->dec_taps) acc += x[k] * e->dec_h[k];
			acc += acc2;
			if (acc > 32767) acc = 32767;
			if (acc < -32768) acc = -32768;
			out[o++] = (int16_t)lrint(acc);
			if (o == sizeof(out) / sizeof(out[0])) {
				ev |= feed_work(e, out, o);
				o = 0;
				if (e->phase == PH_DONE) return ev;
			}
		}
	}
	if (o) ev |= feed_work(e, out, o);
	return ev;
}

int amvd_engine_finish(amvd_engine_t *e)
{
	int ev = 0;
	if (e->phase == PH_ANALYZE) {
		if (e->f > 0) e->f--; /* decide() reports f+1 */
		if (e->nsegs == 0) ev |= decide(e, default_result(e), "stopped_no_speech");
		else if (e->in_seg && f2ms(total_speech_f(e)) > e->c.human_max_greeting_ms) ev |= decide(e, AMVD_RES_MACHINE, "stopped_in_speech");
		else ev |= decide(e, default_result(e), "stopped");
		e->f++;
	}
	if (e->phase == PH_MSGEND) ev |= msg_end(e, AMVD_END_OTHER);
	e->phase = PH_DONE;
	return ev | AMVD_EV_DONE;
}
