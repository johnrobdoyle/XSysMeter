#include "network.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    struct network_rate r = {0};
    struct network_counters c = {1000, 2000, 1};
    assert(network_rate_update(&r, &c, 10.0) == 0);
    c.rx += 4096; c.tx += 2048;
    assert(network_rate_update(&r, &c, 12.0) == 1);
    assert(r.rx == 2048 && r.tx == 1024);
    /* Jitter in the timer must not change rates for constant byte throughput. */
    c.rx += 1024; c.tx += 512;
    assert(network_rate_update(&r, &c, 12.5) == 1);
    assert(r.rx == 2048 && r.tx == 1024);
    assert(network_rate_update(&r, &c, 12.5) == -1);
    assert(network_rate_update(&r, &c, NAN) == -1);
    /* Counter reset and same-name interface recreation establish new baselines. */
    c.rx = 10; c.tx = 20;
    assert(network_rate_update(&r, &c, 13.0) == 0);
    c.index = 2; c.rx = 1000000; c.tx = 2000000;
    assert(network_rate_update(&r, &c, 14.0) == 0);
    c.rx += 800; c.tx += 400;
    assert(network_rate_update(&r, &c, 15.0) == 1);
    assert(r.rx == 800 && r.tx == 400);
    /* Paused or failed intervals are not averaged into the resumed reading. */
    r.ready = 0;
    c.rx += 999999;
    assert(network_rate_update(&r, &c, 100.0) == 0);
    assert(network_rate_update(&r, &c, 101.0) == 1);
    assert(r.rx == 0 && r.tx == 0);
    /* Large cumulative counters retain small increments before conversion. */
    r.ready = 0;
    c.rx = UINT64_C(1) << 60; c.tx = c.rx;
    assert(network_rate_update(&r, &c, 200.0) == 0);
    c.rx += 100; c.tx += 50;
    assert(network_rate_update(&r, &c, 201.0) == 1);
    assert(r.rx == 100 && r.tx == 50);
    puts("Network rate tests passed");
    return 0;
}
