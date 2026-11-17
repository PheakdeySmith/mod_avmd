/*
 * amvd_cli - run the AMVD engine over WAV files.
 *
 *   amvd_cli [options] file.wav [file.wav ...]
 *
 * Options:
 *   -s key=value     override a config key (repeatable), e.g. -s machine_speech_ms=2500
 *   -p "k=v,k=v"     override several keys at once
 *   -m model.txt     use a trained tree model (from train_model.py)
 *   -c channel       channel to analyse for stereo files (0=left, 1=right; default 0)
 *   -o 0             start offset in ms (skip ringback/early media in recordings)
 *   -f csv|json      output format (default json)
 *   -l label         add a label column (for building training CSVs)
 *
 * Supports WAV PCM16, 8-bit mu-law (fmt 7) and A-law (fmt 6), any of 8/16/32/48 kHz.
 * CSV output = file,label,result,reason,p_machine,decision_ms,end_type,end_ms,beep_hz,<16 features>
 */
#include "../engine/amvd_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int16_t ulaw2lin(uint8_t u)
{
	int t;
	u = ~u;
	t = ((u & 0x0F) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

static int16_t alaw2lin(uint8_t a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 0x0F) << 4;
	seg = (a & 0x70) >> 4;
	switch (seg) {
	case 0: t += 8; break;
	case 1: t += 0x108; break;
	default: t += 0x108; t <<= seg - 1;
	}
	return (int16_t)((a & 0x80) ? t : -t);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Returns malloc'd mono int16 buffer */
static int16_t *load_wav(const char *path, int chan, int *rate, size_t *nsamp, char *err, size_t el)
{
	FILE *f = fopen(path, "rb");
	uint8_t hdr[12], ch[8];
	int fmt = 0, nch = 0, bits = 0, have_fmt = 0;
	int16_t *out = NULL;

	if (!f) { snprintf(err, el, "cannot open"); return NULL; }
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { snprintf(err, el, "not a WAV"); goto done; }
	while (fread(ch, 1, 8, f) == 8) {
		uint32_t sz = rd32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			uint8_t b[40];
			size_t rd = sz < sizeof(b) ? sz : sizeof(b);
			if (fread(b, 1, rd, f) != rd) break;
			if (sz > rd) fseek(f, (long)(sz - rd), SEEK_CUR);
			fmt = rd16(b); nch = rd16(b + 2); *rate = (int)rd32(b + 4); bits = rd16(b + 14);
			if (fmt == 0xFFFE && rd >= 26) fmt = rd16(b + 24); /* WAVE_FORMAT_EXTENSIBLE */
			have_fmt = 1;
		} else if (!memcmp(ch, "data", 4)) {
			uint8_t *raw;
			size_t bps, frames, i;
			if (!have_fmt) { snprintf(err, el, "data before fmt"); goto done; }
			if (!((fmt == 1 && bits == 16) || ((fmt == 6 || fmt == 7) && bits == 8))) { snprintf(err, el, "unsupported format %d/%d bits", fmt, bits); goto done; }
			if (chan >= nch) chan = 0;
			bps = (size_t)(bits / 8);
			raw = malloc(sz);
			if (!raw) goto done;
			sz = (uint32_t)fread(raw, 1, sz, f);
			frames = sz / (bps * (size_t)nch);
			out = malloc(frames * sizeof(int16_t) + 1);
			for (i = 0; out && i < frames; i++) {
				const uint8_t *p = raw + (i * (size_t)nch + (size_t)chan) * bps;
				out[i] = fmt == 1 ? (int16_t)rd16(p) : fmt == 7 ? ulaw2lin(*p) : alaw2lin(*p);
			}
			*nsamp = frames;
			free(raw);
			goto done;
		} else {
			fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
		}
	}
	snprintf(err, el, "no data chunk");
done:
	fclose(f);
	return out;
}

static void json_escape(const char *s, FILE *o)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') fputc('\\', o);
		fputc(*s, o);
	}
}

int main(int argc, char **argv)
{
	amvd_config_t cfg;
	amvd_model_t *model = NULL;
	const char *fmt = "json", *label = "";
	int opt, chan = 0, offset_ms = 0, i, csv_header_done = 0;
	char err[512];

	amvd_config_defaults(&cfg);
	while ((opt = getopt(argc, argv, "s:p:m:c:o:f:l:h")) != -1) {
		switch (opt) {
		case 's': {
			char *eq = strchr(optarg, '=');
			if (!eq) { fprintf(stderr, "bad -s %s\n", optarg); return 2; }
			*eq = '\0';
			if (amvd_config_set(&cfg, optarg, eq + 1)) { fprintf(stderr, "bad key/value %s=%s\n", optarg, eq + 1); return 2; }
			break;
		}
		case 'p':
			if (amvd_config_apply_string(&cfg, optarg, err, sizeof(err))) { fprintf(stderr, "bad params: %s\n", err); return 2; }
			break;
		case 'm':
			model = amvd_model_load(optarg, err, sizeof(err));
			if (!model) { fprintf(stderr, "model: %s\n", err); return 2; }
			break;
		case 'c': chan = atoi(optarg); break;
		case 'o': offset_ms = atoi(optarg); break;
		case 'f': fmt = optarg; break;
		case 'l': label = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-s k=v] [-p k=v,..] [-m model] [-c chan] [-o offset_ms] [-f csv|json] [-l label] file.wav...\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	for (i = optind; i < argc; i++) {
		int rate = 0, ev = 0;
		size_t n = 0, pos, chunk;
		int16_t *pcm = load_wav(argv[i], chan, &rate, &n, err, sizeof(err));
		amvd_engine_t *e;
		const amvd_report_t *r;
		char feats[1024];

		if (!pcm) { fprintf(stderr, "%s: %s\n", argv[i], err); continue; }
		e = amvd_engine_create(&cfg, rate, model);
		if (!e) { fprintf(stderr, "%s: unsupported rate %d\n", argv[i], rate); free(pcm); continue; }
		pos = (size_t)offset_ms * (size_t)rate / 1000;
		chunk = (size_t)rate / 50; /* feed 20 ms like a media bug */
		while (pos < n && !(ev & AMVD_EV_DONE)) {
			size_t take = n - pos < chunk ? n - pos : chunk;
			ev |= amvd_engine_feed(e, pcm + pos, take);
			pos += take;
		}
		if (!(ev & AMVD_EV_DONE)) amvd_engine_finish(e); /* recording ended before a verdict */
		r = amvd_engine_report(e);
		amvd_features_csv(r->features, feats, sizeof(feats));

		if (!strcmp(fmt, "csv")) {
			if (!csv_header_done) {
				int k;
				printf("file,label,result,reason,p_machine,decision_ms,end_type,end_ms,beep_hz");
				for (k = 0; k < AMVD_NUM_FEATURES; k++) printf(",%s", amvd_feature_names[k]);
				printf("\n");
				csv_header_done = 1;
			}
			printf("%s,%s,%s,%s,%.4f,%d,%s,%d,%.0f,%s\n", argv[i], label, amvd_result_str(r->result), r->reason,
				   r->p_machine, r->decision_ms, amvd_end_str(r->end_type), r->end_ms, r->beep_hz, feats);
		} else {
			printf("{\"file\":\"");
			json_escape(argv[i], stdout);
			printf("\",\"result\":\"%s\",\"reason\":\"%s\",\"p_machine\":%.4f,\"decision_ms\":%d,\"first_speech_ms\":%d,"
				   "\"end_type\":\"%s\",\"end_ms\":%d,\"beep_hz\":%.0f,\"beeps\":%d,\"features\":[%s]}\n",
				   amvd_result_str(r->result), r->reason, r->p_machine, r->decision_ms, r->first_speech_ms,
				   amvd_end_str(r->end_type), r->end_ms, r->beep_hz, r->beep_count, feats);
		}
		amvd_engine_destroy(e);
		free(pcm);
	}
	amvd_model_free(model);
	return 0;
}
