#!/usr/bin/env python3
"""Steady-state load driver (methodology from the RippleX review, Appendix A).

For each target load the driver ramps up to it, holds it until the network has
settled, and scores only a steady-state measurement window. A load level passes
when the network keeps validating a ledger at least every --max-interval seconds
(5 s by default) for the whole window. Throughput is read from the validated
ledgers, not from client-side confirmations.

Differences from rate_ramp_driver.py (kept for reproducing Phase 2):
  - ramp / settle / measure phases per level; nothing outside the measure
    window is scored, and there is no post-window grace period in the score;
  - the pass/fail lever is the validated-ledger interval, observed on the
    validators, not a client confirmation ratio;
  - terQUEUED is recorded as its own outcome, never as "accepted";
  - accounts are used strictly round-robin, so with a pool of at least
    rate x ledger interval no account submits twice within one ledger;
  - the generator is sharded over --procs processes, each with --threads
    workers on persistent HTTP connections, and it measures itself: every
    submit's scheduled time, send time, RPC latency and result are written to
    disk, together with generator CPU, so the report can show the generator
    delivered the target rate at every level.

Raw outputs (all under --out PREFIX):
  PREFIX.submits.pK.csv.gz     every submit from generator process K
  PREFIX.gen_cpu.csv           generator CPU per process, 1 s samples
  PREFIX.ledgers.jsonl         every validated ledger: first-seen time, tx count
  PREFIX.ledger_txs.csv.gz     ledger_index,hash for every validated tx
  PREFIX.server_info.jsonl     validators + stock nodes, 1 s samples
  PREFIX.fee.jsonl             open ledger / queue state from `fee`, 1 s samples
  PREFIX.schedule.json         the exact load schedule that was run
  PREFIX.online.json           the driver's online per-level verdicts
analyze_steady_state.py turns these into PREFIX.summary.json / .levels.csv.
"""

from __future__ import annotations

import argparse
import collections
import csv
import gzip
import json
import multiprocessing as mp
import os
import queue
import resource
import sys
import threading
import time

import bench_lib as bl

PHASES = ("ramp", "settle", "measure")


# ---------------------------------------------------------------------------
# Schedule
# ---------------------------------------------------------------------------


def build_schedule(rates, ramp_s, settle_s, measure_s, t0):
    """List of levels with absolute phase boundaries. The load ramps linearly
    from the previous level's rate (0 for the first) to this level's rate."""
    levels = []
    t = t0
    prev = 0
    for i, r in enumerate(rates):
        lv = {"level": i, "rate": r, "from_rate": prev}
        lv["ramp"] = [t, t + ramp_s]
        lv["settle"] = [t + ramp_s, t + ramp_s + settle_s]
        lv["measure"] = [t + ramp_s + settle_s, t + ramp_s + settle_s + measure_s]
        t = lv["measure"][1]
        prev = r
        levels.append(lv)
    return levels


def rate_at(schedule, t):
    """(rate, level, phase) at absolute time t; rate 0 outside the schedule."""
    for lv in schedule:
        if lv["ramp"][0] <= t < lv["ramp"][1]:
            a, b = lv["ramp"]
            f = (t - a) / (b - a) if b > a else 1.0
            return lv["from_rate"] + f * (lv["rate"] - lv["from_rate"]), lv["level"], 0
        if lv["settle"][0] <= t < lv["settle"][1]:
            return lv["rate"], lv["level"], 1
        if lv["measure"][0] <= t < lv["measure"][1]:
            return lv["rate"], lv["level"], 2
    return 0.0, -1, -1


def corpus_needed(rates, ramp_s, settle_s, measure_s):
    """Transactions the schedule submits if every level runs."""
    total = 0.0
    prev = 0
    for r in rates:
        total += (prev + r) / 2 * ramp_s + r * (settle_s + measure_s)
        prev = r
    return int(total)


# ---------------------------------------------------------------------------
# Generator process
# ---------------------------------------------------------------------------


def load_shard(corpus_path, k, nprocs):
    """Accounts are numbered by first appearance in the corpus; process k owns
    accounts with index % nprocs == k. Returns (ordered accounts, blobs by
    account, global account index)."""
    order: dict[str, int] = {}
    by_acct: dict[str, list] = collections.defaultdict(list)
    opener = gzip.open if corpus_path.endswith(".gz") else open
    with opener(corpus_path, "rt", encoding="utf-8") as f:
        for line in f:
            e = json.loads(line)
            a = e["account"]
            if a not in order:
                order[a] = len(order)
            if order[a] % nprocs == k:
                by_acct[a].append((e["seq"], e["blob"]))
    for a in by_acct:
        by_acct[a].sort()
    accts = sorted(by_acct, key=lambda a: order[a])
    return accts, by_acct, order


def generator_proc(k, args, rates, urls, stop_ev, counters, exhausted, ready_q, sched_ready, sched_t0):
    nprocs = args.procs
    accts, by_acct, order = load_shard(args.corpus, k, nprocs)
    cursor = {a: 0 for a in accts}
    acct_url = {a: order[a] % len(urls) for a in accts}
    pick_lock = threading.Lock()
    rr = [0]

    def next_blob():
        with pick_lock:
            n = len(accts)
            for _ in range(n):
                a = accts[rr[0]]
                rr[0] = (rr[0] + 1) % n
                i = cursor[a]
                if i < len(by_acct[a]):
                    cursor[a] = i + 1
                    return a, by_acct[a][i][1]
        return None, None

    ready_q.put((k, len(accts), sum(len(v) for v in by_acct.values())))
    # The controller fixes t0 once every shard is loaded, so loading time is
    # never part of the schedule.
    sched_ready.wait()
    schedule = build_schedule(rates, args.ramp, args.settle, args.measure, sched_t0.value)

    tokens: queue.Queue = queue.Queue(maxsize=max(64, args.threads * 4))
    rows: collections.deque = collections.deque()
    end_t = schedule[-1]["measure"][1]

    producer_done = threading.Event()

    def producer():
        t = schedule[0]["ramp"][0]
        while t < end_t and not stop_ev.is_set():
            r, lvl, ph = rate_at(schedule, t)
            if lvl < 0:
                t += 0.01
                continue
            # floor keeps the start of a ramp from 0 from jumping ahead
            per = max(r / nprocs, 0.5)
            while not stop_ev.is_set():
                try:
                    tokens.put((t, lvl, ph), timeout=0.5)
                    break
                except queue.Full:
                    continue
            t += 1.0 / per
        producer_done.set()

    def worker():
        conns: dict[int, bl.KeepAliveRpc] = {}
        while True:
            if stop_ev.is_set():
                return
            try:
                tok = tokens.get(timeout=0.5)
            except queue.Empty:
                if producer_done.is_set():
                    return
                continue
            deadline, lvl, ph = tok
            wait = deadline - time.time()
            if wait > 0:
                time.sleep(wait)
            a, blob = next_blob()
            if blob is None:
                exhausted.value = 1
                rows.append((deadline, time.time(), time.time(), lvl, ph, -1, -1, "exhausted", ""))
                continue
            ui = acct_url[a]
            c = conns.get(ui)
            if c is None:
                c = conns[ui] = bl.KeepAliveRpc(urls[ui], timeout=args.submit_timeout)
            t_sent = time.time()
            try:
                res = c.call("submit", {"tx_blob": blob})
                eng = res.get("engine_result") or res.get("error") or "noresult"
                h = (res.get("tx_json") or {}).get("hash", "")
            except Exception:  # noqa: BLE001
                eng, h = "rpc_error", ""
            t_done = time.time()
            rows.append((deadline, t_sent, t_done, lvl, ph, order[a], ui, eng, h))

    path = f"{args.out}.submits.p{k}.csv.gz"
    stop_writer = threading.Event()

    def writer():
        lag_late = args.gen_lag_max
        with gzip.open(path, "wt", newline="") as f:
            w = csv.writer(f)
            w.writerow(
                ["deadline", "sent", "done", "level", "phase", "account", "url", "engine", "hash"]
            )
            while True:
                batch = []
                while rows:
                    batch.append(rows.popleft())
                for d, s, dn, lvl, ph, a, ui, eng, h in batch:
                    w.writerow([f"{d:.4f}", f"{s:.4f}", f"{dn:.4f}", lvl, ph, a, ui, eng, h])
                    if ph == 2 and lvl >= 0:
                        base = lvl * 4
                        with counters.get_lock():
                            counters[base] += 1
                            if s - d > lag_late:
                                counters[base + 1] += 1
                            if eng in ("rpc_error", "exhausted"):
                                counters[base + 2] += 1
                if stop_writer.is_set() and not rows:
                    return
                time.sleep(0.2)

    def cpu_sampler():
        with open(f"{args.out}.gen_cpu.p{k}.csv", "w") as f:
            f.write("t,proc,cpu_pct,tokens_queued\n")
            last = resource.getrusage(resource.RUSAGE_SELF)
            lt = time.time()
            while not stop_writer.is_set():
                time.sleep(1.0)
                cur = resource.getrusage(resource.RUSAGE_SELF)
                now = time.time()
                used = (cur.ru_utime - last.ru_utime) + (cur.ru_stime - last.ru_stime)
                f.write(f"{now:.3f},{k},{100.0 * used / (now - lt):.1f},{tokens.qsize()}\n")
                f.flush()
                last, lt = cur, now

    wt = threading.Thread(target=writer, daemon=True)
    ct = threading.Thread(target=cpu_sampler, daemon=True)
    wt.start()
    ct.start()
    pt = threading.Thread(target=producer, daemon=True)
    pt.start()
    workers = [threading.Thread(target=worker, daemon=True) for _ in range(args.threads)]
    for th in workers:
        th.start()
    pt.join()
    for th in workers:
        th.join(timeout=args.submit_timeout + 5)
    stop_writer.set()
    wt.join()
    ct.join(timeout=5)


# ---------------------------------------------------------------------------
# Controller
# ---------------------------------------------------------------------------


def wait_ready(url, ledgers=3, timeout_s=120.0):
    start = bl.current_ledger(url)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if bl.current_ledger(url) >= start + ledgers:
            return
        time.sleep(0.5)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--urls", default="http://127.0.0.1:5005", help="submit endpoints (stock nodes)")
    ap.add_argument(
        "--validator-urls",
        default="",
        help="validator admin RPC endpoints; used to time ledger validation and "
        "sample job queue / round time (default: --urls)",
    )
    ap.add_argument("--corpus", default="corpus.jsonl")
    ap.add_argument("--out", default="steady")
    ap.add_argument("--rates", default="", help="explicit comma list; overrides start/step/cap")
    ap.add_argument("--rate-start", type=int, default=250)
    ap.add_argument("--rate-step", type=int, default=250)
    ap.add_argument("--rate-cap", type=int, default=4000)
    ap.add_argument("--ramp", type=float, default=30.0, help="seconds to ramp to each level")
    ap.add_argument("--settle", type=float, default=60.0, help="unscored steady seconds before the window")
    ap.add_argument("--measure", type=float, default=120.0, help="scored steady-state window, seconds")
    ap.add_argument("--drain", type=float, default=30.0, help="seconds to keep recording after the last level")
    ap.add_argument("--max-interval", type=float, default=5.0, help="pass if every validated-ledger interval <= this")
    ap.add_argument("--fails-to-stop", type=int, default=1, help="stop after this many failing levels")
    ap.add_argument("--procs", type=int, default=4, help="generator processes")
    ap.add_argument("--threads", type=int, default=48, help="submit threads per process")
    ap.add_argument("--submit-timeout", type=float, default=15.0)
    ap.add_argument("--gen-lag-max", type=float, default=1.0, help="a submit later than this vs schedule is 'late'")
    ap.add_argument("--gen-delivery-min", type=float, default=0.98, help="min sent/target in the window")
    ap.add_argument("--gen-late-max", type=float, default=0.01, help="max fraction of late submits in the window")
    ap.add_argument("--start-delay", type=float, default=5.0)
    ap.add_argument("--poll", type=float, default=0.1, help="validated-ledger poll period, seconds")
    args = ap.parse_args()

    urls = [u for u in args.urls.split(",") if u]
    vurls = [u for u in args.validator_urls.split(",") if u] or urls
    if args.rates:
        rates = [int(x) for x in args.rates.split(",") if x]
    else:
        rates = list(range(args.rate_start, args.rate_cap + 1, args.rate_step))
    if not rates:
        print("no rate levels", file=sys.stderr)
        return 1

    names = {f"validator-{i + 1}": u for i, u in enumerate(vurls)} if args.validator_urls else {}
    names.update({f"stock-{i + 1}": u for i, u in enumerate(urls)})

    print(f"Waiting for {len(vurls)} validator endpoint(s) to close ledgers ...", flush=True)
    wait_ready(vurls[0])

    rec = bl.ValidatedLedgerRecorder(vurls, urls[0], args.out, poll_s=args.poll)
    sinfo = bl.ServerInfoSampler(names, f"{args.out}.server_info.jsonl", period_s=1.0)
    fee = bl.FeeQueueSampler({f"stock-{i + 1}": u for i, u in enumerate(urls)}, f"{args.out}.fee.jsonl")
    rec.start()
    sinfo.start()
    fee.start()

    ctx = mp.get_context("fork" if sys.platform.startswith("linux") else "spawn")
    stop_ev = ctx.Event()
    exhausted = ctx.Value("i", 0)
    counters = ctx.Array("l", len(rates) * 4)  # per level: sent, late, errors, spare
    ready_q = ctx.Queue()

    need = corpus_needed(rates, args.ramp, args.settle, args.measure)
    sched_ready = ctx.Event()
    sched_t0 = ctx.Value("d", 0.0)
    print(f"Loading corpus shards into {args.procs} generator process(es) ...", flush=True)
    procs = []
    for k in range(args.procs):
        p = ctx.Process(
            target=generator_proc,
            args=(k, args, rates, urls, stop_ev, counters, exhausted, ready_q, sched_ready, sched_t0),
            daemon=True,
        )
        p.start()
        procs.append(p)
    loaded = [ready_q.get() for _ in range(args.procs)]
    n_accts = sum(x[1] for x in loaded)
    n_blobs = sum(x[2] for x in loaded)
    print(
        f"{n_accts} accounts, {n_blobs} pre-signed blobs; schedule needs ~{need} "
        f"if every level runs (levels {rates[0]}..{rates[-1]} tx/s, ramp {args.ramp:.0f}s, "
        f"settle {args.settle:.0f}s, measure {args.measure:.0f}s)",
        flush=True,
    )
    if n_blobs < need:
        print(
            f"WARNING: corpus covers {n_blobs}/{need}; the run stops (generator-limited) "
            "if it reaches a level the corpus cannot feed.",
            flush=True,
        )
    min_pool = max(rates) * args.max_interval
    if n_accts < min_pool:
        print(
            f"WARNING: pool {n_accts} < top rate x max interval ({min_pool:.0f}); some accounts "
            "will submit more than once per ledger at the top levels.",
            flush=True,
        )

    schedule_t0 = time.time() + args.start_delay
    schedule = build_schedule(rates, args.ramp, args.settle, args.measure, schedule_t0)
    sched_t0.value = schedule_t0
    sched_ready.set()
    with open(f"{args.out}.schedule.json", "w") as f:
        json.dump(
            {
                "t0": schedule_t0,
                "levels": schedule,
                "config": vars(args),
                "accounts": n_accts,
                "blobs": n_blobs,
                "validator_urls": vurls,
                "submit_urls": urls,
            },
            f,
            indent=2,
        )

    online = []
    fails = 0
    stop_reason = "completed all levels"
    for lv in schedule:
        m0, m1 = lv["measure"]
        while time.time() < m1 + 1.0:
            time.sleep(0.2)
        win = rec.window(m0, m1)
        base = lv["level"] * 4
        with counters.get_lock():
            sent, late, errs = counters[base], counters[base + 1], counters[base + 2]
        target = lv["rate"] * args.measure
        delivery = sent / target if target else 0.0
        late_frac = late / sent if sent else 1.0
        gen_ok = (
            delivery >= args.gen_delivery_min
            and late_frac <= args.gen_late_max
            and not exhausted.value
        )
        net_ok = win["ledgers"] > 0 and win["max_interval_s"] <= args.max_interval
        ledger_tps = win["txn_total"] / args.measure
        row = {
            "level": lv["level"],
            "rate": lv["rate"],
            "network_pass": net_ok,
            "generator_ok": gen_ok,
            "max_interval_s": win["max_interval_s"],
            "ledgers": win["ledgers"],
            "ledger_tps": round(ledger_tps, 1),
            "txn_per_ledger_mean": round(win["txn_total"] / win["ledgers"], 1) if win["ledgers"] else 0,
            "sent": sent,
            "target": target,
            "delivery": round(delivery, 4),
            "late_frac": round(late_frac, 4),
            "submit_errors": errs,
            "corpus_exhausted": bool(exhausted.value),
        }
        online.append(row)
        verdict = "PASS" if net_ok else "FAIL"
        if not gen_ok:
            verdict = "INVALID (generator did not deliver the target)"
        print(
            f"level {lv['rate']:>5} tx/s: {verdict:<5} max_interval={win['max_interval_s']:.2f}s "
            f"ledgers={win['ledgers']} ledger_tps={ledger_tps:.0f} "
            f"tx/ledger={row['txn_per_ledger_mean']} sent/target={delivery:.3f} late={late_frac:.3%}",
            flush=True,
        )
        if not gen_ok:
            stop_reason = f"generator-limited at {lv['rate']} tx/s"
            break
        if not net_ok:
            fails += 1
            if fails >= args.fails_to_stop:
                stop_reason = f"network over its limit at {lv['rate']} tx/s"
                break

    stop_ev.set()
    print(f"Stopping load ({stop_reason}); recording {args.drain:.0f}s drain ...", flush=True)
    for p in procs:
        p.join(timeout=args.submit_timeout + 60)
    hung = [i for i, p in enumerate(procs) if p.is_alive()]
    if hung:
        print(f"WARNING: generator process(es) {hung} did not exit; their submit logs may be truncated", flush=True)
        for i in hung:
            procs[i].terminate()
    time.sleep(args.drain)
    rec.stop()
    sinfo.stop()
    fee.stop()

    # merge per-process CPU files
    with open(f"{args.out}.gen_cpu.csv", "w") as out:
        out.write("t,proc,cpu_pct,tokens_queued\n")
        for k in range(args.procs):
            pth = f"{args.out}.gen_cpu.p{k}.csv"
            if os.path.exists(pth):
                with open(pth) as f:
                    next(f, None)
                    out.writelines(f)
                os.remove(pth)

    with open(f"{args.out}.online.json", "w") as f:
        json.dump({"stop_reason": stop_reason, "levels": online}, f, indent=2)

    import analyze_steady_state as an

    summary = an.analyze(args.out)
    cap = summary.get("capacity") or {}
    print(
        f"\nCapacity: {cap.get('rate', 0)} tx/s offered, "
        f"{cap.get('ledger_tps', 0)} tps from ledgers "
        f"({cap.get('txn_per_ledger_mean', 0)} tx/ledger); stop: {stop_reason}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
