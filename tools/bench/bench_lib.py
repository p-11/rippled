"""Shared helpers for the hybrid-PQ benchmark harness.

Signs the corpus via the node's `sign` RPC rather than a client library so the
same path produces both ECC-only and hybrid (pq_seed_hex) blobs without needing
client-side support for the custom quantum fields.
"""

from __future__ import annotations

import gzip
import http.client
import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

GENESIS_ACCOUNT = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
GENESIS_SECRET = "masterpassphrase"
ASF_QUANTUM = 18  # AccountSet flag from include/xrpl/protocol/TxFlags.h


def percentile(values: list[float], p: float) -> float:
    """Nearest-rank percentile (p in 0..100). Shared so the driver, the
    aggregator, and the probes all report the same statistic."""
    if not values:
        return float("nan")
    s = sorted(values)
    k = min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))
    return s[k]


class RpcError(RuntimeError):
    pass


def rpc(
    url: str, method: str, params: dict, *, timeout: float = 15.0, retries: int = 0
) -> dict:
    body = json.dumps({"method": method, "params": [params]}).encode()
    attempt = 0
    while True:
        req = urllib.request.Request(
            url, data=body, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                payload = json.loads(resp.read())
            return payload.get("result", {})
        except urllib.error.HTTPError as e:
            # rippled sheds RPCs with 500/503 under high load factor; a heavy
            # pre-signing pass (pure compute, no ledger effect) can trip this, so
            # retrying is safe rather than aborting the run.
            if e.code not in (500, 503) or attempt >= retries:
                raise
        except (urllib.error.URLError, TimeoutError, OSError):
            if attempt >= retries:
                raise
        attempt += 1
        time.sleep(min(2.0, 0.1 * 2**attempt))


def rpc_checked(url: str, method: str, params: dict, **kw) -> dict:
    result = rpc(url, method, params, **kw)
    if result.get("status") != "success":
        raise RpcError(f"{method} failed: {json.dumps(result)[:400]}")
    return result


def wait_for_server(url: str, timeout_s: float = 120.0) -> None:
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        try:
            r = rpc(url, "server_info", {})
            state = r.get("info", {}).get("server_state", "")
            if state in {"proposing", "full", "tracking"}:
                return
        except (urllib.error.URLError, OSError, RpcError) as e:
            last = e
        time.sleep(2)
    raise RpcError(f"{url} not ready within {timeout_s}s ({last})")


def current_ledger(url: str) -> int:
    r = rpc(url, "ledger", {"ledger_index": "validated"})
    idx = r.get("ledger_index") or r.get("ledger", {}).get("ledger_index")
    return int(idx) if idx is not None else 0


def wait_ledgers(url: str, count: int, timeout_s: float = 60.0) -> None:
    """Block until `count` validated ledgers have closed."""
    start = current_ledger(url)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if current_ledger(url) >= start + count:
            return
        time.sleep(0.5)
    raise RpcError(
        f"only saw {current_ledger(url) - start}/{count} ledgers in {timeout_s}s"
    )


def fund(url: str, dest: str, drops: str) -> None:
    r = rpc_checked(
        url,
        "submit",
        {
            "secret": GENESIS_SECRET,
            "tx_json": {
                "TransactionType": "Payment",
                "Account": GENESIS_ACCOUNT,
                "Destination": dest,
                "Amount": drops,
            },
        },
    )
    if r.get("engine_result") != "tesSUCCESS":
        raise RpcError(f"funding {dest} failed: {r.get('engine_result')}")


def account_sequence(
    url: str, account: str, *, retries: int = 5, validated: bool = False
) -> int:
    # "current" briefly returns InsufficientNetworkMode when a node slips out of
    # sync mid-run; retry rather than abort. validated=True reads the
    # agreed-on-ledger sequence, needed where a stale/too-low "current" would
    # corrupt a later submission (e.g. seeding the pre-signed corpus).
    ledger = "validated" if validated else "current"
    last = None
    for _ in range(retries):
        try:
            r = rpc_checked(
                url, "account_info", {"account": account, "ledger_index": ledger}
            )
            return int(r["account_data"]["Sequence"])
        except (RpcError, OSError) as e:
            last = e
            time.sleep(2)
    raise RpcError(f"account_sequence({account}): {last}")


def wait_nodes_converged(
    urls: list[str], timeout_s: float = 90.0, max_lag: int = 1
) -> None:
    """Block until every node agrees on the validated ledger (within max_lag).

    After a large funding burst a node serving sequence reads can lag and hand
    back stale, too-low sequences that submit as tefPAST_SEQ; gating
    corpus-seeding reads on convergence avoids that.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        idxs = []
        for u in urls:
            try:
                idxs.append(current_ledger(u))
            except (RpcError, OSError):
                idxs.append(0)
        if idxs and min(idxs) > 0 and max(idxs) - min(idxs) <= max_lag:
            return
        time.sleep(1.0)
    # Best-effort: proceed even if not fully converged; the validated reads retry.


def job_backlog(url: str) -> int:
    """Approximate JobQueue backlog (queued minus started) from PerfLog counters.

    PerfLog renders counts as JSON strings to avoid precision loss, so values
    must be cast before arithmetic. Returns -1 if counters are unavailable.
    """
    try:
        r = rpc(url, "server_info", {"counters": True})
        jq = r.get("info", {}).get("counters", {}).get("job_queue", {})
        total = 0
        for v in jq.values():
            if isinstance(v, dict):
                total += max(0, int(v.get("queued", 0)) - int(v.get("started", 0)))
        return total
    except (RpcError, OSError, ValueError, TypeError):
        return -1


class LedgerPoller(threading.Thread):
    """Polls validated ledgers and times when each tracked hash is included.

    Also samples the JobQueue backlog so callers can tell whether throughput is
    inclusion- or verification-bound.
    """

    def __init__(self, url: str, poll_s: float = 0.5):
        super().__init__(daemon=True)
        self.url = url
        self.poll_s = poll_s
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.pending: dict[str, float] = {}
        self.done: dict[str, tuple[float, int]] = {}
        self.backlog_samples: list[tuple[float, int]] = []

    def track(self, tx_hash: str, submit_time: float) -> None:
        with self._lock:
            self.pending[tx_hash] = submit_time

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        last_ledger = 0
        while not self._stop.is_set():
            try:
                idx = current_ledger(self.url)
                if idx > last_ledger:
                    # Scan every ledger since the last one seen: skipping ledgers
                    # when the poller falls behind silently loses inclusions.
                    first = last_ledger + 1 if last_ledger else idx
                    for li in range(first, idx + 1):
                        self._scan_ledger(li)
                    last_ledger = idx
                self.backlog_samples.append((time.monotonic(), job_backlog(self.url)))
            except Exception:  # noqa: BLE001 - the poller must never die mid-run
                pass
            time.sleep(self.poll_s)

    def _scan_ledger(self, ledger_index: int) -> None:
        r = rpc(
            self.url,
            "ledger",
            {"ledger_index": ledger_index, "transactions": True, "expand": False},
        )
        txns = r.get("ledger", {}).get("transactions", []) or []
        now = time.monotonic()
        with self._lock:
            for h in txns:
                if h in self.pending and h not in self.done:
                    self.done[h] = (now, ledger_index)


# ---------------------------------------------------------------------------
# Steady-state methodology support (rerun after the RippleX review).
#
# Everything below records raw, timestamped data to files so that the scoring
# can be re-derived offline from what was captured. Times are wall-clock epoch
# seconds (time.time()) so driver processes, pollers and a generator running on
# another machine can be lined up after the fact.
# ---------------------------------------------------------------------------


class KeepAliveRpc:
    """JSON-RPC over one persistent HTTP/1.1 connection.

    urllib opens a fresh TCP connection per request, which caps a Python load
    generator well before the network under test does. One instance per thread
    per endpoint; not thread-safe.
    """

    def __init__(self, url: str, timeout: float = 15.0):
        u = urllib.parse.urlsplit(url)
        self.host = u.hostname or "127.0.0.1"
        self.port = u.port or 80
        self.path = u.path or "/"
        self.timeout = timeout
        self.conn: http.client.HTTPConnection | None = None
        self.reused = False

    def close(self) -> None:
        if self.conn is not None:
            try:
                self.conn.close()
            except OSError:
                pass
        self.conn = None
        self.reused = False

    def call(self, method: str, params: dict) -> dict:
        body = json.dumps({"method": method, "params": [params]}).encode()
        while True:
            fresh = self.conn is None
            if fresh:
                self.conn = http.client.HTTPConnection(
                    self.host, self.port, timeout=self.timeout
                )
            try:
                self.conn.request(
                    "POST",
                    self.path,
                    body,
                    {"Content-Type": "application/json", "Connection": "keep-alive"},
                )
                resp = self.conn.getresponse()
                data = resp.read()
                if resp.will_close:
                    self.close()
                else:
                    self.reused = True
                if resp.status != 200:
                    raise RpcError(f"{method}: HTTP {resp.status}")
                return json.loads(data).get("result", {})
            except (http.client.HTTPException, OSError):
                was_reused = not fresh
                self.close()
                # A server may drop an idle keep-alive connection; retry once on
                # a fresh one. Never retry a request sent on a fresh connection,
                # so a submit that reached the node is not sent twice.
                if not was_reused:
                    raise


class JsonlWriter:
    """Thread-safe append-only JSON-lines writer (gzip when path ends .gz)."""

    def __init__(self, path: str):
        self.path = path
        self._f = gzip.open(path, "at") if path.endswith(".gz") else open(path, "a")
        self._lock = threading.Lock()

    def write(self, obj: dict) -> None:
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        with self._lock:
            self._f.write(line)

    def close(self) -> None:
        with self._lock:
            self._f.close()


def backlog_from_counters(info: dict) -> tuple[int, dict]:
    """(total, per-job-type) JobQueue backlog from server_info counters."""
    jq = info.get("counters", {}).get("job_queue", {})
    per: dict = {}
    total = 0
    for name, v in jq.items():
        if not isinstance(v, dict):
            continue
        try:
            b = max(0, int(v.get("queued", 0)) - int(v.get("started", 0)))
        except (TypeError, ValueError):
            continue
        if b:
            per[name] = b
        total += b
    return total, per


class PeriodicSampler:
    """One thread per node; each calls sample(name, rpc) every period_s and
    writes the returned dict (plus t and node) to a JSON-lines file."""

    def __init__(self, nodes: dict[str, str], out_path: str, period_s: float = 1.0):
        self.nodes = nodes
        self.period_s = period_s
        self.out = JsonlWriter(out_path)
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self.errors = 0

    def sample(self, name: str, rpc_: KeepAliveRpc) -> dict | None:  # override
        raise NotImplementedError

    def _loop(self, name: str, url: str) -> None:
        r = KeepAliveRpc(url, timeout=max(5.0, self.period_s * 4))
        nxt = time.time()
        while not self._stop.is_set():
            t = time.time()
            try:
                row = self.sample(name, r)
                if row is not None:
                    self.out.write({"t": round(t, 3), "node": name, **row})
            except Exception:  # noqa: BLE001 - a sampler must never die mid-run
                self.errors += 1
                r.close()
            nxt += self.period_s
            self._stop.wait(max(0.0, nxt - time.time()))

    def start(self) -> None:
        for name, url in self.nodes.items():
            th = threading.Thread(target=self._loop, args=(name, url), daemon=True)
            th.start()
            self._threads.append(th)

    def stop(self) -> None:
        self._stop.set()
        for th in self._threads:
            th.join(timeout=10)
        self.out.close()


class ServerInfoSampler(PeriodicSampler):
    """Per-node health: consensus round time of the last closed ledger, job
    queue backlog, dropped-transaction counters and load factor. Pointed at the
    validators, this is what shows where the time went past the limit."""

    def sample(self, name, rpc_):
        info = rpc_.call("server_info", {"counters": True}).get("info", {})
        if not info:
            return None
        lc = info.get("last_close", {}) or {}
        vl = info.get("validated_ledger", {}) or {}
        backlog, per_job = backlog_from_counters(info)
        return {
            "server_state": info.get("server_state"),
            "validated_seq": vl.get("seq"),
            "converge_time_s": lc.get("converge_time_s"),
            "proposers": lc.get("proposers"),
            "load_factor": info.get("load_factor"),
            "jq_trans_overflow": _as_int(info.get("jq_trans_overflow")),
            "peer_disconnects": _as_int(info.get("peer_disconnects")),
            "peer_disconnects_resources": _as_int(
                info.get("peer_disconnects_resources")
            ),
            "io_latency_ms": info.get("io_latency_ms"),
            "peers": info.get("peers"),
            "job_backlog": backlog,
            "job_backlog_by_type": per_job,
        }


class FeeQueueSampler(PeriodicSampler):
    """Open-ledger and transaction-queue state from the `fee` command."""

    def sample(self, name, rpc_):
        r = rpc_.call("fee", {})
        if r.get("status") != "success":
            return None
        drops = r.get("drops", {}) or {}
        return {
            "ledger_current_index": r.get("ledger_current_index"),
            "expected_ledger_size": _as_int(r.get("expected_ledger_size")),
            "current_ledger_size": _as_int(r.get("current_ledger_size")),
            "current_queue_size": _as_int(r.get("current_queue_size")),
            "max_queue_size": _as_int(r.get("max_queue_size")),
            "open_ledger_fee": _as_int(drops.get("open_ledger_fee")),
            "minimum_fee": _as_int(drops.get("minimum_fee")),
            "median_fee": _as_int(drops.get("median_fee")),
            "base_fee": _as_int(drops.get("base_fee")),
        }


def _as_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


class ValidatedLedgerRecorder:
    """Records every validated ledger: when the network first reported it
    validated, how many transactions it carried, and their hashes.

    Header threads poll `ledger validated` on each of `header_urls` (normally
    the validators) every poll_s; a ledger's first_seen is the earliest time any
    of them reported it, which approximates when the network validated it to
    within poll_s. A detail thread fetches each ledger once, in order, from
    `detail_url`. Nothing is skipped when a poller falls behind.

    Writes <prefix>.ledgers.jsonl (one row per ledger) and
    <prefix>.ledger_txs.csv.gz (ledger_index,hash for every transaction).
    """

    def __init__(
        self,
        header_urls: list[str],
        detail_url: str,
        out_prefix: str,
        poll_s: float = 0.1,
    ):
        self.header_urls = header_urls
        self.detail_url = detail_url
        self.poll_s = poll_s
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self.first_seen: dict[int, float] = {}
        self.seen_by: dict[int, dict[str, float]] = {}
        self.coalesced: set[int] = set()
        self.details: dict[int, dict] = {}
        self._pending: list[int] = []
        self._highest = 0
        self.rows = JsonlWriter(out_prefix + ".ledgers.jsonl")
        self.txs = gzip.open(out_prefix + ".ledger_txs.csv.gz", "wt")
        self.txs.write("ledger_index,hash\n")
        self._threads: list[threading.Thread] = []
        self.errors = 0

    # -- threads --------------------------------------------------------------
    def _header_loop(self, url: str) -> None:
        r = KeepAliveRpc(url, timeout=5.0)
        last = 0
        while not self._stop.is_set():
            try:
                res = r.call("ledger", {"ledger_index": "validated"})
                idx = res.get("ledger_index") or res.get("ledger", {}).get(
                    "ledger_index"
                )
                idx = int(idx) if idx is not None else 0
            except Exception:  # noqa: BLE001
                self.errors += 1
                r.close()
                idx = 0
            now = time.time()
            if idx > last:
                first = last + 1 if last else idx
                with self._cv:
                    for li in range(first, idx + 1):
                        if li < idx:
                            self.coalesced.add(li)
                        self.seen_by.setdefault(li, {})[url] = now
                        if li not in self.first_seen or now < self.first_seen[li]:
                            self.first_seen[li] = now
                        if li > self._highest:
                            self._pending.append(li)
                            self._highest = li
                    self._cv.notify_all()
                last = idx
            self._stop.wait(self.poll_s)

    def _detail_loop(self) -> None:
        r = KeepAliveRpc(self.detail_url, timeout=30.0)
        while True:
            with self._cv:
                while not self._pending and not self._stop.is_set():
                    self._cv.wait(0.5)
                if not self._pending and self._stop.is_set():
                    return
                li = self._pending.pop(0)
            ledger = None
            for _ in range(20):
                try:
                    res = r.call(
                        "ledger",
                        {"ledger_index": li, "transactions": True, "expand": False},
                    )
                    ledger = res.get("ledger")
                    if ledger:
                        break
                except Exception:  # noqa: BLE001
                    self.errors += 1
                    r.close()
                time.sleep(0.25)
            hashes = (ledger or {}).get("transactions", []) or []
            with self._cv:
                row = {
                    "ledger_index": li,
                    "first_seen": round(self.first_seen.get(li, float("nan")), 3),
                    "seen_by": {k: round(v, 3) for k, v in self.seen_by.get(li, {}).items()},
                    "coalesced": li in self.coalesced,
                    "txn_count": len(hashes) if ledger else None,
                    "close_time": (ledger or {}).get("close_time"),
                    "close_time_resolution": (ledger or {}).get("close_time_resolution"),
                    "fetched": ledger is not None,
                }
                self.details[li] = row
                self._cv.notify_all()
            self.rows.write(row)
            for h in hashes:
                self.txs.write(f"{li},{h}\n")

    def start(self) -> None:
        for u in self.header_urls:
            th = threading.Thread(target=self._header_loop, args=(u,), daemon=True)
            th.start()
            self._threads.append(th)
        th = threading.Thread(target=self._detail_loop, daemon=True)
        th.start()
        self._threads.append(th)

    def stop(self) -> None:
        self._stop.set()
        for th in self._threads:
            th.join(timeout=120)
        self.rows.close()
        self.txs.close()

    # -- online queries -------------------------------------------------------
    def wait_details(self, upto_index: int, timeout_s: float = 60.0) -> bool:
        deadline = time.time() + timeout_s
        with self._cv:
            while True:
                missing = [
                    li for li in self.first_seen if li <= upto_index and li not in self.details
                ]
                if not missing:
                    return True
                left = deadline - time.time()
                if left <= 0:
                    return False
                self._cv.wait(min(left, 0.5))

    def window(self, t0: float, t1: float) -> dict:
        """Ledger-side view of [t0, t1]: validated-ledger intervals and the
        transactions the ledgers carried. The interval series starts at the
        last ledger validated before t0 and ends with the still-open interval
        at t1, so a stall at either edge of the window is not hidden."""
        with self._cv:
            items = sorted(self.first_seen.items())
        before = [t for _, t in items if t < t0]
        inside = [(li, t) for li, t in items if t0 <= t <= t1]
        if inside:
            self.wait_details(inside[-1][0])
        times = ([before[-1]] if before else []) + [t for _, t in inside]
        intervals = [b - a for a, b in zip(times, times[1:]) if b > a]
        open_gap = t1 - times[-1] if times else t1 - t0
        with self._cv:
            counts = [self.details.get(li, {}).get("txn_count") for li, _ in inside]
        known = [c for c in counts if c is not None]
        return {
            "ledgers": len(inside),
            "first_index": inside[0][0] if inside else None,
            "last_index": inside[-1][0] if inside else None,
            "intervals_s": [round(x, 3) for x in intervals],
            "open_gap_s": round(open_gap, 3),
            "max_interval_s": round(max(intervals + [open_gap]), 3),
            "txn_total": sum(known),
            "txn_per_ledger": known,
            "missing_details": len(counts) - len(known),
        }
