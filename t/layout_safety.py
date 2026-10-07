#!/usr/bin/env python3
"""Check private protocol offsets against upstream and native cache-key layout."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


METADATA_CHECK = r'''
#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <assert.h>
#include <stdio.h>
static int locked;
void ngx_shmtx_lock(ngx_shmtx_t *mtx) {
    (void) mtx;
    assert(!locked);
    locked = 1;
}
void ngx_shmtx_unlock(ngx_shmtx_t *mtx) {
    (void) mtx;
    assert(locked);
    locked = 0;
}
/* The lock stubs isolate representation/accounting, not interprocess locking. */
'''

METADATA_MAIN = r'''
int
main(void)
{
    ngx_http_file_cache_t cache;
    ngx_http_file_cache_sh_t shared;
    ngx_slab_pool_t pool;
    ngx_http_file_cache_node_t node;
    ngx_rbtree_node_t sentinel;
    ngx_str_t path = ngx_string("/cache/000102030405060708090a0b0c0d0e0f");
    u_char key[NGX_HTTP_CACHE_KEY_LEN];
    ngx_uint_t i;

    memset(&cache, 0, sizeof(cache));
    memset(&shared, 0, sizeof(shared));
    memset(&pool, 0, sizeof(pool));
    memset(&node, 0, sizeof(node));
    memset(&sentinel, 0, sizeof(sentinel));
    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        key[i] = (u_char) i;
    }
    /* This is the representation written by ngx_http_file_cache_lookup(). */
    memcpy(&node.node.key, key, sizeof(ngx_rbtree_key_t));
    memcpy(node.key, key + sizeof(ngx_rbtree_key_t), sizeof(node.key));
    node.node.left = &sentinel;
    node.node.right = &sentinel;
    node.exists = 1;
    node.fs_size = 3;
    shared.size = 9;
    shared.rbtree.root = &node.node;
    shared.rbtree.sentinel = &sentinel;
    cache.sh = &shared;
    cache.shpool = &pool;
    ngx_http_cache_purge_invalidate_node(&cache, &path);
    assert(!locked && !node.exists && node.fs_size == 0 && shared.size == 6);
    ngx_http_cache_purge_invalidate_node(&cache, &path);
    assert(!locked && shared.size == 6);
    puts("native cache-key representation and idempotent accounting passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nginx-source", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1] / "ngx_cache_purge_module.c"
    nginx = args.nginx_source.resolve()
    includes = ["src/core", "src/event", "src/event/modules", "src/os/unix",
                "objs", "src/http", "src/http/modules"]
    cc = shlex.split(args.cc) + ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
                               "-Wdeclaration-after-statement"]
    cc += ["-I" + str(nginx / path) for path in includes]
    layout = ['#include "' + str(module) + '"']
    for protocol in ("proxy", "fastcgi", "scgi", "uwsgi"):
        upstream = (nginx / f"src/http/modules/ngx_http_{protocol}_module.c").read_text()
        header = nginx / f"src/http/modules/ngx_http_{protocol}_module.h"
        if header.exists():
            upstream += header.read_text()
        declarations = []
        names = []
        for match in re.finditer(r"typedef struct \{\n.*?\n\} (ngx_http_" + protocol + r"_[a-z_]+_t);", upstream, re.S):
            name = match[1]
            if name.endswith(("_main_conf_t", "_loc_conf_t", "_params_t", "_headers_t", "_vars_t")):
                declarations.append(match[0])
                names.append(name)
        assert f"ngx_http_{protocol}_loc_conf_t" in names
        text = "\n".join(declarations)
        for name in names:
            text = re.sub(r"\b" + name + r"\b", "upstream_" + name, text)
        layout.append(text)
        for field in ("upstream", "cache_key", protocol + "_lengths"):
            name = f"ngx_http_{protocol}_loc_conf_t"
            layout.append(f"#if (NGX_HTTP_{protocol.upper()})\n"
                          f"typedef char check_{protocol}_{field}[(offsetof({name}, {field})\n"
                          f"    == offsetof(upstream_{name}, {field})) ? 1 : -1];\n#endif")
    source = module.read_text()
    function = re.search(r"static void\nngx_http_cache_purge_invalidate_node\(.*?\n}", source, re.S)
    assert function
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        (base / "layout.c").write_text("\n".join(layout))
        subprocess.run(cc + ["-c", str(base / "layout.c"), "-o", str(base / "layout.o")], check=True)
        print("private protocol offsets agree with this upstream source", flush=True)
        (base / "metadata.c").write_text(METADATA_CHECK + function[0] + METADATA_MAIN)
        subprocess.run(cc + [str(base / "metadata.c"), "-o", str(base / "metadata")], check=True)
        subprocess.run([str(base / "metadata")], check=True)


if __name__ == "__main__":
    main()
