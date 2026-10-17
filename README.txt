XSysMeter - OpenMotif / Xlib system monitor for FreeBSD
======================================================

BUILD AND INSTALL
  sudo pkg install open-motif libXt libX11 libXpm
  make
  ./xsysmeter
  sudo make install
Default install: /opt/local/bin/xsysmeter
Override: make install PREFIX=/your/prefix
Standard Xt options: ./xsysmeter -geometry 900x800

TABS
  CPU Usage: logical CPU history from kern.cp_times counter deltas.
  CPU Temperature: dev.cpu.N.temperature, converted from tenths of kelvin.
    Missing sensors show N/A. Intel: sudo kldload coretemp
    AMD: sudo kldload amdtemp
    Threads may share a core sensor; AMD may report a shared package sensor.
  Memory: stacked Active/Wired/Inactive/Other/Free RAM history and RAM bar.
    Other is the nonnegative residual after those page states are subtracted
    from hw.physmem. It can include laundry and other unclassified memory.
    Swap is separate: the blue line uses configured swap as its denominator.
    Live libkvm queries need no /dev/mem access. Missing readings leave gaps.
    Snapshots are not atomic; a temporary sum over capacity is normalized
    for drawing only, while legend values remain the measured readings.
  Network Traffic: Receive (teal), Transmit (orange), Combined (blue).
    A light filled area shows combined traffic; scale follows the visible peak.
    Current, peak, and average rates are in B/s, KiB/s, MiB/s, or GiB/s.
    Peak and arithmetic average cover valid samples in the visible history.
    Rates use actual CLOCK_MONOTONIC elapsed time and byte counter deltas.

NETWORK INTERFACES
  The default is the first UP non-loopback interface returned by FreeBSD.
  Use Next interface to cycle through interfaces, including loopback.
  Refresh rediscovers interfaces after attaching/removing a device or tunnel.
  Switching or refreshing resets only network history and establishes a new
  baseline. The other three tabs keep their histories.
  A down/missing interface leaves gaps; recovery establishes a fresh baseline.
  Counter resets and changed interface indices do not cause artificial spikes.
  The selected interface's ifi_ibytes and ifi_obytes come from getifaddrs()
  AF_LINK records. It does not capture packets and requires no elevated access.
  Bridge, tunnel, and physical interfaces report their own counters. Traffic
  is never summed across interfaces, avoiding counting a packet at two layers.
  Numbers are interface traffic, not application payload throughput.

CONTROLS AND APPEARANCE
  Pause/Resume (Space): all four tabs; resume starts new CPU/network baselines.
  Clear (c): all histories. About: application information. Quit (q/Escape): exit.
  Supplied meter001.xpm is embedded in the executable with its mask.
  Background #BFE1FF; black text; original CPU, temperature and memory styling.
  History is 60 intervals (61 samples), sampled about once per second.
  Large CPU counts require a tall window; CPU pages have no scrolling.

VALIDATION
  make check
  make clean
  make CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Werror'
The portable network rate tests passed under strict C11 with warnings treated
as errors. Tests cover elapsed-time jitter, invalid time, counter reset,
interface replacement, resume baselines, and large cumulative counters.

