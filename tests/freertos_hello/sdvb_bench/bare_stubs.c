/*
 * No-op printf for the SDVB benchmark. The benchmark calls printf for status
 * and timing output, but during the polling-overhead sweep the workload must be
 * silent (any UART traffic would land inside the measured window). Defining our
 * own printf also keeps newlib's printf — and its malloc/locking/reentrancy
 * machinery — out of the link.
 */
int printf(const char *fmt, ...)
{
    (void) fmt;
    return 0;
}
