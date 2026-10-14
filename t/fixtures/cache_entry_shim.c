#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
#include <assert.h>
#include <stdio.h>

#define NGX_CACHE_PURGE_ENTRY_KEY_MAX 8192
#define NGX_CACHE_PURGE_KEY_HDR_OFFSET 6

typedef struct {
    u_char key_buffer[NGX_CACHE_PURGE_ENTRY_KEY_MAX + 1];
    size_t entry_key_len;
    ngx_http_file_cache_header_t header;
    off_t file_size;
    ngx_log_t *log;
    ngx_dir_t *parent_dir;
} ngx_http_cache_purge_walk_ctx_t;

static u_char contents[16384];
static size_t length;
static unsigned closes, reads, short_read, fail_read, fail_open, fail_stat, directory;
static void test_log(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{ (void) level; (void) log; (void) err; (void) fmt; }
#undef ngx_log_error
#define ngx_log_error test_log
static ngx_fd_t ngx_http_cache_purge_open_entry(ngx_str_t *path, ngx_dir_t *parent,
    const char **failed)
{ (void) path; (void) parent; *failed = ngx_open_file_n;
  return fail_open ? NGX_INVALID_FILE : 3; }
static void ngx_http_cache_purge_open_failed(ngx_log_t *log, u_char *path,
    const char *failed_op)
{ (void) log; (void) path; (void) failed_op; }
static int test_stat(ngx_fd_t fd, ngx_file_info_t *info)
{
    assert(fd == 3);
    memset(info, 0, sizeof(*info));
    info->st_mode = directory ? S_IFDIR : S_IFREG;
    info->st_size = length;
    return fail_stat ? NGX_FILE_ERROR : 0;
}
static int test_close(ngx_fd_t fd) { assert(fd == 3); closes++; return 0; }
static ssize_t test_read(ngx_file_t *file, u_char *buf, size_t size, off_t offset)
{
    size_t available;
    assert(file->fd == 3 && file->log != NULL && offset >= 0);
    reads++;
    if (reads == fail_read) { return NGX_ERROR; }
    available = (size_t) offset > length ? 0 : length - offset;
    size = ngx_min(size, available);
    if (size != 0 && reads == short_read) { size--; }
    memcpy(buf, contents + offset, size);
    return (ssize_t) size;
}
static uint32_t test_crc(u_char *p, size_t len)
{
    uint32_t crc = 0xffffffff;
    unsigned bit;
    while (len--) {
        crc ^= *p++;
        for (bit = 0; bit < 8; bit++) { crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
    }
    return crc ^ 0xffffffff;
}
#undef ngx_fd_info
#undef ngx_close_file
#define ngx_fd_info test_stat
#define ngx_close_file test_close
#define ngx_read_file test_read
#define ngx_crc32_long test_crc
