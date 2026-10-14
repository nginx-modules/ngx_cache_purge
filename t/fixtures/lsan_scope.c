/* Deliberate leaks: verify the core exception does not disable other checks. */
#include <stdlib.h>

static void *volatile allocation;

__attribute__((noinline)) void
ngx_event_process_init(void)
{
    allocation = malloc(37);
    allocation = NULL;
}

__attribute__((noinline)) void
ngx_cache_purge_lsan_probe(void)
{
    allocation = malloc(17);
    allocation = NULL;
}

int
main(int argc, char **argv)
{
    (void) argv;
    if (argc > 1) {
        ngx_cache_purge_lsan_probe();
    } else {
        ngx_event_process_init();
    }
    return 0;
}
