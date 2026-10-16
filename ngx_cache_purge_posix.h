#ifndef _NGX_CACHE_PURGE_POSIX_H_INCLUDED_
#define _NGX_CACHE_PURGE_POSIX_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>


static ngx_inline ngx_uint_t
ngx_http_cache_purge_process_alive(ngx_pid_t pid)
{
    return kill(pid, 0) == 0 || ngx_errno != NGX_ESRCH;
}


static ngx_inline ngx_uint_t
ngx_http_cache_purge_path_separator(u_char c)
{
    return c == '/';
}


static ngx_inline u_char
ngx_http_cache_purge_path_char(u_char c)
{
#if (NGX_HAVE_CASELESS_FILESYSTEM)
    return ngx_tolower(c);
#else
    return c;
#endif
}


static ngx_inline size_t
ngx_http_cache_purge_path_min_length(const ngx_str_t *path)
{
    (void) path;

    return 1;
}


/* Known entry types avoid stat calls; links are never followed. */
static ngx_inline ngx_int_t
ngx_http_cache_purge_de_info(ngx_str_t *path, ngx_dir_t *dir,
    const char **failed)
{
#if (NGX_HAVE_OPENAT && NGX_CACHE_PURGE_HAVE_AT)
    (void) path;
#endif
    if (!dir->valid_info
#if (NGX_HAVE_D_TYPE)
        && dir->type == 0
#endif
       )
    {
#if (NGX_HAVE_OPENAT && NGX_CACHE_PURGE_HAVE_AT)
        *failed = ngx_file_at_info_n;
        if (ngx_file_at_info(dirfd(dir->dir), ngx_de_name(dir), &dir->info,
                             AT_SYMLINK_NOFOLLOW) == NGX_FILE_ERROR)
#else
        *failed = ngx_de_link_info_n;
        if (ngx_de_link_info(path->data, dir) == NGX_FILE_ERROR)
#endif
        {
            return NGX_ERROR;
        }
        dir->valid_info = 1;
    }

    return NGX_OK;
}


static ngx_inline ngx_int_t
ngx_http_cache_purge_file_info(ngx_str_t *path, ngx_dir_t *dir,
    const char **failed)
{
    if (!dir->valid_info) {
        dir->type = 0;
        if (ngx_http_cache_purge_de_info(path, dir, failed) != NGX_OK) {
            return NGX_ERROR;
        }
    }
    return ngx_de_is_file(dir) ? NGX_OK : NGX_DECLINED;
}


/* Only the explicitly configured root may resolve through a symbolic link. */
static ngx_inline ngx_int_t
ngx_http_cache_purge_open_dir(ngx_str_t *path, ngx_dir_t *parent,
    ngx_dir_t *dir, const char **failed)
{
#if (NGX_HAVE_OPENAT && NGX_CACHE_PURGE_HAVE_AT)
    ngx_fd_t  fd;
    ngx_err_t err;

    if (parent != NULL) {
        *failed = ngx_openat_file_n;
        fd = ngx_openat_file(dirfd(parent->dir), ngx_de_name(parent),
                            NGX_FILE_RDONLY|NGX_FILE_NONBLOCK|NGX_FILE_NOFOLLOW
                            |O_DIRECTORY, NGX_FILE_OPEN, 0);
        if (fd == NGX_INVALID_FILE) {
            if (ngx_errno == NGX_ELOOP || ngx_errno == NGX_ENOTDIR) {
                return NGX_DECLINED;
            }
            return NGX_ERROR;
        }
        ngx_memzero(dir, sizeof(*dir));
        *failed = "fdopendir()";
        dir->dir = fdopendir(fd);
        if (dir->dir == NULL) {
            err = ngx_errno;
            (void) ngx_close_file(fd);
            ngx_set_errno(err);
            return NGX_ERROR;
        }
        return NGX_OK;
    }
#else
    ngx_file_info_t  info;

    if (parent != NULL) {
        *failed = ngx_link_info_n;
        if (ngx_link_info(path->data, &info) == NGX_FILE_ERROR) {
            return NGX_ERROR;
        }
        if (!ngx_is_dir(&info) || ngx_is_link(&info)) {
            return NGX_DECLINED;
        }
    }
#endif

    *failed = ngx_open_dir_n;
    return ngx_open_dir(path, dir);
}


static ngx_inline ngx_fd_t
ngx_http_cache_purge_open_entry(ngx_str_t *path, ngx_dir_t *parent,
    const char **failed)
{
#if (NGX_HAVE_OPENAT && NGX_CACHE_PURGE_HAVE_AT)
    if (parent != NULL) {
        *failed = ngx_openat_file_n;
        return ngx_openat_file(dirfd(parent->dir), ngx_de_name(parent),
                               NGX_FILE_RDONLY|NGX_FILE_NONBLOCK
                               |NGX_FILE_NOFOLLOW, NGX_FILE_OPEN, 0);
    }
#else
    (void) parent;
#endif

    *failed = ngx_open_file_n;
    return ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
}


static ngx_inline ngx_int_t
ngx_http_cache_purge_delete_entry(u_char *name, ngx_dir_t *parent,
    const char **failed)
{
#if (NGX_HAVE_OPENAT && NGX_CACHE_PURGE_HAVE_AT)
    if (parent != NULL) {
        /* No core unlinkat wrapper; all use is behind the complete probe. */
        *failed = "unlinkat()";
        return unlinkat(dirfd(parent->dir), (char *) ngx_de_name(parent), 0);
    }
#else
    (void) parent;
#endif

    *failed = ngx_delete_file_n;
    return ngx_delete_file(name);
}


#endif
