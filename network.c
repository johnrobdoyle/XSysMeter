#include "network.h"
#include <errno.h>
#include <math.h>

int network_rate_update(struct network_rate *rate,
                        const struct network_counters *now, double timestamp)
{
    double elapsed = timestamp - rate->timestamp;
    if (!isfinite(timestamp) || (rate->ready && elapsed <= 0)) {
        rate->rx = rate->tx = 0;
        errno = EINVAL;
        return -1;
    }
    if (!rate->ready || now->index != rate->previous.index ||
        now->rx < rate->previous.rx || now->tx < rate->previous.tx) {
        rate->previous = *now;
        rate->timestamp = timestamp;
        rate->ready = 1;
        rate->rx = rate->tx = 0;
        return 0;
    }
    rate->rx = (double)(now->rx - rate->previous.rx) / elapsed;
    rate->tx = (double)(now->tx - rate->previous.tx) / elapsed;
    rate->previous = *now;
    rate->timestamp = timestamp;
    return 1;
}

#ifndef NETWORK_RATE_TEST
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int network_interfaces(struct network_interface **result, size_t *count)
{
    struct ifaddrs *list, *item;
    struct network_interface *entries = NULL;
    size_t used = 0;
    if (getifaddrs(&list) == -1) return -1;
    for (item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_LINK ||
            !item->ifa_name) continue;
        size_t i;
        for (i = 0; i < used; ++i)
            if (!strcmp(entries[i].name, item->ifa_name)) break;
        if (i != used) continue;
        struct network_interface *grown = realloc(entries, (used + 1) * sizeof(*entries));
        if (!grown) { free(entries); freeifaddrs(list); return -1; }
        entries = grown;
        (void)snprintf(entries[used].name, sizeof(entries[used].name), "%s", item->ifa_name);
        entries[used].preferred = (item->ifa_flags & IFF_UP) && !(item->ifa_flags & IFF_LOOPBACK);
        ++used;
    }
    freeifaddrs(list);
    *result = entries;
    *count = used;
    return 0;
}

int network_read(const char *name, struct network_counters *out)
{
    struct ifaddrs *list, *item;
    if (getifaddrs(&list) == -1) return -1;
    int found = 0, error = ENXIO;
    for (item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_LINK ||
            !item->ifa_data || strcmp(name, item->ifa_name)) continue;
        if (!(item->ifa_flags & IFF_UP)) { error = ENETDOWN; break; }
        const struct if_data *data = (const struct if_data *)item->ifa_data;
        const struct sockaddr_dl *link = (const struct sockaddr_dl *)item->ifa_addr;
        out->rx = data->ifi_ibytes;
        out->tx = data->ifi_obytes;
        out->index = link->sdl_index;
        found = 1;
        break;
    }
    freeifaddrs(list);
    if (!found) { errno = error; return -1; }
    return 0;
}
#endif
