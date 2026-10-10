XSysMeter for FreeBSD 14 - CPU, Temperature and Memory Tabs
======================
OpenMotif widgets with a double-buffered Xlib CPU history renderer.
Inspired by the supplied screenshot: white background, teal traces,
four graphs across. Each graph represents a logical CPU (hardware thread),
not necessarily a separate physical core. All CPUs are discovered at startup.

BUILD
  sudo pkg install open-motif libXt libX11 libXpm
  make
  ./xsysmeter
  sudo make install

Default installation: /opt/local/bin/xsysmeter
Override with: make install PREFIX=/your/prefix
Strict build: make clean && make CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Werror'
Standard Xt options work, e.g. ./xsysmeter -geometry 900x800

TABS
  CPU Usage: teal utilization graphs, scale 0-100%.
  CPU Temperature: orange temperature graphs, scale 0-120 C.
  Memory: one graph with four colored traces (Active, Wired, Inactive, Swap).
  All three pages retain independent 60-second histories and sample every second.
  Pause and Clear affect all three pages.

MEMORY ACCOUNTING
  Active, Wired and Inactive use vm.stats.vm page counts times page size.
  Current amounts are shown in MiB. Their percentage denominator is hw.physmem.
  Swap uses libkvm's live sysctl-backed swap summary, including all swap devices.
  Its percentage denominator is configured swap capacity, not physical RAM.
  No configured swap is shown as zero usage. Failed queries show N/A and gaps.
  Swap is not added to the RAM states: the four lines are independent readings.
  The three RAM states do not cover all physical memory (e.g. free/laundry).
  Link now includes -lkvm (part of the FreeBSD base system).
  The live kvm handle uses /dev/null; no elevated privilege is requested.

TEMPERATURE SENSORS
  Reads dev.cpu.N.temperature, converting integer tenths of kelvin to C.
  If sensors are absent, load the appropriate driver:
    sudo kldload coretemp      # Intel
    sudo kldload amdtemp       # AMD
  Verify: sysctl dev.cpu.0.temperature
  To load at boot, add coretemp_load="YES" or amdtemp_load="YES" to
  /boot/loader.conf as appropriate for your processor.
  The application retries sensors each second; loading a driver while it is
  running does not require a restart. Failed readings show N/A and leave gaps.
  CPU indices match the usage page. Hardware threads may share a core sensor;
  many AMD processors expose a shared package sensor, not per-core sensors.
  XSysMeter displays only readings actually supplied by dev.cpu.N.temperature.
  Temperature values above 120 C retain their label but clip at the graph top.
  Some AMD sensors report a thermal control value rather than die temperature.
  See coretemp(4) and amdtemp(4) on your system.

ICON AND ABOUT
  The supplied meter001.xpm is embedded in the executable via libXpm.
  No icon file is needed after installation. Its transparency mask is retained.
  The window manager decides where to display the icon.
  Toolbar order: Pause, Clear, About, Quit. About opens a reusable information
  dialog with an OK button.

CONTROLS
  Pause/Resume or Space: suspend/resume sampling.
  Clear or c: discard history.
  Quit, q, Escape, or window manager close: exit.

CPU ACCOUNTING
kern.cp_times contains CPUSTATES long counters per logical CPU.
Usage = 100 * (total delta - idle delta) / total delta.
User, nice, system and interrupt time count as busy. No root access needed.
The first read establishes a baseline. Subsequent reads occur every second.
61 sample positions cover 60 intervals; history fills from the right.
Resume establishes a fresh baseline to avoid averaging across the pause.
Counter resets are discarded for the affected CPU. Failed reads are shown
in the status line and cause a fresh baseline on recovery. CPU topology
changes require restarting the application.

Appearance can be adjusted using XSysMeter* X resources; graph colors are
currently set in xsysmeter.c. The processor name comes from hw.model.
Large CPU counts require a large/tall window; there is no scrolling.

VALIDATION
Source reviewed for C11, Xt callback types, Motif headers, resource types,
CPU delta accounting, circular history indexing and Xlib redraw lifecycle.
The delivery environment lacks FreeBSD and Motif/X11 development headers;
a native compilation and graphical runtime test could not be performed.
Run the strict build above on FreeBSD 14 and send any diagnostics.

Reference for the FreeBSD accounting interface:
https://freebsdfoundation.org/wp-content/uploads/2023/01/jones_activitymonitor.pdf
https://man.freebsd.org/cgi/man.cgi?query=coretemp&sektion=4&manpath=FreeBSD+14.3-RELEASE
https://man.freebsd.org/cgi/man.cgi?query=amdtemp&sektion=4&manpath=FreeBSD+14.3-RELEASE

MEMORY TRACE IDENTIFICATION
  Three-pixel strokes and black legend text improve readability.
  Active: solid blue, circle. Wired: dashed violet, square.
  Inactive: dotted green, triangle. Swap: dash-dot orange, diamond.
  Legend swatches match the graph; valid runs end with a marker.
  Missing readings remain gaps; all values retain their original scale.

STACKED MEMORY VIEW
  Filled RAM history and a current RAM composition bar replace the line view.
  Active: apricot; Wired: red; Inactive: pale gold; Other: lavender; Free: gray.
  Other is the nonnegative residual of physical RAM minus these four states;
  it includes unclassified memory and must not be called Linux buffers/cache.
  Swap remains a blue overlay using configured swap capacity as denominator.
  Missing RAM counters leave gaps; unavailable swap leaves no blue trace.
  RAM counter snapshots are not atomic. If their sum briefly exceeds capacity,
  displayed stack heights normalize to 100%; legend readings remain raw.
  Background remains #BFE1FF; text remains black. History remains 60 seconds.
