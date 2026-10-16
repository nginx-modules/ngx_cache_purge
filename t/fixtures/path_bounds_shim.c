#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef unsigned char u_char;
typedef unsigned long ngx_uint_t;
typedef uint32_t ngx_msec_t;
typedef int32_t ngx_msec_int_t;
static ngx_msec_t ngx_current_msec;
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
/* Heap leak checks are separate. */
int __lsan_is_turned_off(void) { return 1; }
#define ngx_string(s) { sizeof(s) - 1, (u_char *) s }
#define ngx_inline inline
#define ngx_tolower(c) ((u_char) (((c) >= 'A' && (c) <= 'Z') ? (c) | 0x20 : (c)))
