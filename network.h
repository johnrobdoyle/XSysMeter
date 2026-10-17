#ifndef XSYSMETER_NETWORK_H
#define XSYSMETER_NETWORK_H
#include <stddef.h>
#include <stdint.h>
#define NETWORK_NAME_MAX 64
struct network_interface { char name[NETWORK_NAME_MAX]; int preferred; };
struct network_counters { uint64_t rx, tx; unsigned int index; };
struct network_rate {
    struct network_counters previous;
    double timestamp, rx, tx;
    int ready;
};
/* 1: rate available, 0: fresh baseline/reset, -1: invalid elapsed time. */
int network_rate_update(struct network_rate *, const struct network_counters *, double);
int network_interfaces(struct network_interface **, size_t *);
int network_read(const char *, struct network_counters *);
#endif
