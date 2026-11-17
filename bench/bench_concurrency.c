/*
 * bench_concurrency - N simultaneous detections fed 20 ms frames round-robin, like
 * FreeSWITCH media threads. Calls that finish are replaced immediately, so N stay live.
 *
 *   bench_concurrency <calls> <threads> <seconds_of_audio_per_call> <mode> file.wav...
 *   mode: real  = normal rules (calls finish at their verdict, then restart)
 *         worst = no early verdicts, every call analysed for the whole duration
 *
 * Reports CPU seconds per call-second of audio and the cores needed for N concurrent calls.
 */
#define _GNU_SOURCE
#include "../engine/amvd_engine.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int16_t *pcm; size_t n; int rate; } clip_t;
static clip_t *clips; static int nclips;
static amvd_config_t cfg;
static int calls, threads, secs;

static int16_t *load(const char *p, size_t *n, int *rate) {
	FILE *f = fopen(p, "rb"); unsigned char h[44]; if (!f) return NULL;
	if (fread(h, 1, 44, f) != 44) { fclose(f); return NULL; }
	*rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
	fseek(f, 0, SEEK_END); long sz = ftell(f) - 44; fseek(f, 44, SEEK_SET);
	int16_t *b = malloc(sz); *n = fread(b, 2, sz / 2, f); fclose(f); return b;
}

typedef struct { int id; double cpu; long frames; long verdicts; } targ_t;

static void *worker(void *a) {
	targ_t *t = a; struct timespec c0, c1;
	int my = calls / threads + (t->id < calls % threads);
	amvd_engine_t **e = calloc(my, sizeof(*e)); size_t *pos = calloc(my, sizeof(size_t)); int *ci = calloc(my, sizeof(int));
	int rate = clips[0].rate, chunk = rate / 50; long total_frames = (long)secs * 50;
	for (int i = 0; i < my; i++) { ci[i] = (t->id * 7919 + i) % nclips; e[i] = amvd_engine_create(&cfg, rate, NULL); }
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
	for (long fr = 0; fr < total_frames; fr++) {
		for (int i = 0; i < my; i++) {
			clip_t *c = &clips[ci[i]];
			if (pos[i] + chunk > c->n) pos[i] = 0;
			int ev = amvd_engine_feed(e[i], c->pcm + pos[i], chunk);
			pos[i] += chunk; t->frames++;
			if (ev & AMVD_EV_DONE) {          /* call done: next call takes the slot */
				t->verdicts++; amvd_engine_destroy(e[i]);
				ci[i] = (ci[i] + 1) % nclips; pos[i] = 0;
				e[i] = amvd_engine_create(&cfg, rate, NULL);
			}
		}
	}
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
	t->cpu = (c1.tv_sec - c0.tv_sec) + (c1.tv_nsec - c0.tv_nsec) / 1e9;
	for (int i = 0; i < my; i++) amvd_engine_destroy(e[i]);
	return NULL;
}

int main(int argc, char **argv) {
	if (argc < 6) { fprintf(stderr, "usage: %s calls threads seconds real|worst file.wav...\n", argv[0]); return 2; }
	calls = atoi(argv[1]); threads = atoi(argv[2]); secs = atoi(argv[3]);
	amvd_config_defaults(&cfg); cfg.wait_message_end = 1;
	if (!strcmp(argv[4], "worst")) {
		const char *p = "machine_speech_ms=99999999,human_max_greeting_ms=0,business_silence_ms=99999999,"
		                "initial_silence_ms=99999999,total_analysis_ms=99999999,machine_max_words=0,"
		                "beep_is_machine=0,fax_enabled=0,sit_enabled=0";
		amvd_config_apply_string(&cfg, p, NULL, 0);
	}
	nclips = argc - 5; clips = calloc(nclips, sizeof(clip_t));
	for (int i = 0; i < nclips; i++) clips[i].pcm = load(argv[5 + i], &clips[i].n, &clips[i].rate);
	pthread_t th[64]; targ_t ta[64] = {0}; struct timespec w0, w1;
	clock_gettime(CLOCK_MONOTONIC, &w0);
	for (int i = 0; i < threads; i++) { ta[i].id = i; pthread_create(&th[i], NULL, worker, &ta[i]); }
	double cpu = 0; long frames = 0, verdicts = 0;
	for (int i = 0; i < threads; i++) { pthread_join(th[i], NULL); cpu += ta[i].cpu; frames += ta[i].frames; verdicts += ta[i].verdicts; }
	clock_gettime(CLOCK_MONOTONIC, &w1);
	double audio = frames * 0.02, wall = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;
	double per_call = cpu / audio;  /* cores per concurrent call */
	printf("%s rate=%d calls=%d threads=%d: %.0f call-seconds of audio, %.2fs CPU (%.2fs wall), %ld verdicts\n",
	       argv[4], clips[0].rate, calls, threads, audio, cpu, wall, verdicts);
	printf("  per concurrent call: %.5f cores (%.2f us CPU per 20 ms frame)  =>  %d concurrent calls need %.2f cores\n",
	       per_call, cpu / frames * 1e6, calls, per_call * calls);
	return 0;
}
