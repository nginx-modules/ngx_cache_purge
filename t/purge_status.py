#!/usr/bin/env python3
"""Real-worker regressions: python3 t/purge_status.py /path/to/nginx."""

import concurrent.futures
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import pwd
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = (self.path + self.headers.get("Accept-Language", "en")).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Vary", "Accept-Language")
        # Avoid the core cache writer's zero-length NULL ETag copy under UBSan.
        self.send_header("ETag", '"' + hashlib.sha256(body).hexdigest() + '"')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="purge-vv-", dir=Path.cwd()) as td:
        base = Path(td)
        a = base / "proxy_temp" / "cache"
        b = base / "app_proxy_temp_file"
        for directory in [a, b, base / "logs", a / "staging"]:
            directory.mkdir(parents=True, exist_ok=True)
        (base / "generation").write_text("ready")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]

        origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
        thread = threading.Thread(target=origin.serve_forever, daemon=True)
        thread.start()
        origin_port = origin.server_port
        username = pwd.getpwuid(os.geteuid()).pw_name
        workers = int(os.environ.get("PURGE_TEST_WORKERS", "4"))
        details = subprocess.check_output([binary, "-V"], stderr=subprocess.STDOUT)
        version = next(line for line in details.splitlines()
                       if line.startswith(b"nginx version:"))
        legacy = b"nginx/1.7." in version
        no_rewrite = b"--without-http_rewrite_module" in details

        def config(enabled=True, throttle="60s", capacity=64, generation=1):
            conditional = "" if no_rewrite else "if ($arg_check) { set $flag 1; }"
            use_temp = "" if legacy else "use_temp_path=off"
            user = f"user {username};" if os.geteuid() == 0 else ""
            return f"""
{user}
worker_processes {workers};
pid logs/nginx.pid;
error_log logs/error.log notice;
events {{ worker_connections 256; }}
http {{
    access_log off;
    proxy_cache_path {a} levels=1:2 keys_zone=a:1m {use_temp};
    proxy_cache_path {b} levels=1:2 keys_zone=b:1m;
    proxy_temp_path {a}/staging;
    cache_purge_background_queue {'on' if enabled else 'off'};
    cache_purge_queue_size {capacity};
    cache_purge_throttle_ms {throttle};
    cache_purge_vary_aware on;
    server {{
        listen 127.0.0.1:{port};
        root {base};
        add_header X-Generation {generation} always;
        location = /status {{ cache_purge_status {a}; {conditional} }}
        location = /status-b {{ cache_purge_status {b}; }}
        location /all/ {{
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache a;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }}
        location = /all/one {{
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache a;
            proxy_cache_key /a/unused-0*;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }}
        location /a/ {{
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache a;
            proxy_cache_valid 200 1h;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE from 127.0.0.1;
        }}
        location /b/ {{
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache b;
            proxy_cache_valid 200 1h;
            proxy_cache_key $uri;
            proxy_cache_purge PURGE from 127.0.0.1;
        }}
    }}
}}
"""

        conf = base / "nginx.conf"
        conf.write_text(config())
        command = [binary, "-p", str(base) + "/", "-c", str(conf)]
        process = subprocess.Popen(command + ["-g", "daemon off;"],
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE,
                                   start_new_session=True)

        def request(path, method="GET", headers=None):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            try:
                conn.request(method, path, headers=headers or {})
                response = conn.getresponse()
                return response.status, dict(response.getheaders()), response.read()
            finally:
                conn.close()

        def until(predicate, label, seconds=30):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                try:
                    if predicate():
                        return
                except (ConnectionError, OSError):
                    pass
                check(process.poll() is None, "nginx exited: " + label)
                time.sleep(0.005)
            raise AssertionError("timeout: " + label)

        def status(path="/status"):
            code, _, body = request(path)
            check(code == 200, f"status HTTP {code}: {body!r}")
            return json.loads(body)

        def reload_config(text, generation):
            conf.write_text(text)
            result = subprocess.run(command + ["-t"], capture_output=True)
            check(result.returncode == 0, result.stderr.decode())
            process.send_signal(signal.SIGHUP)
            until(lambda: request("/generation")[1].get("X-Generation")
                  == str(generation), "new worker generation")

        try:
            until(lambda: request("/generation")[0] == 200, "startup")
            code, headers, body = request("/status")
            check(code == 200 and json.loads(body)["queue_size"] == 0,
                  "initial queue")
            check(headers.get("Cache-Control") == "no-store", "no-store")
            check(int(headers["Content-Length"]) == len(body), "JSON length")
            check(request("/status", "HEAD")[2] == b"", "HEAD body")
            check(request("/status", "POST")[0] == 405, "method restriction")
            if not no_rewrite:
                check(status("/status?check=1")["queue_size"] == 0,
                      "anonymous if location inheritance")

            for language in ["en", "fr"]:
                check(request("/a/vary", headers={"Accept-Language": language})[0]
                      == 200, "cache fill")
            check(request("/b/item")[0] == 200, "second cache fill")

            protected = [a / "proxy_temp" / ("a" * 32),
                         a / "client_body_temp" / ("b" * 32),
                         a / "staging" / "0000000001",
                         a / (("c" * 32) + ".0000000001")]
            removable = [a / "app_proxy_temp_file" / ("d" * 32),
                         a / "x\\proxy_temp" / ("e" * 32)]
            for path in protected + removable:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"sentinel")
            for n in range(int(os.environ.get("PURGE_TEST_FILES", "4096"))):
                name = hashlib.md5(str(n).encode()).hexdigest()
                path = a / name[-1] / name[-3:-1] / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")

            tasks = ["/all/one", "/all/two", "/all/three", "/b/*"]
            tasks += [f"/a/unused-{n}*" for n in range(60)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(lambda p: request(p, "PURGE")[0], tasks))
            check(all(code == 202 for code in results), f"enqueue: {results}")
            snapshot = status()
            check(snapshot == {"queue_size": 63, "purge_all_pending": True,
                               "queue_full": True, "purge_in_flight": False},
                  f"full queue snapshot: {snapshot}")
            check(status("/status-b")["queue_size"] == 1, "path isolation")
            check(request("/all/one", "PURGE")[0] == 202,
                  "duplicate accepted while full")
            check(status()["queue_size"] == 63, "duplicate not counted twice")
            check(request("/a/overflow*", "PURGE")[0] == 412,
                  "full queue uses synchronous fallback")

            conf.write_text(config(capacity=128, generation=99))
            process.send_signal(signal.SIGHUP)
            until(lambda: "resizing requires" in
                  (base / "logs/error.log").read_text(), "zone resize rejected")
            check(request("/generation")[1].get("X-Generation") == "1",
                  "failed reload preserved old workers")
            check(status()["queue_size"] == 63, "failed reload preserved queue")
            conf.write_text(config().replace(f"cache_purge_status {b};",
                                             f"cache_purge_status {base}/typo;"))
            result = subprocess.run(command + ["-t"], capture_output=True)
            check(result.returncode != 0 and b"unknown cache path" in result.stderr,
                  "unknown status path rejected")

            # Disable enqueueing; new workers must still drain accepted work.
            reload_config(config(enabled=False, throttle="1ms", generation=2), 2)
            samples = []

            def drained():
                snap = status()
                samples.append(snap)
                if snap["queue_size"] or snap["purge_in_flight"]:
                    return False
                check(all(not p.exists() for p in removable),
                      "idle reported before deletion finished")
                return status("/status-b")["queue_size"] == 0

            until(drained, "drain after disabling enqueueing")
            check(any(s["purge_in_flight"] for s in samples),
                  "overlapping walks were observed")
            check(all(p.exists() for p in protected), "protected files survived")
            leaves = [p for p in a.rglob("*") if p.is_file() and p not in protected]
            check(not leaves, f"purge left cache entries: {leaves[:3]}")
            check(not any(p.is_file() for p in b.rglob("*")), "partial purge")

            # Exact Vary purge still uses the same file safety guard.
            for language in ["en", "fr"]:
                request("/a/vary", headers={"Accept-Language": language})
            code, _, body = request("/a/vary", "PURGE",
                                    headers={"Accept-Language": "en"})
            check(code == 200, f"exact Vary purge: HTTP {code}, {body!r}")
            leaves = [p for p in a.rglob("*") if p.is_file() and p not in protected]
            check(not leaves, "Vary variants removed")
            check(all(p.exists() for p in protected), "Vary temp protection")

            reload_config(config(throttle="1ms", generation=3), 3)

            def reenabled():
                code, headers, _ = request("/a/again*", "PURGE")
                if headers.get("X-Generation") != "3":
                    return False
                check(code == 202, "re-enable queue")
                return True

            until(reenabled, "purge handled by the re-enabled generation")
            until(lambda: status()["queue_size"] == 0
                  and not status()["purge_in_flight"], "re-enabled queue")
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGQUIT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            stderr = process.stderr.read().decode()
            process.stderr.close()
            origin.shutdown()
            origin.server_close()
            error_log = (base / "logs/error.log").read_text()
            if sys.exc_info()[0] or process.returncode != 0:
                print(stderr, file=sys.stderr)
                print(error_log[-4000:], file=sys.stderr)
            if not sys.exc_info()[0]:
                check(process.returncode == 0, "unclean nginx shutdown")
                diagnostics = stderr + error_log
                for marker in ["ERROR: AddressSanitizer", "runtime error:",
                               "exited on signal"]:
                    offset = diagnostics.find(marker)
                    check(offset == -1,
                          diagnostics[max(0, offset - 160):offset + 4000])

        print(version.decode().strip() + ": status, boundaries, overlap, "
              "saturation, Vary, and reload regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
