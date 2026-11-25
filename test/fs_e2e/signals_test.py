#!/usr/bin/env python3
"""
Check the per-call answer signals end to end on the fs_e2e FreeSWITCH (run by run.sh):

  - AMVD-Ring-MS / AMVD-Early-Media for an instant answer (FAS-like), 180 + delay and
    183 (early media) + delay, using sip_callee.py's X-Ring header
  - AMVD-Gateway for a call sent through the carrier_test gateway
  - with --amvdctl: the controller's JSONL row carries the same fields, and a
    POST /v1/agent-audio fills agent_delay_ms

  signals_test.py --wav <corpus file name> [--amvdctl build/amvdctl]

Exits non-zero on any failed check.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

from run_calls import ESL

CASES = [
    # name, dial string, X-Ring, expected ring_ms range, expected early media, expected gateway
    ("instant answer (FAS-like)", "sofia/uac/x@127.0.0.1:15090", "", (0, 400), "false", ""),
    ("180 ringing, 2 s", "sofia/uac/x@127.0.0.1:15090", "180:2000", (1900, 2600), "false", ""),
    ("183 early media, 1.5 s", "sofia/uac/x@127.0.0.1:15090", "183:1500", (1400, 2100), "true", ""),
    ("through gateway, 180 1 s", "sofia/gateway/carrier_test/x", "180:1000", (900, 1600), "false", "carrier_test"),
]


def post(url, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST")
    try:
        return urllib.request.urlopen(req, timeout=2).status
    except urllib.error.HTTPError as e:
        return e.code
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True, help="corpus file to stream (a human hello works best)")
    ap.add_argument("--profile", default="ai-agent")
    ap.add_argument("--esl", default="127.0.0.1:28021")
    ap.add_argument("--password", default="ClueCon")
    ap.add_argument("--amvdctl", help="controller binary; also checks its JSONL log and /v1/agent-audio")
    ap.add_argument("--http", default="127.0.0.1:28099", help="controller HTTP address for the test")
    args = ap.parse_args()

    fails = []

    def check(ok, what):
        print("  %s %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    ctl, logpath = None, None
    if args.amvdctl:
        logpath = os.path.join(tempfile.mkdtemp(prefix="amvdctl_"), "calls.jsonl")
        # policy detect: log only, never transfer or hang up
        ctl = subprocess.Popen([args.amvdctl, "-esl", args.esl, "-password", args.password,
                                "-http", args.http, "-log", logpath, "-policy", "detect"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    host, port = args.esl.split(":")
    esl = ESL(host, int(port), args.password)
    esl.send("event plain CUSTOM amvd::result")
    esl.read()

    sent = {}
    try:
        for name, dial, ring, (lo, hi), early, gw in CASES:
            u = str(uuid.uuid4())
            sent[u] = name
            esl.send("bgapi originate {origination_uuid=%s,ignore_early_media=true,originate_timeout=20,"
                     "execute_on_answer='amvd_start %s',sip_h_X-Wav=%s%s}%s &park()"
                     % (u, args.profile, args.wav, (",sip_h_X-Ring=" + ring) if ring else "", dial))
            ev, deadline = None, time.time() + 20
            while time.time() < deadline:
                esl.s.settimeout(max(0.1, deadline - time.time()))
                try:
                    h, body = esl.read()
                except socket.timeout:
                    break
                if h.get("Content-Type") != "text/event-plain":
                    continue
                e = dict(urllib.parse.unquote(x).split(": ", 1) for x in body.split("\n") if ": " in x)
                if e.get("Event-Subclass") == "amvd::result" and e.get("Unique-ID") == u:
                    ev = e
                    break
            print("%s: %s" % (name, "result=%s reason=%s ring_ms=%s early_media=%s gateway=%r" % (
                ev.get("AMVD-Result"), ev.get("AMVD-Reason"), ev.get("AMVD-Ring-MS"),
                ev.get("AMVD-Early-Media"), ev.get("AMVD-Gateway")) if ev else "no amvd::result"))
            if not ev:
                fails.append(name + ": no result")
                esl.send("bgapi uuid_kill " + u)
                continue
            rms = int(ev.get("AMVD-Ring-MS", "-1"))
            check(lo <= rms <= hi, "ring_ms %d in [%d, %d]" % (rms, lo, hi))
            check(ev.get("AMVD-Early-Media") == early, "early_media == %s" % early)
            check(ev.get("AMVD-Gateway", "") == gw, "gateway == %r" % gw)
            if ctl:
                # the agent's first audio, ~300 ms after the verdict
                time.sleep(0.3)
                code = post("http://%s/v1/agent-audio" % args.http, {"uuid": u})
                check(code == 204, "POST /v1/agent-audio -> %d" % code)
            esl.send("bgapi uuid_kill " + u)
    finally:
        if ctl:
            time.sleep(2.5)  # hangups reach the controller; its log flushes every second
            ctl.terminate()
            ctl.wait()

    if ctl:
        rows = {}
        for line in open(logpath):
            d = json.loads(line)
            rows[d["uuid"]] = d
        print("amvdctl log (%d rows):" % len(rows))
        for (name, dial, ring, (lo, hi), early, gw), u in zip(CASES, sent):
            d = rows.get(u)
            if not d:
                check(False, "%s: row in amvdctl log" % name)
                continue
            print("%s: ring_ms=%s early_media=%s gateway=%r no_audio_ms=%s agent_delay_ms=%s hangup=%s" % (
                name, d.get("ring_ms"), d.get("early_media"), d.get("gateway", ""), d.get("no_audio_ms"),
                d.get("agent_delay_ms"), d.get("hangup_cause")))
            check(lo <= d.get("ring_ms", -1) <= hi, "log ring_ms in range")
            check(str(d.get("early_media")).lower() == early, "log early_media == %s" % early)
            check(d.get("gateway", "") == gw, "log gateway == %r" % gw)
            ad = d.get("agent_delay_ms")
            check(ad is not None and 250 <= ad <= 1500, "log agent_delay_ms %s in [250, 1500]" % ad)

    print("\nsignals: %s" % ("all checks passed" if not fails else "%d FAILED" % len(fails)))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
