#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef unsigned char u_char;
typedef unsigned long ngx_uint_t;
typedef long ngx_int_t;
typedef unsigned long ngx_msec_t;
typedef long ngx_msec_int_t;
typedef int ngx_err_t;
typedef int ngx_log_t;
typedef int ngx_fd_t;
typedef struct stat ngx_file_info_t;
typedef struct { size_t len; u_char *data; } ngx_str_t;
typedef struct {
    DIR *dir;
    struct dirent *entry;
    struct stat info;
    unsigned valid_info;
    unsigned type;
} ngx_dir_t;
typedef struct {
    off_t file_size;
    ngx_uint_t scan_errors;
    ngx_str_t cache_root;
    ngx_log_t *log;
    ngx_dir_t *parent_dir;
    void (*visit)(void *ctx, ngx_str_t *path);
} ngx_http_cache_purge_walk_ctx_t;

#define NGX_OK 0
#define ngx_inline inline
#define NGX_ERROR -1
#define NGX_AGAIN -2
#define NGX_ABORT -6
#define NGX_DECLINED -5
#define NGX_FILE_ERROR -1
#define NGX_INVALID_FILE -1
#define NGX_HAVE_OPENAT 1
#define NGX_FILE_RDONLY O_RDONLY
#define NGX_FILE_NONBLOCK O_NONBLOCK
#define NGX_FILE_NOFOLLOW O_NOFOLLOW
#define NGX_FILE_OPEN 0
#define ngx_file_at_info(fd, name, info, flags) fstatat(fd, (char *) name, info, flags)
#define ngx_link_info(p, s) lstat((char *) (p), s)
#define ngx_is_dir(s) S_ISDIR((s)->st_mode)
#define ngx_is_link(s) S_ISLNK((s)->st_mode)
#define ngx_close_file close
#define NGX_ENOENT ENOENT
#define NGX_ELOOP ELOOP
#define NGX_ENOTDIR ENOTDIR
#define NGX_ENOMOREFILES 0
#define NGX_LOG_CRIT 3
#define NGX_CACHE_PURGE_WALK_MSEC 5
#define ngx_errno errno
#define ngx_set_errno(n) (errno = (n))
#define ngx_memzero(p, n) memset(p, 0, n)
#define ngx_memcpy(d, s, n) memcpy(d, s, n)
#define ngx_cpymem(d, s, n) ((u_char *) memcpy(d, s, n) + (n))
#define ngx_de_name(d) ((u_char *) (d)->entry->d_name)
#define ngx_de_namelen(d) strlen((d)->entry->d_name)
#define ngx_de_link_info(p, d) lstat((char *) (p), &(d)->info)
#define ngx_de_size(d) ((d)->info.st_size)
#define ngx_de_is_file(d) S_ISREG((d)->info.st_mode)
#define ngx_de_is_dir(d) S_ISDIR((d)->info.st_mode)
#define ngx_open_dir_n "opendir()"
#define ngx_openat_file_n "openat()"
#define ngx_file_at_info_n "fstatat()"
#define ngx_link_info_n "lstat()"
#define ngx_close_dir_n "closedir()"
#define ngx_read_dir_n "readdir()"
#define ngx_de_link_info_n "lstat()"
#define ngx_log_error(level, log, error, ...) ((void) (error))

static ngx_msec_t ngx_current_msec;
static unsigned clock_step, reads, handles, allocs, live, fail_alloc, matched;
static unsigned fail_directory_read;
static char *outside_path;

#if (NGX_CACHE_PURGE_HAVE_AT)
static unsigned swap_directory;
static char swap_path[1024], moved_path[1024];
static int test_openat(int fd, u_char *name, int mode, int create, int access)
{
    (void) create;
    (void) access;
    if (swap_directory && strcmp((char *) name, "swap") == 0) {
        swap_directory = 0;
        assert(rename(swap_path, moved_path) == 0);
        assert(symlink(outside_path, swap_path) == 0);
    }
    return openat(fd, (char *) name, mode);
}
#define ngx_openat_file test_openat

static DIR *test_fdopendir(int fd)
{
    DIR *d = fdopendir(fd);
    if (d != NULL) { handles++; }
    return d;
}
#define fdopendir test_fdopendir
#endif

static void ngx_time_update(void) { ngx_current_msec += clock_step; }

static void *ngx_alloc(size_t n, ngx_log_t *log)
{
    void *p;
    (void) log;
    if (++allocs == fail_alloc) { return NULL; }
    p = malloc(n);
    assert(p != NULL);
    live++;
    return p;
}

static void ngx_free(void *p)
{
    assert(p != NULL && live != 0);
    live--;
    free(p);
}

static ngx_int_t ngx_open_dir(ngx_str_t *name, ngx_dir_t *dir)
{
    dir->dir = opendir((char *) name->data);
    dir->valid_info = 0;
    if (dir->dir == NULL) { return NGX_ERROR; }
    handles++;
    return NGX_OK;
}

static ngx_int_t ngx_close_dir(ngx_dir_t *dir)
{
    assert(handles != 0);
    handles--;
    return closedir(dir->dir);
}

static ngx_int_t ngx_read_dir(ngx_dir_t *dir)
{
    reads++;
    dir->valid_info = 0;
    if (fail_directory_read) {
        fail_directory_read = 0;
        errno = EIO;
        return NGX_ERROR;
    }
    dir->entry = readdir(dir->dir);
    return dir->entry == NULL ? NGX_ERROR : NGX_OK;
}

static void ngx_http_cache_purge_delete_file(
    ngx_http_cache_purge_walk_ctx_t *ctx, ngx_str_t *path)
{
    struct stat info;
    (void) ctx;
    assert(path->data[path->len] == 0);
    assert(lstat((char *) path->data, &info) == 0 && S_ISREG(info.st_mode));
    matched++;
}

static ngx_int_t ngx_http_cache_purge_skip_protected_dir(
    ngx_http_cache_purge_walk_ctx_t *ctx, ngx_str_t *path)
{
    (void) ctx;
    (void) path;
    return NGX_OK;
}
