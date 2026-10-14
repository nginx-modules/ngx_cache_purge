#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char u_char;
typedef long ngx_int_t;
typedef int ngx_err_t;
typedef int ngx_log_t;
#define ngx_inline inline
#define NGX_OK 0
#define NGX_ERROR -1
#define NGX_DECLINED -5
#define NGX_FILE_ERROR -1
#define NGX_ENOENT ENOENT
#define NGX_LOG_CRIT 3
#define NGX_LOG_ERR 4
#define ngx_errno errno
#define ngx_delete_file_n "unlink()"
#define ngx_open_file_n "open()"
static int file_result, logged, logged_level, logged_errno;
static char message[256];
static int
ngx_delete_file(u_char *name)
{
    (void) name;
    return file_result;
}
static void
ngx_log_error(int level, ngx_log_t *log, ngx_err_t err, const char *fmt, ...)
{
    va_list args;
    (void) log;
    logged++;
    logged_level = level;
    logged_errno = err;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    errno = EIO;
}
