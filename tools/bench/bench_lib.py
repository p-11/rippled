"""Shared helpers for the hybrid-PQ benchmark harness.

Signs the corpus via the node's `sign` RPC rather than a client library so the
same path produces both ECC-only and hybrid (pq_seed_hex) blobs without needing
client-side support for the custom quantum fields.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.request

GENESIS_ACCOUNT = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
GENESIS_SECRET = "masterpassphrase"
ASF_QUANTUM = 18  # AccountSet flag from include/xrpl/protocol/TxFlags.h


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
                    for li in range(max(last_ledger + 1, idx - 4), idx + 1):
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
