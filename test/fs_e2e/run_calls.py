#!/usr/bin/env python3
"""
Place real SIP calls through FreeSWITCH + mod_amvd and score the verdicts.

For each corpus file: originate from the 'uac' sofia profile to sip_callee.py with
amvd_start <profile> on answer, wait for the amvd::result event, kill the call.

  run_calls.py --corpus build/corpus [-n 15] [-j 20] [--profile ai-agent]
"""
import argparse
import collections
import csv
import socket
import time
import urllib.parse
import uuid


class ESL:
    def __init__(self, host, port, password):
        self.s = socket.create_connection((host, port))
        self.f = self.s.makefile("rwb", buffering=0)
        self.read()
        self.send("auth " + password)
        if "+OK" not in self.read()[0].get("Reply-Text", ""):
            raise SystemExit("ESL auth failed")

    def send(self, cmd):
        self.f.write((cmd + "\n\n").encode())

    def read(self):
        h = {}
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError
            line = line.decode().rstrip("\n")
            if line == "":
                if h:
                    break
                continue
            k, _, v = line.partition(": ")
            h[k] = v
        n = int(h.get("Content-Length", 0))
        return h, (self.f.read(n).decode() if n else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True, help="dir with *.wav and labels.csv (gen_corpus.py)")
    ap.add_argument("-n", type=int, default=15, help="calls per stratum")
    ap.add_argument("-j", type=int, default=20, help="concurrent calls")
    ap.add_argument("--profile", default="ai-agent")
    ap.add_argument("--target", default="sofia/uac/x@127.0.0.1:15090")
    ap.add_argument("--esl", default="127.0.0.1:28021")
    ap.add_argument("--password", default="ClueCon")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    labels = list(csv.DictReader(open(args.corpus + "/labels.csv")))
    per = collections.defaultdict(list)
    for r in labels:
        per[r["stratum"]].append(r)
    queue = [r for rows in per.values() for r in rows[:args.n]]
    total = len(queue)

    host, port = args.esl.split(":")
    esl = ESL(host, int(port), args.password)
    esl.send("event plain CUSTOM amvd::result")
    esl.read()

    pending, out = {}, {}

    def launch(r):
        u = str(uuid.uuid4())
        pending[u] = r
        esl.send("bgapi originate {origination_uuid=%s,ignore_early_media=true,originate_timeout=20,"
                 "execute_on_answer='amvd_start %s',sip_h_X-Wav=%s}%s &park()"
                 % (u, args.profile, r["file"], args.target))

    while queue and len(pending) < args.j:
        launch(queue.pop(0))
    t0 = time.time()
    deadline = t0 + args.timeout
    while (pending or queue) and time.time() < deadline:
        esl.s.settimeout(30)
        try:
            h, body = esl.read()
        except (socket.timeout, EOFError):
            break
        if h.get("Content-Type") != "text/event-plain":
            continue
        ev = dict(urllib.parse.unquote(x).split(": ", 1) for x in body.split("\n") if ": " in x)
        u = ev.get("Unique-ID")
        if ev.get("Event-Subclass") == "amvd::result" and u in pending:
            r = pending.pop(u)
            out[r["file"]] = (r, ev)
            esl.send("bgapi uuid_kill " + u)
            if queue:
                launch(queue.pop(0))

    print("calls with a verdict: %d / %d (%.0fs, profile %s)" % (len(out), total, time.time() - t0, args.profile))
    conf = collections.Counter((r["truth"], ev.get("AMVD-Result")) for r, ev in out.values())
    for (t, v), c in sorted(conf.items()):
        print("  truth=%-12s verdict=%-12s %d" % (t, v, c))
    humans = [1 for r, ev in out.values() if r["truth"] == "human"]
    machines = [1 for r, ev in out.values() if r["truth"] == "machine"]
    fm = conf[("human", "machine")]
    fh = conf[("machine", "human")]
    if humans:
        print("false-machine (humans->machine): %d/%d" % (fm, len(humans)))
    if machines:
        print("false-human (machines->human):   %d/%d" % (fh, len(machines)))
    for fn, (r, ev) in sorted(out.items()):
        if ev.get("AMVD-Result") != r["truth"]:
            print("  MISMATCH %s truth=%s verdict=%s reason=%s" % (fn, r["truth"], ev.get("AMVD-Result"), ev.get("AMVD-Reason")))
    print("\nper stratum (correct, decision ms from answer):")
    by = collections.defaultdict(list)
    for r, ev in out.values():
        by[r["stratum"]].append((r["truth"] == ev.get("AMVD-Result"), int(ev["AMVD-Decision-MS"])))
    for st, v in sorted(by.items()):
        ms = sorted(m for _, m in v)
        print("  %-20s %3d/%3d  P50=%4d P95=%4d" % (st, sum(ok for ok, _ in v), len(v),
                                                    ms[len(ms) // 2], ms[max(0, int(len(ms) * .95) - 1)]))
    missing = total - len(out)
    if missing:
        print("\n%d calls produced no verdict (check the FreeSWITCH log)" % missing)


if __name__ == "__main__":
    main()
