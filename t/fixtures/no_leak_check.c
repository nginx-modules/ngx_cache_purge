/* Leak checks need process facilities absent in some test environments. */
int
__lsan_is_turned_off(void)
{
    return 1;
}
