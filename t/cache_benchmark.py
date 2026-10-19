#!/usr/bin/env python3
"""Manual Linux/Actions experiment with real HTTP cache fills; see PERFORMANCE.md."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import http.client
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import sysconfig
import tempfile
import threading
import time

from cache_benchmark_report import write_report


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def progress(output, phase, trial=None):
    write_json(output / "progress.json", dict(phase=phase, trial=trial))
    print(f"{output.name} trial={trial or '-'} phase={phase}", flush=True)


def client_metadata():
    jit = getattr(sys, "_jit", None)
    return dict(implementation=platform.python_implementation(), version=sys.version,
        executable=sys.executable, clock=vars(time.get_clock_info("perf_counter")),
        free_threaded_build=sysconfig.get_config_var("Py_GIL_DISABLED"),
        gil_enabled=getattr(sys, "_is_gil_enabled", lambda: None)(),
        jit_available=getattr(jit, "is_available", lambda: None)(),
        jit_enabled=getattr(jit, "is_enabled", lambda: None)(),
        dont_write_bytecode=sys.dont_write_bytecode,
        stdout_write_through=getattr(sys.stdout, "write_through", None),
        environment={key: os.environ.get(key) for key in
            ("PYTHONDONTWRITEBYTECODE", "PYTHONUNBUFFERED", "PYTHON_GIL", "PYTHON_JIT")})


def request(port, uri, method="GET", headers=None, connection=None):
    own = connection is None
    connection = connection or http.client.HTTPConnection("127.0.0.1", port, timeout=300)
    try:
        connection.request(method, uri, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        if own:
            connection.close()


def cache_state(response):
    return next((v for k, v in response[1].items() if k.lower() == "x-cache"), "")


def wait_for(predicate, timeout, why):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError(why)


def children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [int(p) for p in path.read_text().split()] if path.exists() else []


def process_name(pid):
    try:
        return Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode()
    except FileNotFoundError:
        return ""


def resources(master):
    result = {}
    for pid in children(master):
        if "worker process" not in process_name(pid):
            continue
        root = Path(f"/proc/{pid}")
        values = {"fds": len(list((root / "fd").iterdir()))}
        for filename in ("smaps_rollup", "io"):
            for line in (root / filename).read_text().splitlines():
                match = re.match(r"(\w+):\s+(\d+)(?: kB)?$", line)
                if match:
                    values[match[1]] = int(match[2])
        stat = (root / "stat").read_text().rsplit(")", 1)[1].split()
        values["cpu_ticks"] = int(stat[11]) + int(stat[12])
        result[str(pid)] = values
    return result


class Server:
    def __init__(self, args, mode, root, output):
        self.root, self.output = root, output
        self.cache = root / "cache"
        self.cache.mkdir()
        (root / "logs").mkdir()
        self.process = None
        self.forced_exit = False
        binary = args.nginx_threads if mode == "generation-threads" else args.nginx_incremental
        reserved = [socket.socket(), socket.socket()]
        for listener in reserved:
            listener.bind(("127.0.0.1", 0))
        self.port, origin = [listener.getsockname()[1] for listener in reserved]
        generation = mode != "legacy-vary"
        namespace = (f"cache_purge_namespace objects 8m {root}/state;\n"
                     "cache_purge_key objects $uri;") if generation else ""
        key = "$cache_purge_key" if generation else "$uri"
        pool = "thread_pool cleanup threads=2 max_queue=16;" if mode == "generation-threads" else ""
        selection = "cache_purge_thread_pool cleanup;" if pool else ""
        levels = "" if args.levels == "flat" else f"levels={args.levels}"
        config = f"""
{'user root;' if os.geteuid() == 0 else ''}
daemon off;
worker_processes {args.workers};
pid {root}/nginx.pid;
error_log {output}/error.log notice;
{pool}
events {{ worker_connections 1024; }}
http {{
    access_log off;
    {namespace}
    {selection}
    client_body_temp_path {root}/client-temp;
    proxy_temp_path {root}/proxy-temp;
    proxy_cache_path {self.cache} {levels} keys_zone=bench:256m inactive=2h
        loader_files=1000 loader_threshold=200ms loader_sleep=1ms;
    cache_purge_background_queue on;
    cache_purge_vary_aware on;
    cache_purge_batch_size 10;
    cache_purge_throttle_ms 1ms;
    server {{
        listen 127.0.0.1:{origin};
        add_header Cache-Control max-age=3600;
        add_header ETag '"benchmark"';
        location = /cache/target {{
            add_header Cache-Control max-age=3600;
            add_header ETag '"benchmark-vary"';
            add_header Vary 'X-Test-Variant, Accept-Language';
            return 200 "$http_x_test_variant:$http_accept_language";
        }}
        location / {{ return 200 "unrelated object"; }}
    }}
    server {{
        listen 127.0.0.1:{self.port};
        location = /ping {{ return 200 "ready"; }}
        location = /control {{ return 200 "control"; }}
        location = /status {{ cache_purge_queue_status {self.cache}; }}
        location /cache/ {{
            proxy_pass http://127.0.0.1:{origin};
            proxy_cache bench;
            proxy_cache_key {key};
            proxy_cache_valid 200 1h;
            proxy_cache_purge PURGE from 127.0.0.1;
            add_header X-Cache $upstream_cache_status always;
        }}
    }}
}}
"""
        config_path = output / "nginx.conf"
        config_path.write_text(config)
        for listener in reserved:
            listener.close()
        self.log = (output / "process.log").open("w")
        try:
            self.process = subprocess.Popen([binary, "-p", str(root) + "/",
                "-c", str(config_path)], stdout=self.log, stderr=subprocess.STDOUT,
                start_new_session=True)
            def ready():
                if self.process.poll() is not None:
                    raise RuntimeError(f"NGINX exited; inspect {output}")
                try:
                    return request(self.port, "/ping")[2] == b"ready"
                except OSError:
                    return False
            wait_for(ready, 10, "NGINX did not start")
            wait_for(lambda: len(self.worker_pids()) == args.workers, 10,
                     "not all NGINX workers started")
            self.workers = self.worker_pids()
            self.check()
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.send_signal(signal.SIGQUIT)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.forced_exit = True
                os.killpg(self.process.pid, signal.SIGTERM)
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait()
        self.log.close()
        return self.process is not None and self.process.returncode == 0 and not self.forced_exit

    def worker_pids(self):
        return {pid for pid in children(self.process.pid)
                if "worker process" in process_name(pid)}

    def check_log(self):
        log = (self.output / "error.log").read_text()
        if re.search(r"\[(?:error|crit|alert|emerg)\]|exited on signal", log):
            raise RuntimeError(f"NGINX error or worker crash; inspect {self.output / 'error.log'}")

    def check(self):
        if self.process.poll() is not None or self.worker_pids() != self.workers:
            raise RuntimeError("NGINX master exited or its worker set changed")
        self.check_log()

    def status(self):
        response = request(self.port, "/status")
        if response[0] != 200:
            raise RuntimeError("status request failed")
        return json.loads(response[2])


class Sampler(threading.Thread):
    """Closed-loop responsiveness probe, not an offered-load latency benchmark."""
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port, self.rows = port, []
        self.stop_event = threading.Event()

    def run(self):
        number = 0
        while not self.stop_event.is_set():
            kind = "hit" if number % 2 else "ping"
            start = time.perf_counter()
            try:
                response = request(self.port, "/cache/data/0" if kind == "hit" else "/ping")
                ok = response[0] == 200 and (kind != "hit" or cache_state(response) == "HIT")
            except (OSError, http.client.HTTPException):
                ok = False
            end = time.perf_counter()
            self.rows.append(dict(kind=kind, start=start, end=end,
                                  ms=(end - start) * 1000, ok=ok))
            number += 1
            self.stop_event.wait(0.005)

    def close(self):
        self.stop_event.set()
        self.join(timeout=305)
        if self.is_alive():
            raise RuntimeError("responsiveness probe did not terminate")


def distribution(rows):
    values = sorted(row["ms"] for row in rows)
    def quantile(fraction):
        return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None
    return dict(n=len(values), errors=sum(not row["ok"] for row in rows),
                p50_ms=quantile(.5), p95_ms=quantile(.95),
                p99_ms=quantile(.99) if len(values) >= 100 else None,
                max_ms=max(values) if values else None)


def representations(count):
    return [{"X-Test-Variant": str(i // 2), "Accept-Language": "en" if i % 2 else "fr"}
            for i in range(count)]


def warm_targets(server, headers, expect_miss=False):
    for values in headers:
        response = request(server.port, "/cache/target", headers=values)
        expected = f"{values['X-Test-Variant']}:{values['Accept-Language']}".encode()
        if response[0] != 200 or response[2] != expected:
            raise RuntimeError("target representation/body mismatch")
        if expect_miss and cache_state(response) != "MISS":
            raise RuntimeError("old representation remained reachable after PURGE")
        wait_for(lambda: cache_state(request(server.port, "/cache/target", headers=values)) == "HIT",
                 5, "target did not become a HIT")


def manifest(server, expected):
    # Validation intentionally preconditions file headers/metadata before each
    # measured purge. These are warm-after-validation trials, never cold trials.
    targets, count = [], 0
    for root, _, files in os.walk(server.cache):
        for name in files:
            if re.fullmatch("[0-9a-f]{32}", name):
                path = Path(root) / name
                with path.open("rb") as entry:
                    header = entry.read(4096)
                if b"/cache/target\n" in header:
                    targets.append(path)
                count += 1
    if count != expected:
        raise RuntimeError(f"cache population mismatch: {count} != {expected}")
    return targets, count


def client_control(server):
    # Same client, method and connection policy as PURGE, without cache work.
    # This is an HTTP round-trip floor, not the interpreter's isolated cost.
    samples = []
    for _ in range(20):
        start = time.perf_counter()
        response = request(server.port, "/control", "PURGE")
        end = time.perf_counter()
        if response[0] != 200 or response[2] != b"control":
            raise RuntimeError("HTTP client control failed")
        samples.append((end - start) * 1000)
    return samples


def run_case(args, mode, output):
    output.mkdir()
    root = Path(tempfile.mkdtemp(prefix=mode + "-", dir=args.work))
    server = None
    trials = []
    success = False
    try:
        progress(output, "startup")
        server = Server(args, mode, root, output)
        setup = time.perf_counter()
        progress(output, "populate")
        def seed(shard):
            connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=30)
            try:
                for index in range(shard, args.files, 8):
                    if request(server.port, f"/cache/data/{index}", connection=connection)[0] != 200:
                        raise RuntimeError("HTTP population failed")
            finally:
                connection.close()
        # Eight bounded producers; never enqueue one million Python futures.
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(seed, range(8)))
        wait_for(lambda: cache_state(request(server.port, "/cache/data/0")) == "HIT",
                 5, "unrelated sentinel did not warm")
        headers = representations(args.variants)
        warm_targets(server, headers)
        # Core starts its loader with a 60-second delay. Await its real exit,
        # outside timing, so it cannot overlap the scan experiment.
        progress(output, "wait-loader")
        wait_for(lambda: not any("cache loader process" in process_name(pid)
                                for pid in children(server.process.pid)),
                 600, "cache loader did not exit")
        setup_seconds = time.perf_counter() - setup
        for number in range(args.trials):
            progress(output, "validate-files", number + 1)
            validation = time.perf_counter()
            targets, count = manifest(server, args.files + args.variants)
            validation_seconds = time.perf_counter() - validation
            if len(targets) != args.variants:
                raise RuntimeError("native Vary population does not match requested cardinality")
            server.check()
            progress(output, "client-control", number + 1)
            control_ms = client_control(server)
            sampler = Sampler(server.port)
            progress(output, "baseline", number + 1)
            sampler.start()
            try:
                time.sleep(2)  # baseline outside the measured purge
                before = resources(server.process.pid)
                progress(output, "purge-and-drain", number + 1)
                start = time.perf_counter()
                response = request(server.port, "/cache/target", "PURGE")
                ack = time.perf_counter()
                if response[0] != 200:
                    raise RuntimeError(f"PURGE failed: {response[0]} {response[2]!r}")
                gone = None
                deadline = start + 300
                while True:
                    now = time.perf_counter()
                    if gone is None and not any(path.exists() for path in targets):
                        gone = now
                    state = server.status()["cache"]
                    if gone is not None and not state["active"] and not state["pending"]["total"]:
                        break
                    if now >= deadline:
                        raise RuntimeError("cleanup did not finish in 300 seconds")
                    time.sleep(.02)
                drained = time.perf_counter()
            finally:
                sampler.close()
                with (output / f"probes-{number + 1}.csv").open("w") as stream:
                    writer = csv.DictWriter(stream, fieldnames=["kind", "start", "end", "ms", "ok"])
                    writer.writeheader()
                    writer.writerows(sampler.rows)
            if state["files"]["errors"] or state["cleanup"]["scan_errors"]:
                raise RuntimeError("cleanup reported file/scan errors")
            row = dict(mode=mode, trial=number + 1, files=count, variants=args.variants,
                       client_control_ms=control_ms, validation_seconds=validation_seconds,
                       ack_ms=(ack - start) * 1000,
                       files_gone_observed_ms=(gone - start) * 1000,
                       drained_observed_ms=(drained - start) * 1000,
                       start=start, ack=ack, drained=drained,
                       resources_before=before, resources_after=resources(server.process.pid))
            row["probes"] = {}
            for phase, lower, upper in [("baseline", 0, start), ("ack", start, ack),
                                        ("cleanup", ack, drained)]:
                row["probes"][phase] = {kind: distribution([r for r in sampler.rows
                    if r["kind"] == kind and r["start"] < upper and r["end"] > lower])
                    for kind in ("ping", "hit")}
            if any(not r["ok"] for r in sampler.rows):
                raise RuntimeError("ordinary request failed or unrelated HIT was lost")
            # Refill only after the old filenames have disappeared: legacy
            # replacements reuse those names, so earlier refills would make
            # the physical-removal observation ambiguous.
            progress(output, "verify-refill", number + 1)
            refill = time.perf_counter()
            warm_targets(server, headers, expect_miss=True)
            server.check()
            row["refill_seconds"] = time.perf_counter() - refill
            trials.append(row)
            write_json(output / "results.json", dict(setup_seconds=setup_seconds, trials=trials))
            print(f"{mode} trial={number + 1} ack={row['ack_ms']:.3f}ms "
                  f"drained={row['drained_observed_ms']:.3f}ms", flush=True)
        progress(output, "teardown")
        success = True
        return trials
    except Exception as error:
        write_json(output / "failure.json", dict(error=str(error), verified_trials=len(trials)))
        raise
    finally:
        clean = True
        try:
            if server is not None:
                clean = server.close()
        finally:
            shutil.rmtree(root)
        if success and server is not None:
            server.check_log()
            if not clean:
                raise RuntimeError("NGINX did not exit cleanly after the experiment")
            progress(output, "complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nginx-incremental", default="/opt/benchmark/incremental/nginx")
    parser.add_argument("--nginx-threads", default="/opt/benchmark/threads/nginx")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--files", type=int, choices=[1000, 10000, 100000, 1000000], default=10000)
    parser.add_argument("--variants", type=int, choices=[2, 16, 66], default=16)
    parser.add_argument("--trials", type=int, choices=[5, 10], default=5)
    parser.add_argument("--workers", type=int, choices=[1, 4], default=1)
    parser.add_argument("--levels", choices=["flat", "1:2", "2:2"], default="1:2")
    parser.add_argument("--mode", choices=["compare", "legacy-vary", "generation", "generation-threads"], default="compare")
    args = parser.parse_args()
    args.work, args.output = args.work.resolve(), args.output.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    disk = os.statvfs(args.work)
    if disk.f_bavail * disk.f_frsize < args.files * 8192 + 268435456 or disk.f_favail < args.files + 10000:
        raise RuntimeError("insufficient free bytes/inodes for this preset; select fewer files")
    environment = dict(revision=os.environ.get("GITHUB_SHA"), platform=platform.platform(),
        cpu_count=os.cpu_count(), cpu=Path("/proc/cpuinfo").read_text(),
        client=client_metadata(),
        nginx={kind: subprocess.run([binary, "-V"], capture_output=True, text=True, check=True).stderr
               for kind, binary in (("incremental", args.nginx_incremental), ("threads", args.nginx_threads))},
        options={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        cache_condition="warm after native-file validation; no page-cache dropping",
        filesystem=subprocess.check_output(["stat", "-f", "-c", "%T", str(args.work)], text=True).strip(),
        available_bytes=disk.f_bavail * disk.f_frsize, available_inodes=disk.f_favail,
        probe_model="closed-loop, alternating fresh TCP ping/HIT requests, 5ms pause",
        cpu_tick_hz=os.sysconf("SC_CLK_TCK"))
    write_json(args.output / "environment.json", environment)
    modes = ["legacy-vary", "generation", "generation-threads"] if args.mode == "compare" else [args.mode]
    for mode in modes:
        run_case(args, mode, args.output / mode)
    write_report(args.output, "success")


if __name__ == "__main__":
    main()
