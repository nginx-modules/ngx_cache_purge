static unsigned locked, fail_alloc, allocations, live, busy;
static ngx_uint_t test_trylock(ngx_shmtx_t *mutex)
{
    (void) mutex;
    assert(!locked);
    if (busy) { return 0; }
    locked = 1;
    return 1;
}
static void test_lock(ngx_shmtx_t *mutex)
{ assert(test_trylock(mutex)); }
static void test_unlock(ngx_shmtx_t *mutex)
{ (void) mutex; assert(locked); locked = 0; }
static void *test_allocate(ngx_slab_pool_t *slab, size_t size)
{
    void *p;
    (void) slab;
    assert(locked);
    if (++allocations == fail_alloc) { return NULL; }
    p = calloc(1, size);
    assert(p != NULL);
    live++;
    return p;
}
static void test_free(ngx_slab_pool_t *slab, void *p)
{ (void) slab; assert(locked && live); live--; free(p); }
#define ngx_shmtx_trylock test_trylock
#define ngx_shmtx_lock test_lock
#define ngx_shmtx_unlock test_unlock
#define ngx_slab_calloc_locked test_allocate
#define ngx_slab_free_locked test_free
