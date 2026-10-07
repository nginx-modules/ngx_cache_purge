#!/usr/bin/env python3
"""Configure a supplied nginx source tree; verify the actual addon object."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def run(command, **kwargs):
    print("+", shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nginx-source", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--files", type=int, default=4096)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1]
    source = args.nginx_source.resolve()
    cc = shlex.split(args.cc)
    sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
    # Modern compilers warn about upstream legacy code.  The module itself
    # is compiled separately with every required warning treated as an error.
    core_flags = ["-O1", "-g", "-pipe", "-Wall", "-Wno-unused-parameter"] + sanitizer
    run(["./configure", "--with-cc=" + args.cc,
         "--with-cc-opt=" + " ".join(core_flags),
         "--with-ld-opt=" + " ".join(sanitizer),
         "--without-http_rewrite_module", "--without-http_auth_basic_module",
         "--http-client-body-temp-path=client_body_temp", "--with-debug",
         "--add-module=" + str(module)], cwd=source)
    config = (source / "objs/ngx_auto_config.h").read_text()
    for feature in ("CACHE", "PROXY", "FASTCGI", "SCGI", "UWSGI"):
        assert re.search(r"#define NGX_HTTP_" + feature + r"\s+1\b", config), feature
    makefile = (source / "objs/Makefile").read_text()
    objects = re.findall(r"\S*addon/\S*ngx_cache_purge_module\.o", makefile)
    assert objects, "addon object absent from nginx's generated Makefile"
    target = source / objects[0]
    target.parent.mkdir(parents=True, exist_ok=True)
    includes = ["src/core", "src/event", "src/event/modules", "src/os/unix",
                "objs", "src/http", "src/http/modules"]
    strict_flags = ["-std=gnu89", "-O1", "-g", "-pipe", "-Wall", "-Wextra", "-Werror",
                    "-Wdeclaration-after-statement"] + sanitizer
    run(cc + strict_flags + ["-I" + str(source / p) for p in includes]
        + ["-c", module / "ngx_cache_purge_module.c", "-o", target])
    assert target.stat().st_size > 0
    run([sys.executable, module / "t/layout_safety.py", "--cc", args.cc,
         "--nginx-source", source])
    # Use the nginx target directly; unrelated generated manpage/install
    # rules are not part of module verification.
    run(["make", "-f", "objs/Makefile", "-j2", "objs/nginx",
         "CFLAGS=" + " ".join(core_flags)], cwd=source)
    run([sys.executable, module / "t/path_safety.py", "--cc", args.cc])
    run([sys.executable, module / "t/queue_safety.py",
         "--nginx", source / "objs/nginx", "--files", args.files,
         "--workers", args.workers])


if __name__ == "__main__":
    main()
