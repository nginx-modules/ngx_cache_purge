#!/usr/bin/env python3
"""Compile the actual path guards and timer expression under sanitizers.

The small type adapter models both path conventions, not a Windows runtime.
Set CC and CFLAGS to select a compiler and optional sanitizer flags.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


source = (Path(__file__).resolve().parent.parent / "ngx_cache_purge_module.c").read_text()
start = source.index("#if (NGX_WIN32)\n#define ngx_http_cache_purge_separator")
end = source.index("/* -- file-walk helpers", start)
guards = source[start:end]
age = re.search(r"age = \(ngx_msec_int_t\).*?;", source).group()

adapter = r'''
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u_char;
typedef unsigned long ngx_uint_t;
typedef long ngx_int_t;
typedef unsigned int ngx_msec_t;
typedef int ngx_msec_int_t;
typedef struct { size_t len; u_char *data; } ngx_str_t;
typedef struct { size_t root_len; } ngx_http_cache_purge_walk_ctx_t;
typedef struct { void *data; } ngx_tree_ctx_t;
#define ngx_string(s) {sizeof(s) - 1, (u_char *) s}
#define ngx_tolower(c) tolower(c)
#define NGX_DECLINED -5
#define NGX_OK 0
static ngx_int_t ngx_filename_cmp(u_char *a, u_char *b, size_t n)
{
    size_t i;
    int x, y;
    for (i = 0; i < n; i++) {
        x = CASELESS ? tolower(a[i]) : a[i];
        y = CASELESS ? tolower(b[i]) : b[i];
        if (x != y || x == 0) {
            return x - y;
        }
    }
    return 0;
}
'''

tests = r'''
static void check_path(const char *text, size_t root_len,
    ngx_uint_t protected, ngx_uint_t cache_file)
{
    ngx_str_t path;
    ngx_tree_ctx_t tree;
    ngx_http_cache_purge_walk_ctx_t ctx;
    path.len = strlen(text);
    path.data = malloc(path.len ? path.len : 1);
    assert(path.data != NULL);
    memcpy(path.data, text, path.len);
    ctx.root_len = root_len;
    tree.data = &ctx;
    assert(ngx_http_cache_purge_protected(&path, root_len) == protected);
    assert(ngx_http_cache_purge_cache_file(&tree, &path) == cache_file);
    assert(ngx_http_cache_purge_pre_tree(&tree, &path)
           == (protected ? NGX_DECLINED : NGX_OK));
    assert(memcmp(path.data, text, path.len) == 0);
    free(path.data);
}
static ngx_msec_int_t elapsed(ngx_msec_t now, ngx_msec_t then)
{
    struct { ngx_msec_t enqueued_at; } stamp, *item;
    ngx_msec_t ngx_current_msec;
    ngx_msec_int_t age;
    stamp.enqueued_at = then;
    item = &stamp;
    ngx_current_msec = now;
    AGE_EXPRESSION
    return age;
}
int main(void)
{
    ngx_str_t path;
    ngx_tree_ctx_t tree;
    ngx_http_cache_purge_walk_ctx_t ctx;
    size_t n, root, i;
    unsigned long random;
    const char *hex = "0123456789abcdef0123456789abcdef";
    char name[160];

    assert(sizeof(ngx_msec_t) == 4);
    assert(elapsed(120001, (ngx_msec_t) -1000) == 121001);
    assert(elapsed(99, 100) == -1);
    assert(elapsed(60001, 0) == 60001);
    assert(elapsed(0, (ngx_msec_t) -1) == 1);

    check_path("", 0, 0, 0);
    check_path("/cache/proxy_temp", 6, 1, 0);
    check_path("/cache/proxy_temp2", 6, 0, 0);
    check_path("/cache/staging/0000000001", 6, 0, 0);
    sprintf(name, "/cache/%s", hex);
    check_path(name, 6, 0, 1);
    check_path(name, strlen(name) + 1, 0, 0);
    sprintf(name, "/cache/%s.0000000001", hex);
    check_path(name, 6, 0, 0);
    sprintf(name, "/cache/prefix%s", hex);
    check_path(name, 6, 0, 0);
    sprintf(name, "/srv/proxy_temp/cache/%s", hex);
    check_path(name, strlen("/srv/proxy_temp/cache"), 0, 1);
    sprintf(name, "/cache/app_proxy_temp_file/%s", hex);
    check_path(name, 6, 0, 1);
    sprintf(name, "/cache/proxy_temp/%s", hex);
    check_path(name, 6, 1, 0);
    sprintf(name, "/cache/Proxy_Temp/%s", hex);
    check_path(name, 6, CASELESS, !CASELESS);
    sprintf(name, "/cache/x\\proxy_temp/%s", hex);
    check_path(name, 6, NGX_WIN32, !NGX_WIN32);

    /* Exact allocations have no terminator or readable byte after len. */
    random = 1;
    tree.data = &ctx;
    for (n = 0; n < 128; n++) {
        path.len = n;
        path.data = malloc(n ? n : 1);
        assert(path.data != NULL);
        for (i = 0; i < n; i++) {
            random = random * 1664525UL + 1013904223UL;
            path.data[i] = (u_char) (random >> 16);
        }
        for (root = 0; root <= n + 1; root++) {
            ctx.root_len = root;
            (void) ngx_http_cache_purge_cache_file(&tree, &path);
            (void) ngx_http_cache_purge_pre_tree(&tree, &path);
        }
        free(path.data);
    }
    printf("path guards and 32-bit timer: WIN32=%d CASELESS=%d passed\n",
           NGX_WIN32, CASELESS);
    return 0;
}
'''.replace("AGE_EXPRESSION", age)

with tempfile.TemporaryDirectory(prefix="purge-bounds-", dir=Path.cwd()) as td:
    path = Path(td)
    unit = path / "bounds.c"
    unit.write_text(adapter + guards + tests)
    for windows, caseless in [(0, 0), (0, 1), (1, 1)]:
        binary = path / "bounds"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
                    "-Wdeclaration-after-statement", "-g", "-O1"]
        command += shlex.split(os.environ.get("CFLAGS", ""))
        command += [f"-DNGX_WIN32={windows}", f"-DCASELESS={caseless}",
                    str(unit), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
