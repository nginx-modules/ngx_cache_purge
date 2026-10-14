#include <ngx_config.h>
#include <ngx_core.h>
#include <assert.h>

extern int lsan_cycle_fail, lsan_cycle_leak;

static __attribute__((noinline)) void
check_cycle(void)
{
    ngx_cycle_t  *cycle, bootstrap;

    memset(&bootstrap, 0, sizeof(bootstrap));
    bootstrap.pool = calloc(1, 1024);
    assert(bootstrap.pool != NULL);
    cycle = ngx_init_cycle(&bootstrap);
    if (lsan_cycle_fail) {
        assert(cycle == NULL);
    } else {
        assert(cycle != NULL && cycle->connection_n == 7);
    }
}

int
main(int argc, char **argv)
{
    assert(argc == 2);
    ngx_test_config = strstr(argv[1], "runtime") == NULL;
    lsan_cycle_fail = strncmp(argv[1], "failed", 6) == 0;
    lsan_cycle_leak = strstr(argv[1], "module") != NULL;
    printf("cycle bytes: %lu\n", (unsigned long) sizeof(ngx_cycle_t));
    assert(fflush(stdout) == 0);
    check_cycle();
    return 0;
}
