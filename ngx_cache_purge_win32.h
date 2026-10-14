#ifndef _NGX_CACHE_PURGE_WIN32_H_INCLUDED_
#define _NGX_CACHE_PURGE_WIN32_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>


static ngx_inline ngx_uint_t
ngx_http_cache_purge_process_alive(ngx_pid_t pid)
{
    HANDLE  process;
    DWORD   state;

    process = OpenProcess(SYNCHRONIZE, 0, pid);
    if (process == NULL) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state != WAIT_OBJECT_0;
}


static ngx_inline ngx_uint_t
ngx_http_cache_purge_path_separator(u_char c)
{
    return c == '/' || c == '\\';
}


static ngx_inline u_char
ngx_http_cache_purge_path_char(u_char c)
{
    if (ngx_http_cache_purge_path_separator(c)) {
        return '/';
    }

    return ngx_tolower(c);
}


static ngx_inline size_t
ngx_http_cache_purge_path_min_length(const ngx_str_t *path)
{
    return (path->len >= 3 && path->data[1] == ':'
            && ngx_http_cache_purge_path_separator(path->data[2])) ? 3 : 1;
}


/* Directory entries carry metadata; reparse points are skipped. */
static ngx_inline ngx_int_t
ngx_http_cache_purge_de_info(ngx_str_t *path, ngx_dir_t *dir,
    const char **failed)
{
    (void) path;
    (void) failed;

    if (dir->finddata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


static ngx_inline ngx_int_t
ngx_http_cache_purge_file_info(ngx_str_t *path, ngx_dir_t *dir,
    const char **failed)
{
    return ngx_http_cache_purge_de_info(path, dir, failed) == NGX_OK
           && ngx_de_is_file(dir) ? NGX_OK : NGX_DECLINED;
}


static ngx_inline ngx_int_t
ngx_http_cache_purge_open_dir(ngx_str_t *path, ngx_dir_t *parent,
    ngx_dir_t *dir, const char **failed)
{
    ngx_file_info_t  info;

    if (parent != NULL) {
        *failed = ngx_link_info_n;
        if (ngx_link_info(path->data, &info) == NGX_FILE_ERROR) {
            return NGX_ERROR;
        }
        if (!ngx_is_dir(&info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            return NGX_DECLINED;
        }
    }

    *failed = ngx_open_dir_n;
    return ngx_open_dir(path, dir);
}


static ngx_inline ngx_fd_t
ngx_http_cache_purge_open_entry(ngx_str_t *path, ngx_dir_t *parent,
    const char **failed)
{
    (void) parent;

    *failed = ngx_open_file_n;
    return ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
}


static ngx_inline ngx_int_t
ngx_http_cache_purge_delete_entry(u_char *name, ngx_dir_t *parent,
    const char **failed)
{
    (void) parent;

    *failed = ngx_delete_file_n;
    return ngx_delete_file(name);
}


#endif
