#!/usr/bin/env python3
"""
Train the AMVD window classifier on YOUR traffic and export it for mod_amvd.

Approach follows the temporal-feature AMD research (VAD timing features over a
3-5 s window, shallow boosted trees): 16 timing/level features, depth-2 trees.
The model runs only at the window checkpoint; tones (beep/fax/SIT) and the
fast-path rules still act before it.

1) Extract full-window features from labelled recordings (callee leg only,
   starting at answer):

     amvd_cli -f csv -s features_only=1 -l human   human/*.wav   >  feats.csv
     amvd_cli -f csv -s features_only=1 -l machine machine/*.wav | tail -n +2 >> feats.csv

   or use --labels labels.csv (columns file,truth[,stratum]) instead of -l.
   The window length you extract with (window_ms, default 3000) must match
   the window_ms you deploy with.

2) Train + export:

     train_model.py feats.csv -o /etc/freeswitch/amvd/model.txt

3) Set model-file in amvd.conf.xml and the suggested machine-threshold.

The threshold suggestion is cost-based: act as machine only when
P(machine) > C_FM / (C_FM + C_FH), and also reports the threshold that keeps
the validation false-machine rate (humans called machine) under --max-fmr.
"""
import argparse
import csv
import math
import os
import sys

import numpy as np

FEATURES = [
    "init_silence_ms", "first_seg_ms", "speech_spread", "speech_ratio", "num_segments",
    "num_words", "longest_seg_ms", "max_gap_ms", "mean_gap_ms", "gap_after_first_ms",
    "speech_last_1s", "trailing_silence_ms", "speech_db_mean", "speech_db_std",
    "noise_floor_db", "tone_count",
]


def wilson(k, n, z=1.96):
    if n == 0:
        return (0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))


def load(paths, labels_csv):
    lab = {}
    if labels_csv:
        for r in csv.DictReader(open(labels_csv)):
            lab[os.path.basename(r["file"])] = r
    X, y, strata = [], [], []
    for p in paths:
        for r in csv.DictReader(open(p)):
            truth = r.get("label") or ""
            st = ""
            b = os.path.basename(r["file"])
            if b in lab:
                truth = truth or lab[b]["truth"]
                st = lab[b].get("stratum", "")
            truth = truth.strip().lower()
            if truth not in ("human", "machine"):
                continue  # fax/unavailable are handled by tone detection, not the model
            X.append([float(r[f]) for f in FEATURES])
            y.append(1 if truth == "machine" else 0)
            strata.append(st or truth)
    return np.array(X), np.array(y), np.array(strata)


def export(model, path):
    """sklearn GradientBoostingClassifier (log_loss, binary) -> amvd text model."""
    init = model.init_
    if hasattr(init, "class_prior_"):
        p = init.class_prior_[1]
    else:  # DummyClassifier(strategy='prior')
        p = init.class_prior_[1] if hasattr(init, "class_prior_") else 0.5
    base = math.log(p / (1 - p))
    lr = model.learning_rate
    lines = ["amvd-model 1", "features %d %s" % (len(FEATURES), " ".join(FEATURES)),
             "base %.17g" % base, "trees %d" % model.n_estimators_]
    for est in model.estimators_[:, 0]:
        t = est.tree_
        lines.append("tree %d" % t.node_count)
        for i in range(t.node_count):
            if t.children_left[i] == -1:
                lines.append("-1 0 0 0 %.17g" % (lr * t.value[i][0][0]))
            else:
                lines.append("%d %.17g %d %d 0" % (t.feature[i], t.threshold[i], t.children_left[i], t.children_right[i]))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def predict_exported(path, X):
    """Re-implement the C evaluator to prove the export is exact."""
    tok = open(path).read().split()
    i = 0
    assert tok[0] == "amvd-model"; i = 2
    nf = int(tok[i + 1]); i += 2 + nf
    base = float(tok[i + 1]); i += 2
    ntrees = int(tok[i + 1]); i += 2
    trees = []
    for _ in range(ntrees):
        n = int(tok[i + 1]); i += 2
        nodes = []
        for _ in range(n):
            nodes.append((int(tok[i]), float(tok[i + 1]), int(tok[i + 2]), int(tok[i + 3]), float(tok[i + 4])))
            i += 5
        trees.append(nodes)
    out = []
    for x in np.asarray(X, dtype=np.float32).astype(np.float64):
        raw = base
        for nodes in trees:
            n = 0
            while nodes[n][0] >= 0:
                n = nodes[n][2] if x[nodes[n][0]] <= nodes[n][1] else nodes[n][3]
            raw += nodes[n][4]
        out.append(1 / (1 + math.exp(-raw)))
    return np.array(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="amvd_cli -f csv -s features_only=1 output(s)")
    ap.add_argument("--labels", help="labels.csv with file,truth[,stratum]")
    ap.add_argument("-o", "--out", default="amvd_model.txt")
    ap.add_argument("--trees", type=int, default=50)
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--lr", type=float, default=0.1)
    ap.add_argument("--cost-false-machine", type=float, default=10.0, help="cost of hanging up on / VM-dropping a human")
    ap.add_argument("--cost-false-human", type=float, default=0.6, help="cost of sending a machine to the agent")
    ap.add_argument("--max-fmr", type=float, default=0.01, help="target false-machine rate on humans")
    ap.add_argument("--test-size", type=float, default=0.25)
    args = ap.parse_args()

    from sklearn.ensemble import GradientBoostingClassifier
    from sklearn.model_selection import train_test_split

    X, y, st = load(args.csv, args.labels)
    if len(set(y)) < 2:
        sys.exit("need both human and machine samples")
    print(f"samples: {len(y)}  human={int((y == 0).sum())}  machine={int((y == 1).sum())}")
    Xtr, Xte, ytr, yte, sttr, stte = train_test_split(X, y, st, test_size=args.test_size, stratify=y, random_state=1)
    clf = GradientBoostingClassifier(n_estimators=args.trees, max_depth=args.depth, learning_rate=args.lr,
                                     subsample=0.9, random_state=1)
    clf.fit(Xtr, ytr)
    export(clf, args.out)

    p = predict_exported(args.out, Xte)
    ref = clf.predict_proba(Xte)[:, 1]
    assert np.max(np.abs(p - ref)) < 1e-6, "export mismatch"

    cost_thr = args.cost_false_machine / (args.cost_false_machine + args.cost_false_human)
    humans = p[yte == 0]
    # smallest threshold whose upper Wilson bound on FMR stays under target, falls back to empirical
    cands = sorted(set(np.round(p, 4)) | {cost_thr})
    fmr_thr = None
    for t in cands:
        k = int((humans >= t).sum())
        if wilson(k, len(humans))[1] <= args.max_fmr or (k == 0 and t >= cost_thr):
            fmr_thr = t
            break

    def report(t, name):
        pred = p >= t
        fm = int(((yte == 0) & pred).sum()); nh = int((yte == 0).sum())
        fh = int(((yte == 1) & ~pred).sum()); nm = int((yte == 1).sum())
        lo, hi = wilson(fm, nh); lo2, hi2 = wilson(fh, nm)
        print(f"  {name:28s} thr={t:.3f}  acc={(pred == yte).mean():.1%}  false-machine={fm}/{nh} "
              f"({fm/max(nh,1):.1%}, 95% CI {lo:.1%}-{hi:.1%})  false-human={fh}/{nm} ({fh/max(nm,1):.1%}, CI {lo2:.1%}-{hi2:.1%})")

    print(f"held-out set: {len(yte)} calls")
    report(0.5, "p>=0.5")
    report(cost_thr, "cost-based")
    if fmr_thr is not None:
        report(fmr_thr, f"FMR<= {args.max_fmr:.1%} (upper bound)")
    imp = sorted(zip(clf.feature_importances_, FEATURES), reverse=True)[:6]
    print("top features: " + ", ".join(f"{n} {v:.2f}" for v, n in imp))
    rec = max(cost_thr, fmr_thr or 0)
    print(f"\nwrote {args.out}\nsuggested amvd.conf.xml: machine-threshold={rec:.3f}  human-threshold={min(0.35, rec / 2):.3f}")


if __name__ == "__main__":
    main()
