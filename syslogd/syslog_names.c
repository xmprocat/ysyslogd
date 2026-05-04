/*
 * Defines pointers to syslog prioritynames/facilitynames arrays.
 * These are exposed by glibc's <syslog.h> when SYSLOG_NAMES is defined.
 * We keep them in a single TU to avoid multiple definition errors.
 */
#include <stddef.h>
#define SYSLOG_NAMES
#define SYSLOG_NAMES_CONST
#include <syslog.h>

const CODE *const bb_prioritynames = prioritynames;
const CODE *const bb_facilitynames = facilitynames;
