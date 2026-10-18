#!/usr/bin/env python3
"""Report the configured ABI, not estimated production RSS (Linux benchmark image)."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    source = Path(os.environ["NGINX_SOURCE"])
    build = source / "objs"
    # Use production declarations so this report follows layout changes.
    generation = (repo / "ngx_cache_purge_invalidation.c").read_text()
    generation = generation[generation.index("typedef struct ngx_http_cache_purge_rule_s"):
                            generation.index("static ngx_int_t")]
    refresh = re.search(r"(typedef struct \{\n    u_char\s+hash\[32\];.*?"
                        r"\} ngx_http_cache_refresh_t;)",
                        (repo / "ngx_cache_purge_refresh.c").read_text(), re.S)
    if refresh is None:
        raise RuntimeError("REFRESH declarations changed; update the layout probe")
    types = ["ngx_pool_t", "ngx_http_request_t", "ngx_http_cache_t",
             "ngx_http_file_cache_node_t", "ngx_http_cache_purge_rule_t",
             "ngx_http_cache_purge_generation_t", "ngx_http_cache_purge_path_t",
             "ngx_http_cache_purge_snapshot_t", "ngx_http_cache_purge_generation_request_t",
             "ngx_http_cache_purge_queue_t", "ngx_http_cache_purge_queue_item_t",
             "ngx_http_cache_purge_stats_t", "ngx_http_cache_purge_walk_ctx_t",
             "ngx_http_cache_purge_cursor_t", "ngx_http_cache_purge_work_t",
             "ngx_http_cache_purge_dir_t", "ngx_http_cache_refresh_entry_t",
             "ngx_http_cache_refresh_scan_t", "ngx_http_cache_refresh_t"]
    code = '#include "ngx_cache_purge.h"\n#include <stdio.h>\n'
    code += generation + refresh.group(1) + "\nint main(void) {\n"
    for name in types:
        code += f'printf("{name} %lu\\n", (unsigned long) sizeof({name}));\n'
    for name in ["NGX_MAX_PROCESSES", "NGX_CACHE_PURGE_REFRESH_BATCH", "NGX_PTR_SIZE"]:
        code += f'printf("{name} %lu\\n", (unsigned long) {name});\n'
    code += "return 0;\n}\n"
    includes = [repo, build] + [source / p for p in (
        "src/core", "src/event", "src/event/modules", "src/os/unix",
        "src/http", "src/http/modules", "src/http/v2", "src/http/v3")]
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / "layout.c").write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + ["-O2"]
        command += [f"-I{p}" for p in includes]
        command += [str(path / "layout.c"), "-o", str(path / "layout")]
        subprocess.run(command, check=True)
        values = dict(line.split() for line in subprocess.check_output(
            [str(path / "layout")], text=True).splitlines())
    report = {name: int(value) for name, value in values.items()}
    report["page_size"] = os.sysconf("SC_PAGE_SIZE")
    report["nginx_build"] = subprocess.run([str(build / "nginx"), "-V"],
        check=True, capture_output=True, text=True).stderr
    report["note"] = "sizeof values exclude allocator overhead, stacks and shared mapping accounting"
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "layout.json").write_text(json.dumps(report, indent=2) + "\n")
    stack_files = sorted(build.rglob("*ngx_cache_purge*.su"))
    if not stack_files:
        raise RuntimeError("compiler produced no module stack-usage reports")
    with (args.output / "stack-usage.tsv").open("w") as out:
        writer = csv.writer(out, delimiter="\t")
        writer.writerow(["function", "bytes", "classification"])
        for path in stack_files:
            for line in path.read_text().splitlines():
                writer.writerow(line.split("\t"))


if __name__ == "__main__":
    main()
