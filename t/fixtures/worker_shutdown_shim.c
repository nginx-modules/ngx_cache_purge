#include <assert.h>
#include <stddef.h>

#define NGX_THREADS 1
#define ngx_memory_barrier() ((void) 0)
#define ngx_str_null(s) ((s)->data = NULL)
typedef int ngx_cycle_t;
typedef int ngx_uint_t;
#define NGX_CONF_UNSET_UINT -1
static ngx_uint_t ngx_cache_purge_slot = NGX_CONF_UNSET_UINT;
typedef struct { int mutex; } test_slab_t;
typedef struct { test_slab_t *shpool; int owners[1]; unsigned probes[1]; } test_queue_t;
typedef struct { test_queue_t *queue; } test_conf_t;
static void ngx_shmtx_lock(int *p) { (void) p; }
static void ngx_shmtx_unlock(int *p) { (void) p; }
typedef struct { int timer_set; int active; } ngx_event_t;
typedef struct { ngx_event_t event; } ngx_thread_task_t;
typedef struct {
    ngx_event_t event;
    test_conf_t *conf;
    ngx_thread_task_t *task;
    volatile unsigned finished;
    void *item;
    struct { struct { void *data; } path; size_t allocated; } cursor;
} ngx_http_cache_purge_work_t;
static ngx_http_cache_purge_work_t ngx_cache_purge_work;
static unsigned finalized, freed;
static void ngx_del_timer(ngx_event_t *ev) { ev->timer_set = 0; }
static void ngx_free(void *data) { assert(data != NULL); freed++; }
static int ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle,
    ngx_uint_t final)
{
    (void) cycle;
    assert(final == 1);
    finalized++;
    ngx_cache_purge_work.item = NULL;
    return 0;
}
