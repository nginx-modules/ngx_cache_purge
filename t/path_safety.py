#!/usr/bin/env python3
"""Exercise the actual C path helpers with guard pages, ASan and UBSan."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

SHIM = r'''
#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef unsigned char u_char;
typedef unsigned long ngx_uint_t;
typedef long ngx_flag_t;
typedef struct { size_t len; u_char *data; } ngx_str_t;
typedef struct { void *elts; ngx_uint_t nelts; } ngx_array_t;
typedef struct { ngx_str_t name; void *manager; void *loader; } ngx_path_t;
typedef struct { ngx_str_t *names; ngx_uint_t nelts; } protection_t;
typedef struct {
    ngx_str_t cache_root;
    protection_t *protection;
    ngx_array_t *paths;
} ngx_http_cache_purge_walk_ctx_t;
/* This mmap-only harness tests bounds; it does not test heap leaks. */
int __lsan_is_turned_off(void) { return 1; }
#define ngx_string(s) { sizeof(s) - 1, (u_char *) s }
#define ngx_tolower(c) ((u_char) (((c) >= 'A' && (c) <= 'Z') ? (c) | 0x20 : (c)))
'''

HARNESS = r'''
static void
check(char *root, char *input, int directory, unsigned int expected)
{
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_str_t path;
    long page;
    u_char *memory;
    size_t len;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) root;
    ctx.cache_root.len = strlen(root);
    page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    memory = mmap(NULL, (size_t) page * 2, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, (size_t) page, PROT_NONE) == 0);
    len = strlen(input);
    assert(len < (size_t) page);
    path.data = memory + page - len;
    path.len = len;
    memcpy(path.data, input, len); /* Deliberately no trailing NUL. */
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path, directory)
           == expected);
    assert(munmap(memory, (size_t) page * 2) == 0);
}

int
main(void)
{
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_str_t path, names[1];
    protection_t protection;
    unsigned int state, count;
    size_t i, len;
    u_char input[256];

    check("/var/cache", "/var/cache/proxy_temp/", 1, 1);
    check("/var/cache", "/var/cache/proxy_temp/abc", 0, 1);
    check("/var/cache", "/var/cache/main/app_proxy_temp_file", 1, 0);
    check("/var/cache", "/var/cache/main/proxy_temp2", 1, 0);
    check("/var/cache", "/var/cache/main/my_proxy_temp_backup", 1, 0);
    check("/var/cache", "/var/cache/proxy_temp", 0, 0);
    check("/var/cache", "/var/cache//scgi_temp", 1, 1);
    check("/var/cache", "/var/cache/uwsgi_temp", 1, 1);
    check("/var/cache", "/var/cache/client_body_temp/1", 0, 1);
    check("/var/cache", "/var/cache/client_temp/1", 0, 1);
    check("/var/cache", "/var/cache/fastcgi_temp/1", 0, 1);
    check("/proxy_temp/cache", "/proxy_temp/cache/main/entry", 0, 0);
    check("/var/cache/proxy_temp", "/var/cache/proxy_temp/entry", 0, 0);
    check("/var/cache///", "/var/cache/main/entry", 0, 0);
    check("/", "/proxy_temp/entry", 0, 1);
    check("/var/cache", "/var/cache-other/proxy_temp/entry", 0, 1);
    check("/var/cache", "", 1, 1);
#if (NGX_WIN32)
    check("C:\\Cache", "c:/cache\\PROXY_TEMP\\entry", 0, 1);
    check("C:/", "c:\\proxy_temp\\entry", 0, 1);
    check("//server/share/cache", "\\\\server\\share\\cache\\scgi_temp", 1, 1);
    check("/var/cache", "/var/cache/name\\proxy_temp/entry", 0, 1);
#else
    check("/var/cache", "/var/cache/name\\proxy_temp/entry", 0, 0);
#if (NGX_HAVE_CASELESS_FILESYSTEM)
    check("/var/cache", "/var/cache/PROXY_TEMP/entry", 0, 1);
#else
    check("/var/cache", "/var/cache/PROXY_TEMP/entry", 0, 0);
#endif
#endif

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) "/var/cache";
    ctx.cache_root.len = sizeof("/var/cache") - 1;
    names[0].data = (u_char *) "/var/cache/custom-spool";
    names[0].len = sizeof("/var/cache/custom-spool") - 1;
    protection.names = names;
    protection.nelts = 1;
    ctx.protection = &protection;
    path.data = (u_char *) "/var/cache/custom-spool/entry";
    path.len = sizeof("/var/cache/custom-spool/entry") - 1;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path, 0) == 1);
    path.data = (u_char *) "/var/cache/custom-spool-backup/entry";
    path.len = sizeof("/var/cache/custom-spool-backup/entry") - 1;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path, 0) == 0);
    path.data = NULL;
    path.len = 0;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path, 0) == 1);

    path.data = (u_char *) "/cache/0123456789abcdef0123456789abcdef";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 1);
    path.data = (u_char *) "/cache/0000000123";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 0);
    path.data = (u_char *) "/cache/0123456789abcdef0123456789abcdef.0000000123";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) "/";
    ctx.cache_root.len = 1;
    state = 12345;
    for (count = 0; count < 100000; count++) {
        state = state * 1664525U + 1013904223U;
        len = state % sizeof(input);
        for (i = 0; i < len; i++) {
            state = state * 1664525U + 1013904223U;
            input[i] = (u_char) (state >> 24);
        }
        path.data = input;
        path.len = len;
        (void) ngx_http_cache_purge_is_protected_path(&ctx, &path, count & 1);
        (void) ngx_http_cache_purge_is_cache_file(&path);
    }
    puts("path bounds and delimiter checks passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--no-sanitize", action="store_true")
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / "ngx_cache_purge_module.c").read_text()
    names = ["path_separator", "path_equal", "path_length", "path_contains",
             "is_protected_path", "is_cache_file"]
    pieces = [SHIM]
    table = re.search(r"static const ngx_str_t ngx_http_cache_purge_protected_dirs\[\].*?\n};", source, re.S)
    pieces.append(table.group())
    for name in names:
        match = re.search(r"static (?:ngx_uint_t|size_t)\nngx_http_cache_purge_" + name + r"\(.*?\n}", source, re.S)
        assert match, name
        pieces.append(match.group())
    pieces.append(HARNESS)
    with tempfile.TemporaryDirectory() as temporary:
        cfile = Path(temporary) / "path_safety.c"
        cfile.write_text("\n\n".join(pieces))
        for windows, caseless in ((0, 0), (1, 1), (0, 1)):
            output = Path(temporary) / f"check-{windows}-{caseless}"
            command = shlex.split(args.cc) + ["-std=gnu89", "-Wall", "-Wextra", "-Werror",
                       "-Wdeclaration-after-statement", "-O1", "-g",
                       "-DNGX_WIN32=" + str(windows),
                       "-DNGX_HAVE_CASELESS_FILESYSTEM=" + str(caseless),
                       str(cfile), "-o", str(output)]
            if not args.no_sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
