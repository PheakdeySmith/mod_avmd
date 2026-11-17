#!/usr/bin/env python3
"""
Evaluate AMVD output against ground truth (stdlib only).

  evaluate.py labels.csv results.csv [results_b.csv ...]

labels.csv : file,truth[,stratum][,greeting_end_ms][,beep_end_ms]
results    : amvd_cli -f csv output, or a CSV exported from the amvd::result events
             (needs file,result,decision_ms[,end_type,end_ms]).

Reports, per configuration (compare configs on the SAME corpus, one change at a time):
  - false-machine rate on humans with 95% Wilson interval (the expensive error)
  - false-human rate on machines
  - machine precision / recall
  - dead air on humans P50/P95  = decision_ms - greeting_end_ms
  - VM drop: offset after beep, clipped-drop rate (drop before recording started)
  - worst strata (error rate > 2x the class average)
Zero errors in n trials bounds the true rate near 3/n (rule of three) - size the corpus accordingly.
"""
import collections
import csv
import math
import os
import sys


def wilson(k, n, z=1.96):
    if n == 0:
        return (0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))


def pct(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def evaluate(lab, path):
    rows = list(csv.DictReader(open(path)))
    tp = fp = tn = fn = 0
    dead_air, offs, lat = [], [], []
    clipped = drops = 0
    per = collections.defaultdict(lambda: [0, 0])
    conf = collections.Counter()
    for r in rows:
        b = os.path.basename(r["file"])
        if b not in lab:
            continue
        l = lab[b]
        truth, pred = l["truth"].lower(), r["result"].lower()
        st = l.get("stratum") or truth
        conf[(truth, pred)] += 1
        per[st][0] += 1
        if truth != pred:
            per[st][1] += 1
        lat.append(int(r["decision_ms"]))
        if truth in ("human", "machine"):
            pm = pred == "machine"
            if truth == "machine":
                tp += pm; fn += not pm
            else:
                fp += pm; tn += not pm
                if not pm and l.get("greeting_end_ms"):
                    dead_air.append(int(r["decision_ms"]) - int(l["greeting_end_ms"]))
        if r.get("end_type") in ("beep", "silence", "other") and truth == "machine":
            drops += 1
            ref = l.get("beep_end_ms") or ""
            if ref:
                d = int(r["end_ms"]) - int(ref)
                offs.append(d)
                clipped += d < 0
    nh, nm = fp + tn, tp + fn
    lo, hi = wilson(fp, nh)
    lo2, hi2 = wilson(fn, nm)
    print(f"== {path}  ({sum(conf.values())} calls)")
    print(f"  false-machine (humans->machine): {fp}/{nh} = {fp/max(nh,1):.2%}  95% CI [{lo:.2%}, {hi:.2%}]   target upper <= 1%")
    print(f"  false-human (machines->human):   {fn}/{nm} = {fn/max(nm,1):.2%}  95% CI [{lo2:.2%}, {hi2:.2%}]   target <= 5%")
    print(f"  machine precision {tp/max(tp+fp,1):.2%}  recall {tp/max(nm,1):.2%}")
    print(f"  decision latency P50={pct(lat,.5)} ms P95={pct(lat,.95)} ms")
    if dead_air:
        print(f"  dead air on humans P50={pct(dead_air,.5)} ms P95={pct(dead_air,.95)} ms   target P95 <= 1500 (<2000 telemarketing)")
    if offs:
        print(f"  VM drop vs beep end: P50={pct(offs,.5)} ms min={min(offs)} ms; clipped {clipped}/{drops} = {clipped/max(drops,1):.2%}   target <= 1%")
    print("  confusion: " + ", ".join(f"{t}->{p}:{n}" for (t, p), n in sorted(conf.items())))
    class_err = (fp + fn) / max(nh + nm, 1)
    worst = [(s, e / n, n) for s, (n, e) in per.items() if n and e / n > max(2 * class_err, 0.02)]
    for s, rate, n in sorted(worst, key=lambda x: -x[1]):
        print(f"  WORST stratum {s}: {rate:.1%} errors over {n}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    lab = {os.path.basename(r["file"]): r for r in csv.DictReader(open(sys.argv[1]))}
    for p in sys.argv[2:]:
        evaluate(lab, p)


if __name__ == "__main__":
    main()
