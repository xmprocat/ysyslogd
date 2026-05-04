/*
 * Standalone syslogd entry point.
 * Adapted from busybox sysklogd/syslogd_and_logger.c
 */
#include "compat.h"

/* Forward declarations from syslogd.c */
extern int syslogd_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return syslogd_main(argc, argv);
}
