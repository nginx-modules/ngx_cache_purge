#!/usr/bin/env python3
"""Black-box queue/path regressions against an actual nginx binary."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import errno
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


class Instance:
    def __init__(self, nginx, base, workers, background=True, throttle="10ms", capacity=100):
        self.base, self.workers = base, workers
        self.background, self.throttle, self.capacity = background, throttle, capacity
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.cache = base / "proxy_temp" / "cache-a"
        self.cache_b = base / "cache-b"
        self.temp = self.cache / "custom-spool"
        base.mkdir(parents=True)
        (base / "origin").mkdir()
        (base / "logs").mkdir()
        self.cache.mkdir(parents=True)
        self.cache_b.mkdir(parents=True)
        for name in ("warm", "item"):
            (base / "origin" / name).write_text("cache fixture " + name)
        (base / "health").write_text("ready")
        self.config = base / "nginx.conf"
        self.write_config()
        self.log = (base / "process.log").open("wb")
        self.process = subprocess.Popen([nginx, "-p", str(base) + "/", "-c", str(self.config)],
                                        stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError((base / "process.log").read_text())
            try:
                if self.request("/health")[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(0.01)
        self.close()
        raise AssertionError("nginx failed to start")

    def write_config(self):
        user = "user root;" if os.geteuid() == 0 else ""
        self.config.write_text(f'''
{user}
daemon off;
worker_processes {self.workers};
pid {self.base}/nginx.pid;
error_log {self.base}/error.log notice;
events {{ worker_connections 1024; }}
http {{
    access_log off;
    client_body_temp_path {self.base}/client-temp;
    proxy_temp_path {self.temp};
    proxy_cache_path {self.cache} levels=1:2 keys_zone=a:2m inactive=1h;
    proxy_cache_path {self.cache_b} levels=1:2 keys_zone=b:2m inactive=1h;
    cache_purge_background_queue {"on" if self.background else "off"};
    cache_purge_queue_size {self.capacity};
    cache_purge_throttle_ms {self.throttle};
    cache_purge_vary_aware on;
    cache_purge_response_type json;
    server {{
        listen 127.0.0.1:{self.port};
        root {self.base};
        proxy_cache_purge PURGE from 127.0.0.1;
        location /health {{ proxy_cache_purge off; }}
        location /origin/ {{
            proxy_cache_purge off;
            add_header Vary Accept-Encoding always;
        }}
        location /cache/ {{
            proxy_pass http://127.0.0.1:{self.port}/origin/;
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_valid 200 1h;
            add_header X-Cache $upstream_cache_status always;
        }}
        location /b/ {{
            proxy_cache b;
            proxy_cache_key $uri;
        }}
        location /all {{
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }}
        location /denied {{
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE from 10.255.255.1;
        }}
        location /status-a {{
            cache_purge_queue_status {self.cache}///;
            add_header X-Queue-Config {"on" if self.background else "off"} always;
            location /status-a/child {{ }}
            location /status-a/purge {{
                proxy_cache a;
                proxy_cache_key $uri;
                proxy_cache_purge PURGE from 127.0.0.1;
            }}
        }}
        location /status-b {{ cache_purge_queue_status {self.cache_b}; }}
    }}
}}
''')

    def request(self, uri, method="GET", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            connection.request(method, uri, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def status(self, uri="/status-a"):
        code, headers, body = self.request(uri)
        assert code == 200, body
        assert headers.get("Cache-Control") == "no-store"
        assert headers.get("Content-Type") == "application/json"
        assert int(headers["Content-Length"]) == len(body)
        state = json.loads(body)
        assert set(state) == set(STATUS_COUNTERS) | set(STATUS_FLAGS), state
        for field in STATUS_COUNTERS:
            assert isinstance(state[field], int) and state[field] >= 0, state
        for field in STATUS_FLAGS:
            assert isinstance(state[field], bool), state
        assert not state["purge_all_pending"] or state["queued"] > 0
        assert state["queued"] <= state["queued_total"] <= state["capacity"], state
        assert state["full"] == (state["capacity"] != 0
                                 and state["queued_total"] >= state["capacity"]), state
        assert state["queued_total"] != 0 or state["oldest_ms"] == 0, state
        return state

    def drain(self):
        deadline = time.monotonic() + 30
        active = False
        while time.monotonic() < deadline:
            state = self.status()
            active |= state["active"]
            if state["queued"] == 0 and not state["active"]:
                return active
            time.sleep(0.005)
        raise AssertionError("queue did not drain")

    def warm(self):
        assert self.request("/cache/warm")[0] == 200
        for _ in range(20):
            if self.request("/cache/warm")[1].get("X-Cache") == "HIT":
                break
            time.sleep(0.01)
        else:
            raise AssertionError("cache warmup did not produce a HIT")
        path = next(p for p in self.cache.rglob("*") if p.is_file() and len(p.name) == 32)
        data = path.read_bytes()
        return data[:data.index(b"\nKEY: ") + len(b"\nKEY: ")]

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGQUIT)
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                log = (self.base / "error.log").read_text()
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=5)
                raise AssertionError("graceful shutdown stalled: " + log[-2000:])
        self.log.close()
        log = (self.base / "error.log").read_text()
        assert self.process.returncode == 0, log
        assert "exited on signal" not in log, log
        report = (self.base / "process.log").read_text()
        assert "AddressSanitizer" not in report, report
        assert "runtime error" not in report, report
        assert "runtime error" not in log, log


STATUS_COUNTERS = ("queued", "capacity", "queued_total", "oldest_ms",
                   "rejected_full", "files_deleted", "protected_skipped")
STATUS_FLAGS = ("purge_all_pending", "full", "active")


def fixture(root, key, prefix, parent=None):
    digest = hashlib.md5(key.encode()).hexdigest()
    folder = parent if parent is not None else root / digest[-1] / digest[-3:-1]
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / digest
    path.write_bytes(prefix + key.encode() + b"\nbody\n")
    return path


def regression(nginx, base, workers, files):
    instance = Instance(nginx, base, workers)
    try:
        assert instance.status()["queued"] == 0
        assert instance.request("/status-a", "POST")[0] == 405
        assert instance.request("/status-a/purge", "PURGE")[0] == 412
        assert instance.request("/denied/x", "PURGE")[0] == 403
        long_key = "/cache/" + "x" * 600 + "*"
        assert instance.request(long_key, "PURGE")[0] == 412
        code, headers, body = instance.request("/status-a", "HEAD")
        assert code == 200 and not body and int(headers["Content-Length"]) > 0
        prefix = instance.warm()
        sentinels = []
        sentinels_dirs = ("proxy_temp", "client_temp", "client_body_temp", "fastcgi_temp",
                          "scgi_temp", "uwsgi_temp", "custom-spool")
        for directory in ("proxy_temp", "client_temp", "client_body_temp", "fastcgi_temp",
                          "scgi_temp", "uwsgi_temp", "custom-spool"):
            sentinels.append(fixture(instance.cache, "/cache/batch0/protected-" + directory,
                                     prefix, instance.cache / directory / "nested"))
        for name in ("0000000123", "a" * 32 + ".0000000123"):
            path = instance.cache / name
            path.write_bytes(b"in-flight response")
            sentinels.append(path)
        snapshots = {p: p.read_bytes() for p in sentinels}
        ordinary = fixture(instance.cache, "/cache/plain", prefix,
                           instance.cache / "main" / "app_proxy_temp_file")
        assert instance.request("/all", "PURGE")[0] == 202
        instance.drain()
        assert not ordinary.exists()
        targets = [fixture(instance.cache, f"/cache/batch{i % 4}/entry{i}", prefix) for i in range(files)]
        case = fixture(instance.cache, "/cache/BATCH0/retain", prefix)
        with ThreadPoolExecutor(max_workers=4) as executor:
            responses = list(executor.map(lambda i: instance.request(f"/cache/batch{i}*", "PURGE"), range(4)))
        assert all(response[0] == 202 for response in responses)
        active = instance.drain()
        assert not any(p.exists() for p in targets)
        state = instance.status()
        assert state["files_deleted"] >= len(targets), state
        assert state["protected_skipped"] >= len(sentinels_dirs), state
        assert state["rejected_full"] == 0, state
        assert not case.exists(), "wildcard matching is case-insensitive, as before"
        for path, content in snapshots.items():
            assert path.read_bytes() == content
        for _ in range(100):
            instance.status()
        for encoding in ("identity", "gzip"):
            assert instance.request("/cache/item", headers={"Accept-Encoding": encoding})[0] == 200
            for _ in range(50):
                # nginx commits the cache file after the response is sent
                if instance.request("/cache/item", headers={"Accept-Encoding": encoding})[1].get("X-Cache") == "HIT":
                    break
                time.sleep(0.02)
            else:
                raise AssertionError("response was not cached")
        assert instance.request("/cache/item", "PURGE")[0] == 200
        for encoding in ("identity", "gzip"):
            assert instance.request("/cache/item", headers={"Accept-Encoding": encoding})[1].get("X-Cache") == "MISS"
        log = (instance.base / "error.log").read_text()
        assert "purge denied, client address is not in the \"from\" list" in log
        assert log.count("exceeds the 511 byte limit") == 1, "over-long key must warn once"
        assert "key too long" not in log
        # Files removed behind nginx's back leave their shm nodes, so the cache
        # manager later logs a "stat() failed" line for each; that is nginx
        # core, not a purge failure.  Everything else at crit or above is.
        severe = [line for line in log.splitlines()
                  if any(level in line for level in ("[crit]", "[alert]", "[emerg]"))
                  and "stat() " not in line]
        assert not severe, severe
        print("queue, temp paths and Vary regressions passed; active observed:", active)
    finally:
        instance.close()


def reload_and_sync(nginx, base, workers):
    instance = Instance(nginx, base / "reload", workers, throttle="1s", capacity=1)
    try:
        prefix = instance.warm()
        protected = fixture(instance.cache, "/cache/reload/protected", prefix, instance.temp / "nested")
        ordinary = fixture(instance.cache, "/cache/reload/delete", prefix)
        assert instance.request("/cache/reload*", "PURGE")[0] == 202
        assert instance.request("/cache/reload*", "PURGE")[0] == 202
        state = instance.status()
        assert state["queued"] == 1 and state["full"]
        assert instance.status("/status-a/child")["queued"] == 1
        assert instance.status("/status-b")["queued"] == 0
        assert instance.status("/status-b")["full"]
        assert instance.request("/cache/missing*", "PURGE")[0] == 412
        assert instance.status()["rejected_full"] == 1
        log = (instance.base / "error.log").read_text()
        assert "queue full (1/1 tasks)" in log and "(1 rejected so far)" in log, log
        assert instance.status("/status-b")["rejected_full"] == 0
        assert instance.status("/status-b")["queued_total"] == 1
        instance.capacity = 100000
        instance.write_config()
        instance.process.send_signal(signal.SIGHUP)
        deadline = time.monotonic() + 5
        while "changes the shared zone size" not in (instance.base / "error.log").read_text():
            assert time.monotonic() < deadline, "unsafe zone resize was not rejected"
            time.sleep(0.01)
        instance.capacity = 1
        instance.temp = instance.cache / "new-custom-spool"
        instance.write_config()
        instance.process.send_signal(signal.SIGHUP)
        instance.drain()
        assert protected.exists() and not ordinary.exists()
        assert instance.request("/all", "PURGE")[0] == 202
        state = instance.status()
        # A worker may dequeue between the enqueue response and this snapshot.
        assert state["purge_all_pending"] == (state["queued"] != 0)
        instance.drain()
        print("full queue, duplicate, resize rejection and reload regressions passed")
    finally:
        instance.close()
    instance = Instance(nginx, base / "disable", workers, throttle="1s")
    try:
        prefix = instance.warm()
        normal = fixture(instance.cache, "/cache/disable/delete", prefix)
        assert instance.request("/cache/disable*", "PURGE")[0] == 202
        instance.background = False
        instance.write_config()
        instance.process.send_signal(signal.SIGHUP)
        deadline = time.monotonic() + 10
        while instance.request("/status-a")[1].get("X-Queue-Config") != "off":
            assert time.monotonic() < deadline, "new configuration was not installed"
            time.sleep(0.01)
        instance.drain()
        assert not normal.exists(), "disabling enqueueing must drain accepted work"
        print("background-disable reload retains the queue")
    finally:
        instance.close()
    instance = Instance(nginx, base / "sync", workers, background=False)
    try:
        assert instance.status() == {
            "queued": 0, "capacity": 0, "queued_total": 0, "oldest_ms": 0,
            "purge_all_pending": False, "full": False, "active": False,
            "rejected_full": 0, "files_deleted": 0, "protected_skipped": 0}
        prefix = instance.warm()
        protected = fixture(instance.cache, "/cache/sync/protected", prefix, instance.temp / "nested")
        normal = fixture(instance.cache, "/cache/sync/delete", prefix)
        assert instance.request("/cache/sync*", "PURGE")[0] == 200
        assert protected.exists() and not normal.exists()
        print("synchronous delete-time guards passed")
    finally:
        instance.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nginx", required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--files", type=int, default=4096)
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory(prefix="purge-safety-")
    try:
        base = Path(temporary.name)
        nginx = str(Path(args.nginx).resolve())
        regression(nginx, base / "regression", args.workers, args.files)
        reload_and_sync(nginx, base, args.workers)
    finally:
        # Some mounted filesystems expose removals with a short delay.
        # Retry only ENOTEMPTY and retain a finite cleanup deadline.
        for attempt in range(40):
            try:
                temporary.cleanup()
                break
            except OSError as error:
                if error.errno != errno.ENOTEMPTY or attempt == 39:
                    raise
                time.sleep(0.05)


if __name__ == "__main__":
    main()
