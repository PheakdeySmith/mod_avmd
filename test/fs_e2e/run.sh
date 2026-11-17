#!/bin/bash
# End-to-end test: real FreeSWITCH + mod_amvd, real SIP/RTP, scored against corpus labels.
# Runs an isolated FreeSWITCH (SIP 15060, ESL 28021, RTP 40000-40500 on 127.0.0.1) next to
# any production instance; nothing is installed into the system FreeSWITCH.
#
#   test/fs_e2e/run.sh [corpus_dir] [calls_per_stratum] [profile]
#
# Env: FS_PREFIX (default /usr/local/freeswitch), FS_MODDIR (FreeSWITCH module dir).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CORPUS=${1:-$ROOT/build/corpus}
N=${2:-15}
PROFILE=${3:-ai-agent}
FS_PREFIX=${FS_PREFIX:-/usr/local/freeswitch}
FS_MODDIR=${FS_MODDIR:-$FS_PREFIX/lib/freeswitch/mod}
FS_CLI="$FS_PREFIX/bin/fs_cli -P 28021 -p ClueCon"
WORK=$ROOT/build/fs_e2e

[ -f "$ROOT/mod_amvd/mod_amvd.so" ] || { echo "build the module first (cd mod_amvd && make)"; exit 1; }
[ -f "$CORPUS/labels.csv" ] || { echo "no corpus at $CORPUS (python3 test/gen_corpus.py $CORPUS)"; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"/{mod,log,db,run,storage,recordings,sounds,temp,scripts,grammar}
cp -r "$HERE/conf" "$WORK/conf"
cp "$ROOT/mod_amvd/conf/amvd.conf.xml" "$WORK/conf/autoload_configs/"
for m in mod_console mod_logfile mod_commands mod_dptools mod_dialplan_xml mod_event_socket mod_sofia; do
	ln -s "$FS_MODDIR/$m.so" "$WORK/mod/"
done
ln -s "$ROOT/mod_amvd/mod_amvd.so" "$WORK/mod/"
python3 "$HERE/to_pcmu.py" "$CORPUS" "$WORK/audio"

cleanup() {
	$FS_CLI -x 'fsctl shutdown now' >/dev/null 2>&1 || true
	[ -n "${CALLEE:-}" ] && kill "$CALLEE" 2>/dev/null || true
}
trap cleanup EXIT

python3 "$HERE/sip_callee.py" --audio-dir "$WORK/audio" > "$WORK/log/callee.log" 2>&1 &
CALLEE=$!
# do not add -nosql: with it, timed media (file playback, RTP pacing) stalls on FreeSWITCH 1.10
"$FS_PREFIX/bin/freeswitch" -nc -nf \
	-conf "$WORK/conf" -log "$WORK/log" -db "$WORK/db" -run "$WORK/run" -mod "$WORK/mod" \
	-storage "$WORK/storage" -recordings "$WORK/recordings" -sounds "$WORK/sounds" -temp "$WORK/temp" \
	-scripts "$WORK/scripts" -grammar "$WORK/grammar" -base "$WORK" < /dev/null > "$WORK/log/console.txt" 2>&1 &
for _ in $(seq 1 30); do sleep 2; $FS_CLI -x 'amvd status' >/dev/null 2>&1 && break; done
$FS_CLI -x 'amvd status' | head -1

python3 "$HERE/run_calls.py" --corpus "$CORPUS" -n "$N" --profile "$PROFILE" ${AMVD_OUT:+--out "$AMVD_OUT"}
