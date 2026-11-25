# AMVD — Answering Machine / Voicemail Detection for FreeSWITCH

`mod_amvd` is a native FreeSWITCH module plus a portable C engine, an ESL controller in Go,
and the tooling to measure and train it on your own traffic.

It answers two questions, and treats both as first-class:

1. **Who answered?** `human`, `machine`, `fax`, `unavailable` (SIT tone), `unknown`
2. **When to act?** Immediately for a human. For a voicemail, at the beep, or at the end of the
   greeting when there is no beep (`amvd::message_end`), so the drop is never clipped.

```
callee audio (media bug, read-only, from answer)
  │
  ├─ adaptive VAD (minimum-statistics noise floor) → words → utterances
  ├─ tone detector (ZCR-guided Goertzel, 1 % grid) → beep · fax CED 2100 Hz · SIT triplet
  │
  ├─ fast rules ── short greeting + graded silence ──────────► human   (~0.95 s after "hello?")
  │             └─ long cumulative speech / many words / beep ► machine
  ├─ window checkpoint (3 s) ── optional trained tree model ──► human / machine
  ├─ hard stop (6 s) ─────────────────────────────────────────► default (human)
  │
  └─ message-end tracker (machine only, optional) ─► beep | silence | other
                     │
     events + vars + hooks ─► amvdctl (Go, ESL) ─► transfer to agent / LiveKit, VM drop, hang up
                                    ▲
                     transcript verdict from the LiveKit agent (screener, IVR, human→VM fix)
```

## Contents

| Path | What |
|---|---|
| `engine/amvd_engine.[ch]` | Portable C99 detection engine, no dependencies, any rate 8–48 kHz |
| `mod_amvd/` | FreeSWITCH module, `amvd.conf.xml` with 5 profiles, dialplan examples, Makefile, linker map |
| `controller/` | `amvdctl`: Go ESL controller (stdlib only): policies, VM drop, transcript verdicts, JSONL call log |
| `tools/amvd_cli.c` | Run the engine on WAV files (PCM16 / µ-law / A-law, mono or stereo) → JSON or CSV |
| `tools/train_model.py` | Train depth-2 boosted trees on your labelled calls, export for the module, suggest thresholds |
| `tools/evaluate.py` | False-machine / false-human rates with Wilson intervals, dead air, clipped drops, worst strata |
| `test/` | Synthetic 14-stratum corpus generator, chunk/rate invariance test, mock ESL servers (functional + load) |
| `bench/` | `bench_concurrency.c`: thousands of simultaneous detections on 20 ms frames, reports cores needed |

## What came from where

- **VICIdial / Asterisk `AMD()`** (`2000,2000,1000,5000,120,50,4,256`): the word/silence timing
  model and parameter vocabulary. Its known weaknesses are fixed. A fixed energy threshold becomes
  an adaptive noise floor. Initial silence is no longer "machine": silent pickups are usually
  people. Required silence now scales with greeting length instead of being one fixed value.
  (The forum thread itself returned a server error and could not be read.)
- **Evalgent article**: timing errors matter as much as classification errors. It supplied the
  cost-weighted threshold `P(machine) > C_FM / (C_FM + C_FH)`, the temporal-feature research
  (top features: speech spread, first-segment length, delay before speech; shallow boosted trees),
  the finding that beep detection alone scores badly, the 14 test strata, and the metric targets
  (false machine ≤ 1 % upper bound, dead air P95 ≤ 1.5 s, clipped drops ≤ 1 %).
- **LiveKit AMD**: the transcript is the strongest signal; "uncertain" means human; thin evidence
  defaults to human. The class set (`machine-vm`, `machine-ivr`, `machine-unavailable`) maps onto
  the controller's transcript endpoint.
- **nikvb/mod_amd_ws**: FreeSWITCH integration lessons. Exactly one `switch_core_media_bug_read()`
  per callback. All-`0xFFFF` frames are the core's fill for missing RTP. `SMBF_ONE_ONLY`. Hooks
  must never run under the bug lock. Hold a module read lock per detection. Export only the module
  symbol. Fail safe to human. (Its classifier is the amdy.io cloud service, so it is not used.)
- **W1ck3dZA/mod_amd**: a straight `app_amd` port (blocking only, fixed threshold, initial
  silence → MACHINE). Its `AMDSTATUS`/`AMDCAUSE` variables are kept for dialplan compatibility.

## Build

### Engine, CLI, tests (any Linux)

```bash
cd amvd
make                 # build/amvd_cli
make test            # ASan/UBSan invariance test + 420-call synthetic corpus + evaluation
```

### FreeSWITCH module

You need the FreeSWITCH headers. With SignalWire packages:

```bash
sudo apt-get install -y build-essential pkg-config libfreeswitch-dev
cd amvd/mod_amvd
make
sudo make install            # → /usr/lib/freeswitch/mod/mod_amvd.so + autoload_configs/amvd.conf.xml (if absent)
```

With a source build in `/usr/local/freeswitch`:

```bash
cd amvd/mod_amvd
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
make
sudo make install FS_MODDIR=/usr/local/freeswitch/mod FS_CONFDIR=/usr/local/freeswitch/conf
```

Without pkg-config:

```bash
make FS_INCLUDES=/usr/local/freeswitch/include/freeswitch FS_LIBS="-L/usr/local/freeswitch/lib -lfreeswitch"
```

Load and check:

```bash
fs_cli -x 'load mod_amvd'
fs_cli -x 'amvd status'
fs_cli -x 'amvd profiles'
# autoload: add <load module="mod_amvd"/> to autoload_configs/modules.conf.xml
```

### Controller

```bash
cd amvd/controller
go build -o amvdctl .
./amvdctl -esl fs1:8021,fs2:8021,fs3:8021 -password ClueCon \
  -log /var/log/amvd/calls.jsonl -http 127.0.0.1:8099 \
  -human-cmd 'uuid_transfer {uuid} ai_agent XML default' \
  -vm-cmd 'uuid_transfer {uuid} amvd_vm_machine XML default' \
  -machine-cmd 'uuid_kill {uuid} NORMAL_CLEARING' \
  -policy agent
```

## Use

**Start at answer.** The first syllables carry most of the signal. For originate-based campaigns:

```bash
fs_cli -x "originate {execute_on_answer='amvd_start ai-agent',amvd_policy=agent,amvd_campaign=42}sofia/gateway/trunk/855XXXXXXXX &park()"
fs_cli -x "originate {execute_on_answer='amvd_start voicemail-drop',amvd_policy=vmdrop}sofia/gateway/trunk/855XXXXXXXX &park()"
```

Background mode (`amvd_start` / `uuid_amvd start`) never holds the callee in silence. Synchronous
AMD that waits about 4 s makes people hang up. Act on the events instead.

| Interface | Form |
|---|---|
| App, blocking | `amvd [profile] [key=value ...]` → continues with `${amvd_result}` (and `${amvd_end_type}` with `wait-message-end`) |
| App, background | `amvd_start [profile] [key=value ...]`, `amvd_stop` |
| API | `uuid_amvd <uuid> start [profile] [key=value ...]`, `uuid_amvd <uuid> stop` |
| API | `amvd status`, `amvd reload` (live calls keep their settings), `amvd profiles` |
| Per-call override | `amvd_profile=...`, `amvd_params=machine-speech-ms=2500,window-ms=3000` |

Precedence: profile < `${amvd_params}` < app/API arguments.

### Results

| Variable | Values |
|---|---|
| `amvd_result` | `human` `machine` `fax` `unavailable` `unknown` `hangup` `error` |
| `amvd_reason` | `short_greeting` `business_greeting` `long_greeting` `max_words` `beep` `model` `fax_ced` `sit_tone` `initial_silence` `timeout` `speech_at_timeout` `no_speech` `stopped*` |
| `amvd_decision_ms`, `amvd_first_speech_ms`, `amvd_p_machine` | timing, score (`-1` if rules only) |
| `amvd_features` | 16 timing/level features (CSV, order = `amvd_feature_names`) |
| `amvd_end_type`, `amvd_end_ms`, `amvd_beep_hz` | message end: `beep` `silence` `other` |
| `amvd_no_audio_ms` | audio that was the core's no-RTP fill |
| `amvd_ring_ms`, `amvd_early_media` | channel created → answered, and whether early media (183) came first. A very short ring with no early media is a false-answer-supervision (FAS) suspect |
| `AMDSTATUS`, `AMDCAUSE` | `HUMAN`/`MACHINE`/`NOTSURE`/`HANGUP` and `HUMAN`/`LONGGREETING`/`MAXWORDS`/`INITIALSILENCE`/`BEEP`/`FAX`/`SIT`/`TOOLONG`/`MODEL` (mod_amd / VICIdial compatible) |

Events: `CUSTOM amvd::result`, `amvd::message_end`, `amvd::beep`, with headers `AMVD-Result`,
`AMVD-Reason`, `AMVD-Decision-MS`, `AMVD-P-Machine`, `AMVD-Features`, `AMVD-End-Type`,
`AMVD-End-MS`, `AMVD-Beep-Hz`, `AMVD-Profile`, `AMVD-Model`, `AMVD-No-Audio-MS`, and on results
`AMVD-Ring-MS`, `AMVD-Early-Media`, `AMVD-Gateway` (`sip_gateway_name`), plus the standard channel headers.

When nobody speaks, the verdict is `default-verdict`: `unknown` in the shipped profiles. Silence can be a
quiet human, dead air or FAS, so it is not labelled `human`. `amvdctl` still routes `unknown` like
`human`. The engine's own default (CLI, `make test`) is still `human`.

Hooks for background mode, with no controller needed. They run on a pool thread, never under the
bug lock, so they may transfer, record, or start other media bugs:

```xml
<action application="set" data="api_on_amvd_result=uuid_transfer ${uuid} amvd_\${amvd_result} XML ${context}"/>
<action application="set" data="api_on_amvd_message_end=uuid_transfer ${uuid} amvd_vm_machine XML ${context}"/>
<action application="amvd_start" data="voicemail-drop"/>
```

Full examples: `mod_amvd/conf/dialplan_examples.xml`.

## With LiveKit (your AI voice path)

1. `amvd_start ai-agent` at answer, on the PSTN leg, before bridging to the LiveKit SIP trunk.
2. Let the agent pre-generate its first reply (`preemptive_generation`) and release it on `human`.
   That recovers the latency without shortening thresholds, which is the cheaper trade per the
   Evalgent cost model.
3. Tones (fax, SIT, beep) belong to mod_amvd. They are signal-level and final.
4. If you also run LiveKit AMD, post its verdict to the controller. It may only move a call
   human → `machine-vm` / `machine-unavailable`, or flag `screener` / `machine-ivr` (so the agent
   answers the screener or navigates the IVR). It never overrides a voicemail drop. That gives one
   authority per class.

```bash
curl -X POST http://127.0.0.1:8099/v1/transcript-verdict \
  -d '{"uuid":"<freeswitch-uuid>","category":"machine-vm","transcript":"you have reached...","at_ms":2600}'
```

When the agent plays its first audio on the call, have the worker report it, so the log shows how
much dead air the callee heard after the verdict (`agent_delay_ms`):

```bash
curl -X POST http://127.0.0.1:8099/v1/agent-audio -d '{"uuid":"<freeswitch-uuid>"}'
```

Call screeners (iOS "Ask Reason for Calling", Pixel Call Screen) look like machines on timing
alone. Only the transcript separates them, which is why that path exists.

## Tuning

| Symptom (from `evaluate.py` / `amvdctl` log) | Change |
|---|---|
| Humans with longer hellos called machine | raise `machine-speech-ms`, `human-max-greeting-ms`; use `conservative` |
| Short voicemails with a mid-greeting pause called human | raise `human-silence-slope` / `human-silence-max-ms` (costs dead air; already raised to 0.7 / 1700, see below), or train a model with `human-defer-to-model=true` |
| Dead air too long for humans | lower `human-silence-base-ms` (≥ 450) |
| Quiet / noisy lines | `vad-margin-db` 7–12, `vad-floor-dbfs` −50…−40 |
| VM drop starts mid-greeting on no-beep mailboxes | raise `end-silence-ms` (2000–2500) |
| Music-on-greeting fires beeps | raise `tone-purity` to 0.85, lower `beep-max-ms` |
| `unknown` too frequent | check `amvd_no_audio_ms` (RTP problem), group by `gateway` / `ring_ms` in the `amvdctl` log (FAS routes), raise `initial-silence-ms` |
| Callees say "hello? hello?" after a `human` verdict | check `agent_delay_ms` in the `amvdctl` log (agent too slow to speak) and outbound RTP (one-way audio) |

Change one variable at a time and compare on the **same** corpus. Only call a change better when
the Wilson intervals don't overlap.

### Defaults tuned 2026-11 (synthetic corpus)

Swept on a 1,400-call training corpus (`gen_corpus.py --seed 101 -n 100`), then checked once on a
separate 840-call held-out corpus (`--seed 202 -n 60`) that tuning never saw:

| Param | Old | New | Why |
|---|---|---|---|
| `human-silence-slope` / `-max-ms` (default) | 0.5 / 1500 | 0.7 / 1700 | voicemails that pause after the first phrase were called human |
| `human-silence-slope` / `-max-ms` (`ai-agent`) | 0.45 / 1300 | 0.7 / 1600 | same; this profile had 8 % of machines called human |
| `total-analysis-ms` | 5000 | 6000 | noisy voicemails timed out to the default verdict (human) |
| `machine-max-words` | 6 | 7 | long business greetings ("thanks for calling …") hit the word rule |

Held-out results, `ai-agent` profile: machines called human 34/420 → 7/420 (offline) and 3/420
through FreeSWITCH; humans called machine 2/300 → 1/300; median dead air on humans
737 → 860 ms. The trained window model added nothing over these rules on held-out data, so
`model-file` stays empty. These values come from synthetic audio: re-tune on your recordings.

## Performance at high volume (5,000 concurrent calls)

All audio is downsampled inside the engine to 8 kHz, which is all AMD needs, so wideband and Opus
legs cost about the same as G.711. Measured with `bench/bench_concurrency.c`: 5,000 simultaneous
detections fed 20 ms frames round-robin, as FreeSWITCH media threads do (Xeon 2.1 GHz):

| Leg codec rate | CPU per detecting call | 5,000 *simultaneous* detections | Memory |
|---|---|---|---|
| 8 kHz (PCMU/PCMA) | 0.00031 cores | 1.6 cores | 5.1 KB/call → 26 MB |
| 16 kHz (G.722) | 0.00035 cores | 1.8 cores | same |
| 48 kHz (Opus) | 0.00058 cores | 2.9 cores | same |

Those are upper bounds. Detection only runs for part of each call, so simultaneous detections are
about calls-per-second × average detection time. Example: 5,000 CC with 60 s average calls gives
about 83 CPS. With humans decided in about 2.5 s and machines followed to message end in about 15 s,
that's under 1,000 detections at once, so well under one core.

```bash
gcc -O2 -std=c99 -pthread -o bench_concurrency bench/bench_concurrency.c engine/amvd_engine.c -lm
./bench_concurrency 5000 $(nproc) 20 real /path/to/recordings/*.wav
```

Controller: one `amvdctl` serves many nodes (`-esl fs1:8021,fs2:8021,...`); commands go back to
the node that owns the channel. It processed 100,000 calls from 2 nodes in about 3.4 s on a mock
socket (the mock was the bottleneck). ESL filters mean only AMD events, and hangups of channels
marked `amvd_active=true`, cross the socket. The JSONL log is buffered and flushed every second.
Calls whose hangup never arrived expire after `-call-ttl`.

At this volume, set in `amvd.conf.xml` (the shipped default):

```xml
<param name="event-channel-data" value="false"/>   <!-- slim event headers instead of 100+ channel vars -->
<param name="fire-beep-events" value="false"/>
```

Things that matter more than the detector at 5,000 CC:
- **FreeSWITCH itself** (RTP, jitter buffers, transcoding) is the real CPU cost. Spread 5,000 CC
  across several media nodes behind your Go control plane rather than one box.
- **Media must flow through FreeSWITCH.** Calls in `bypass_media` / `proxy_media` cannot be
  analysed. The media bug decodes the callee leg: free for G.711, real CPU for G.729/Opus if you
  weren't transcoding already.
- **Hooks** (`api_on_amvd_*`) run on FreeSWITCH's thread pool. For high volume prefer the
  controller over hooks: events are queued, and hooks spawn work inside the switch.
- Per-verdict log lines are INFO (one short line). Features are DEBUG. Run `amvdctl` without `-v`.

## Make it accurate on YOUR traffic

The rules work out of the box. Real accuracy comes from your own carriers, languages and mailbox
greetings. Khmer carrier greetings will not match English timing norms. The loop:

```bash
# 1. record the first 8 s of the CALLEE leg only, from answer (originate vars;
#    from fs_cli/ESL write \${uuid} so it expands on the channel, not in the console)
{execute_on_answer='amvd_start ai-agent',RECORD_READ_ONLY=true,execute_on_answer_2='record_session /var/amvd/rec/\${uuid}.wav +8'}

# 2. label: amvdctl writes /var/log/amvd/calls.jsonl; fill "truth" from agent dispositions/QA,
#    then make labels.csv:  file,truth[,stratum]   (truth = human | machine)

# 3. extract full-window features (no early decisions) with the same window-ms you deploy
./build/amvd_cli -f csv -s features_only=1 /var/amvd/rec/*.wav > feats.csv

# 4. train + export (cost-based and FMR-bounded thresholds are printed)
python3 tools/train_model.py feats.csv --labels labels.csv -o /etc/freeswitch/amvd/model.txt \
  --cost-false-machine 10 --cost-false-human 0.6 --max-fmr 0.01

# 5. compare rules vs model on held-out recordings
./build/amvd_cli -f csv -s wait_message_end=1 test/*.wav > rules.csv
./build/amvd_cli -f csv -s wait_message_end=1 -m /etc/freeswitch/amvd/model.txt test/*.wav > model.csv
python3 tools/evaluate.py labels_test.csv rules.csv model.csv

# 6. deploy: model-file + machine-threshold in amvd.conf.xml, then
fs_cli -x 'amvd reload'
```

Corpus sizing (from the Evalgent article): about 300 human answers with zero false machines bounds
the rate under 1 % (rule of three). Plan for about 500 human / 500 machine, with ≥ 50 per stratum.
Re-run after any carrier, codec, phone-OS, model or threshold change. Shadow-test 5–10 % of
production before switching.

## Verified here

- **Synthetic held-out corpus, 840 calls, 14 strata** (µ-law 8 kHz, source-filter speech,
  10 dB-SNR variants, music greetings, fax, SIT; `--seed 202`, not used for tuning), default
  profile: humans called machine 1/300 (0.33 %, 95 % CI upper bound 1.86 %; a long business
  greeting); machines called human 5/420 (1.2 %, mostly 10 dB-SNR voicemails); dead air on
  humans P50 960 ms / P95 1507 ms; VM drop fires a median 148 ms after the beep ends, with
  0 of 415 drops clipped.
- **CPU**: see the performance section. 5,000 simultaneous detections need 1.6–2.9 cores depending on codec rate.
- **Robustness**: same verdicts at 8/16/48 kHz (120/120 clips) and identical results across 1–4096-sample chunks;
  ASan + UBSan clean; model export matches sklearn exactly (float32 split semantics reproduced in C).
- **Module**: compiles warning-free (`-Wall -Wextra`) against FreeSWITCH v1.10 headers; the `.so`
  exports only `mod_amvd_module_interface`.
- **Controller**: `go vet` clean; tested against a mock event socket (routing, VM drop on
  message_end, transcript override, JSONL output).

- **Real FreeSWITCH (1.10.12), real SIP/RTP**: `test/fs_e2e/run.sh` starts an isolated
  FreeSWITCH (SIP 15060, ESL 28021, all on 127.0.0.1), loads `mod_amvd.so`, and places calls to
  `sip_callee.py`, which answers and streams each corpus file as PCMU RTP. On the 840-call held-out
  corpus with the `ai-agent` profile: 836/840 correct, 1/300 humans called machine, 3/420
  machines called human, all 60 fax and 60 SIT correct, 30 concurrent calls, no crashes.

  ```
  python3 test/gen_corpus.py build/heldout -n 60 --seed 202
  test/fs_e2e/run.sh build/heldout 60 ai-agent
  ```

**Not verified here:** real PSTN calls and long soak tests at production volume. Synthetic accuracy is
an upper bound: the research cited above saw 99.3 % on curated data drop to about 88 % in
production. Treat the numbers as "the logic works", then measure on your traffic with the loop above.
