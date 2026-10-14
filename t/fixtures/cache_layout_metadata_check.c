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
/* Mock locks check call order, not interprocess locking. */
