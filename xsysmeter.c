/* XSysMeter: OpenMotif controls, Xlib rendering, FreeBSD CPU accounting. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/dkstat.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/keysym.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/DrawingA.h>
#include <Xm/RowColumn.h>
#include <Xm/PushB.h>
#include <Xm/Protocols.h>
#include <Xm/Notebook.h>
#include <Xm/MessageB.h>
#include <Xm/Label.h>
#include "network.h"
#include <X11/Shell.h>
#include <X11/xpm.h>
#include "meter001.xpm"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <kvm.h>

#define HISTORY 61
#define NETWORK_INTERVAL_MS 100
#define NETWORK_HISTORY 601
#define COLUMNS 4
static XtAppContext context;
static Widget shell, canvases[4], pause_button;
static long *previous, *current;
static size_t cpu_count, tick_bytes, position, samples;
static double *history, *temperature;
static size_t temp_position, temp_samples;
static char temp_status[256] = "Collecting temperature samples";
static unsigned long orange, warm_fill;
static double memory_history[6 * HISTORY], memory_mib[6];
static size_t memory_position, memory_samples;
static char memory_status[256] = "Collecting memory samples";
static const char *memory_names[6] = {"Active", "Wired", "Inactive", "Swap", "Free", "Other"};
static unsigned long memory_colors[6], swap_outline;
static kvm_t *swap_handle;
static double ram_mib, swap_mib;
static Boolean swap_valid;

static char model[256] = "FreeBSD processor", status[256] = "Collecting CPU samples";
static Boolean paused = False, baseline = True;
static Display *display;
static GC gc;
static XFontStruct *font, *heading;
static unsigned long canvas_background, ink, teal, grid, fill;
static XtIntervalId timer, network_timer;
static Widget about_dialog;
static Pixmap icon_pixmap = None, icon_mask = None;

static struct network_interface *net_interfaces;
static size_t net_count, net_selected, net_position, net_samples;
static struct network_rate net_rate;
static double net_history[3 * NETWORK_HISTORY];
static unsigned long net_colors[3], net_fill;
static char net_status[256] = "Collecting network samples";
static Widget net_selector;

/* Drawing helper is shared by renderers defined before its implementation. */
static void text_at(Drawable target, int x, int y, const char *s,
                    XFontStruct *f, unsigned long pixel);

static void net_format(double value, char *out, size_t size)
{
    const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    size_t unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    (void)snprintf(out, size, "%.1f %s", value, units[unit]);
}

static void net_reset(void)
{
    memset(&net_rate, 0, sizeof(net_rate));
    net_position = net_samples = 0;
    for (size_t i = 0; i < 3 * NETWORK_HISTORY; ++i) net_history[i] = -1;
}

static void net_selector_label(void)
{
    if (!net_selector) return;
    XmString label = XmStringCreateLocalized(net_count ? net_interfaces[net_selected].name : "No interfaces");
    XtVaSetValues(net_selector, XmNlabelString, label, NULL);
    XmStringFree(label);
}

static void net_refresh(Widget w, XtPointer data, XtPointer call)
{
    struct network_interface *entries = NULL;
    size_t count = 0;
    char old[NETWORK_NAME_MAX] = "";
    (void)w; (void)data; (void)call;
    if (net_count) (void)snprintf(old, sizeof(old), "%s", net_interfaces[net_selected].name);
    if (network_interfaces(&entries, &count) == -1) {
        (void)snprintf(net_status, sizeof(net_status), "Interface discovery failed: %s", strerror(errno));
        return;
    }
    free(net_interfaces);
    net_interfaces = entries;
    net_count = count;
    net_selected = 0;
    for (size_t i = 0; i < count; ++i)
        if (entries[i].preferred) { net_selected = i; break; }
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(old, entries[i].name)) { net_selected = i; break; }
    net_reset();
    net_selector_label();
    (void)snprintf(net_status, sizeof(net_status), "%s", count ? "Collecting selected interface" : "No interfaces; use Refresh");
}

static void net_next(Widget w, XtPointer data, XtPointer call)
{
    (void)w; (void)data; (void)call;
    if (net_count) net_selected = (net_selected + 1) % net_count;
    net_reset();
    net_selector_label();
    (void)snprintf(net_status, sizeof(net_status), "Collecting selected interface");
}

static void sample_network(void)
{
    struct network_counters counters;
    struct timespec now;
    double rx = -1, tx = -1;
    if (!net_count) {
        (void)snprintf(net_status, sizeof(net_status), "No interfaces; use Refresh");
    } else if (network_read(net_interfaces[net_selected].name, &counters) == -1 ||
               clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        (void)snprintf(net_status, sizeof(net_status), "Interface unavailable: %s (use Refresh if removed)", strerror(errno));
        net_rate.ready = 0;
    } else {
        double timestamp = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
        int result = network_rate_update(&net_rate, &counters, timestamp);
        if (result == 1) {
            rx = net_rate.rx;
            tx = net_rate.tx;
            (void)snprintf(net_status, sizeof(net_status), "Interface %s  |  60-second history  |  10 samples/sec  |  bytes per second",
                net_interfaces[net_selected].name);
        } else {
            net_rate.ready = result < 0 ? 0 : net_rate.ready;
            (void)snprintf(net_status, sizeof(net_status), "Establishing interface counter baseline");
        }
    }
    net_history[net_position] = rx;
    net_history[NETWORK_HISTORY + net_position] = tx;
    net_history[2 * NETWORK_HISTORY + net_position] = rx < 0 || tx < 0 ? -1 : rx + tx;
    net_position = (net_position + 1) % NETWORK_HISTORY;
    if (net_samples < NETWORK_HISTORY) ++net_samples;
}

static void render_network(void)
{
    static const char *names[3] = {"Receive", "Transmit", "Combined"};
    Widget canvas = canvases[3];
    Dimension width, height;
    char label[256], formatted[40];
    if (!gc || !XtIsRealized(canvas) || !XtIsManaged(canvas)) return;
    XtVaGetValues(canvas, XmNwidth, &width, XmNheight, &height, NULL);
    if (width < 400 || height < 340) return;
    Pixmap back = XCreatePixmap(display, XtWindow(canvas), width, height,
        (unsigned int)DefaultDepth(display, DefaultScreen(display)));
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
    XSetForeground(display, gc, canvas_background);
    XFillRectangle(display, back, gc, 0, 0, width, height);
    text_at(back, 24, 45, "Network Traffic", heading, ink);
    (void)snprintf(label, sizeof(label), "Interface: %s", net_count ? net_interfaces[net_selected].name : "none");
    text_at(back, 24, 70, label, font, ink);
    double maximum = 1024;
    for (size_t j = 0; j < net_samples; ++j) {
        size_t index = (net_position + NETWORK_HISTORY - net_samples + j) % NETWORK_HISTORY;
        if (net_history[2 * NETWORK_HISTORY + index] > maximum) maximum = net_history[2 * NETWORK_HISTORY + index];
    }
    maximum *= 1.15;
    int left = 110, top = 100, plotw = (int)width - 140, ploth = (int)height - 260;
    /* Light filled area under combined traffic, as in the reference. */
    for (size_t j = 1; j < net_samples; ++j) {
        size_t first = (net_position + NETWORK_HISTORY - net_samples + j - 1) % NETWORK_HISTORY;
        double v1 = net_history[2 * NETWORK_HISTORY + first];
        double v2 = net_history[2 * NETWORK_HISTORY + (first + 1) % NETWORK_HISTORY];
        if (v1 < 0 || v2 < 0) continue;
        int x1 = left + (int)(NETWORK_HISTORY - net_samples + j - 1) * plotw / (NETWORK_HISTORY - 1);
        int x2 = left + (int)(NETWORK_HISTORY - net_samples + j) * plotw / (NETWORK_HISTORY - 1);
        XPoint points[4] = {
            {(short)x1, (short)(top + ploth)},
            {(short)x1, (short)(top + ploth - (int)(v1 * ploth / maximum))},
            {(short)x2, (short)(top + ploth - (int)(v2 * ploth / maximum))},
            {(short)x2, (short)(top + ploth)}
        };
        XSetForeground(display, gc, net_fill);
        XFillPolygon(display, back, gc, points, 4, Convex, CoordModeOrigin);
    }
    for (int j = 0; j <= 4; ++j) {
        int y = top + j * ploth / 4;
        XSetForeground(display, gc, grid);
        XDrawLine(display, back, gc, left, y, left + plotw, y);
        net_format(maximum * (4 - j) / 4, formatted, sizeof(formatted));
        text_at(back, 6, y + 4, formatted, font, ink);
    }
    XSetForeground(display, gc, grid);
    for (int j = 1; j < 6; ++j)
        XDrawLine(display, back, gc, left + j * plotw / 6, top, left + j * plotw / 6, top + ploth);
    for (size_t series = 0; series < 3; ++series) {
        double sum = 0, peak = 0, current_rate = -1;
        size_t count = 0;
        XPoint points[NETWORK_HISTORY];
        int used = 0;
        XSetForeground(display, gc, net_colors[series]);
        XSetLineAttributes(display, gc, 2, LineSolid, CapButt, JoinRound);
        for (size_t j = 0; j <= net_samples; ++j) {
            double value = -1;
            if (j < net_samples) {
                size_t index = (net_position + NETWORK_HISTORY - net_samples + j) % NETWORK_HISTORY;
                value = net_history[series * NETWORK_HISTORY + index];
                if (j + 1 == net_samples) current_rate = value;
            }
            if (value < 0) {
                if (used > 1) XDrawLines(display, back, gc, points, used, CoordModeOrigin);
                used = 0;
                continue;
            }
            sum += value; ++count;
            if (value > peak) peak = value;
            int x = left + (int)(NETWORK_HISTORY - net_samples + j) * plotw / (NETWORK_HISTORY - 1);
            int y = top + ploth - (int)(value * ploth / maximum);
            points[used++] = (XPoint){(short)x, (short)y};
            if (j % 50 == 0 || j + 1 == net_samples)
                XFillArc(display, back, gc, x - 2, y - 2, 4, 4, 0, 360 * 64);
        }
        XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
        int y = top + ploth + 55 + (int)series * 25;
        XSetForeground(display, gc, net_colors[series]);
        XFillRectangle(display, back, gc, 24, y - 10, 12, 12);
        char current_text[40], peak_text[40], mean_text[40];
        if (current_rate >= 0) net_format(current_rate, current_text, sizeof(current_text));
        else (void)snprintf(current_text, sizeof(current_text), "N/A");
        net_format(peak, peak_text, sizeof(peak_text));
        net_format(count ? sum / (double)count : 0, mean_text, sizeof(mean_text));
        (void)snprintf(label, sizeof(label), "%s: %s   Peak: %s   Avg: %s", names[series], current_text, peak_text, mean_text);
        text_at(back, 46, y, label, font, ink);
    }
    XSetForeground(display, gc, ink);
    XDrawRectangle(display, back, gc, left, top, (unsigned int)plotw, (unsigned int)ploth);
    text_at(back, left, top + ploth + 22, "60 seconds ago", font, ink);
    text_at(back, (int)width - 50, top + ploth + 22, "Now", font, ink);
    text_at(back, 24, (int)height - 15, paused ? "Paused" : net_status, font, ink);
    XCopyArea(display, back, XtWindow(canvas), gc, 0, 0, width, height, 0, 0);
    XFreePixmap(display, back);
}

static void sample_network_timer(XtPointer data, XtIntervalId *id)
{
    (void)data;
    (void)id;
    if (!paused) {
        sample_network();
        render_network();
    }
    network_timer = XtAppAddTimeOut(context, NETWORK_INTERVAL_MS,
        sample_network_timer, NULL);
}

static void fatal(const char *s)
{
    perror(s);
    exit(EXIT_FAILURE);
}

static unsigned long color(const char *name, unsigned long fallback)
{
    XColor exact, screen;
    if (XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)),
                         name, &screen, &exact))
        return screen.pixel;
    return fallback;
}

static void text_at(Drawable target, int x, int y, const char *s,
                    XFontStruct *f, unsigned long pixel)
{
    XSetForeground(display, gc, pixel);
    if (f != NULL)
        XSetFont(display, gc, f->fid);
    XDrawString(display, target, gc, x, y, s, (int)strlen(s));
}

/* Stack RAM states only. Swap has its own capacity and stays a separate line. */
static Boolean memory_stack(size_t index, double levels[6])
{
    static const size_t order[5] = {0, 1, 2, 5, 4};
    double sum = 0;
    levels[0] = 0;
    for (size_t i = 0; i < 5; ++i) {
        double value = memory_history[order[i] * HISTORY + index];
        if (value < 0) return False;
        sum += value;
        levels[i + 1] = sum;
    }
    /* Independently read counters can briefly exceed total RAM. */
    if (sum > 100)
        for (size_t i = 1; i <= 5; ++i) levels[i] *= 100.0 / sum;
    return True;
}

static void render_memory(void)
{
    static const size_t order[5] = {0, 1, 2, 5, 4};
    Widget canvas = canvases[2];
    Dimension width, height;
    char label[256];
    if (!gc || !XtIsRealized(canvas) || !XtIsManaged(canvas)) return;
    XtVaGetValues(canvas, XmNwidth, &width, XmNheight, &height, NULL);
    if (width < 400 || height < 360) return;
    Pixmap back = XCreatePixmap(display, XtWindow(canvas), width, height,
        (unsigned int)DefaultDepth(display, DefaultScreen(display)));
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
    XSetForeground(display, gc, canvas_background);
    XFillRectangle(display, back, gc, 0, 0, width, height);
    text_at(back, 24, 48, "Memory", heading, ink);
    (void)snprintf(label, sizeof(label), "Physical RAM %.2f GiB    Configured swap %.2f GiB",
        ram_mib / 1024.0, swap_mib / 1024.0);
    text_at(back, 24, 72, label, font, ink);
    int left = 60, top = 102, plotw = (int)width - 88, ploth = (int)height - 298;
    for (size_t j = 1; j < memory_samples; ++j) {
        size_t first = (memory_position + HISTORY - memory_samples + j - 1) % HISTORY;
        size_t second = (first + 1) % HISTORY;
        double low[6], high[6];
        if (!memory_stack(first, low) || !memory_stack(second, high)) continue;
        int x1 = left + (int)(HISTORY - memory_samples + j - 1) * plotw / (HISTORY - 1);
        int x2 = left + (int)(HISTORY - memory_samples + j) * plotw / (HISTORY - 1);
        for (size_t band = 0; band < 5; ++band) {
            XPoint points[4] = {
                {(short)x1, (short)(top + ploth - (int)(low[band] * ploth / 100))},
                {(short)x1, (short)(top + ploth - (int)(low[band + 1] * ploth / 100))},
                {(short)x2, (short)(top + ploth - (int)(high[band + 1] * ploth / 100))},
                {(short)x2, (short)(top + ploth - (int)(high[band] * ploth / 100))}
            };
            XSetForeground(display, gc, memory_colors[order[band]]);
            XFillPolygon(display, back, gc, points, 4, Convex, CoordModeOrigin);
            /* Fine dark boundaries make adjacent bands easier to distinguish. */
            XSetForeground(display, gc, ink);
            XDrawLine(display, back, gc, points[1].x, points[1].y, points[2].x, points[2].y);
        }
    }
    /* Draw a continuous grid over the filled memory bands. */
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
    for (int j = 0; j <= 4; ++j) {
        int y = top + j * ploth / 4;
        XSetForeground(display, gc, grid);
        XDrawLine(display, back, gc, left, y, left + plotw, y);
        (void)snprintf(label, sizeof(label), "%d%%", 100 - j * 25);
        text_at(back, 8, y + 4, label, font, ink);
    }
    XSetForeground(display, gc, grid);
    for (int j = 1; j < 6; ++j)
        XDrawLine(display, back, gc, left + j * plotw / 6, top, left + j * plotw / 6, top + ploth);
    /* Swap: white underlay and blue line remain visible over filled RAM bands. */
    for (int pass = 0; pass < 2; ++pass) {
        XSetLineAttributes(display, gc, pass ? 2 : 4, LineSolid, CapButt, JoinRound);
        XSetForeground(display, gc, pass ? memory_colors[3] : swap_outline);
        for (size_t j = 1; j < memory_samples; ++j) {
            size_t first = (memory_position + HISTORY - memory_samples + j - 1) % HISTORY;
            double v1 = memory_history[3 * HISTORY + first];
            double v2 = memory_history[3 * HISTORY + (first + 1) % HISTORY];
            if (v1 < 0 || v2 < 0) continue;
            if (v1 > 100) v1 = 100;
            if (v2 > 100) v2 = 100;
            int x1 = left + (int)(HISTORY - memory_samples + j - 1) * plotw / (HISTORY - 1);
            int x2 = left + (int)(HISTORY - memory_samples + j) * plotw / (HISTORY - 1);
            XDrawLine(display, back, gc, x1, top + ploth - (int)(v1 * ploth / 100),
                x2, top + ploth - (int)(v2 * ploth / 100));
        }
    }
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
    XSetForeground(display, gc, ink);
    XDrawRectangle(display, back, gc, left, top, (unsigned int)plotw, (unsigned int)ploth);
    text_at(back, left, top + ploth + 20, "60 seconds ago", font, ink);
    text_at(back, (int)width - 50, top + ploth + 20, "Now", font, ink);
    size_t latest = (memory_position + HISTORY - 1) % HISTORY;
    double levels[6];
    int bar_y = top + ploth + 43;
    text_at(back, 12, bar_y + 20, "RAM", font, ink);
    if (memory_samples && memory_stack(latest, levels)) {
        for (size_t band = 0; band < 5; ++band) {
            int x1 = left + (int)(levels[band] * plotw / 100);
            int x2 = left + (int)(levels[band + 1] * plotw / 100);
            XSetForeground(display, gc, memory_colors[order[band]]);
            XFillRectangle(display, back, gc, x1, bar_y, (unsigned int)(x2 - x1), 28);
        }
        double free_percent = memory_history[4 * HISTORY + latest];
        (void)snprintf(label, sizeof(label), "%.1f%% non-free     %.1f%% free", 100 - free_percent, free_percent);
    } else (void)snprintf(label, sizeof(label), "RAM composition unavailable");
    XSetForeground(display, gc, ink);
    XDrawRectangle(display, back, gc, left, bar_y, (unsigned int)plotw, 28);
    text_at(back, left, bar_y + 46, label, font, ink);
    for (size_t graph = 0; graph < 6; ++graph) {
        int x = 24 + (int)(graph % 2) * ((int)width - 48) / 2;
        int y = bar_y + 70 + (int)(graph / 2) * 22;
        double value = memory_samples ? memory_history[graph * HISTORY + latest] : -1;
        if (value < 0) (void)snprintf(label, sizeof(label), "%s: N/A", memory_names[graph]);
        else (void)snprintf(label, sizeof(label), "%s: %.2f GiB (%.1f%%)",
            memory_names[graph], memory_mib[graph] / 1024.0, value);
        XSetForeground(display, gc, memory_colors[graph]);
        XFillRectangle(display, back, gc, x, y - 11, 12, 12);
        XSetForeground(display, gc, ink);
        XDrawRectangle(display, back, gc, x, y - 11, 12, 12);
        text_at(back, x + 20, y, label, font, ink);
    }
    text_at(back, 24, (int)height - 14, "Stack: % of RAM. Blue line: swap usage / swap capacity.", font, ink);
    XCopyArea(display, back, XtWindow(canvas), gc, 0, 0, width, height, 0, 0);
    XFreePixmap(display, back);
}

static void render_page(int page)
{
    Widget canvas = canvases[page];
    double *values = page ? temperature : history;
    size_t count = page ? temp_samples : samples;
    size_t next = page ? temp_position : position;
    double maximum = page == 1 ? 120.0 : 100.0;
    size_t graph_count = cpu_count;
    unsigned long trace = page ? orange : teal;
    unsigned long area = page ? warm_fill : fill;
    Dimension width, height;
    Pixmap back;
    int columns, rows, cellw, cellh, plotw, ploth;
    char label[256];
    if (gc == NULL || !XtIsRealized(canvas) || !XtIsManaged(canvas))
        return;
    XtVaGetValues(canvas, XmNwidth, &width, XmNheight, &height, NULL);
    if (width < 100 || height < 150)
        return;
    columns = graph_count < COLUMNS ? (int)graph_count : COLUMNS;
    rows = (int)((graph_count + (size_t)columns - 1) / (size_t)columns);
    cellw = ((int)width - 48) / columns;
    cellh = ((int)height - 158) / rows;
    if (cellw < 40 || cellh < 40)
        return;
    back = XCreatePixmap(display, XtWindow(canvas), width, height,
                         (unsigned int)DefaultDepth(display, DefaultScreen(display)));
    XSetForeground(display, gc, canvas_background);
    XFillRectangle(display, back, gc, 0, 0, width, height);
    text_at(back, 24, 51, "CPU", heading, ink);
    /* Clip the processor name to the available header width. */
    int header_x = 125;
    XRectangle clip = {(short)header_x, 0, (unsigned short)(width > header_x + 24 ? width - header_x - 24 : 1), 65};
    XSetClipRectangles(display, gc, 0, 0, &clip, 1, Unsorted);
    text_at(back, header_x, 48, model, font, ink);
    XSetClipMask(display, gc, None);
    text_at(back, 24, 81, page == 1 ? "Temperature (C) over 60 seconds" : "% Utilization over 60 seconds", font, ink);
    text_at(back, (int)width - 62, 81, page == 1 ? "120 C" : "100%", font, ink);
    plotw = cellw - 12;
    ploth = cellh - 27;
    for (size_t cpu = 0; cpu < graph_count; ++cpu) {
        int x = 24 + (int)(cpu % (size_t)columns) * cellw;
        int y = 104 + (int)(cpu / (size_t)columns) * cellh;
        double latest = count ? values[cpu * HISTORY + (next + HISTORY - 1) % HISTORY] : -1;
        if (page == 1 && latest < 0)
            (void)snprintf(label, sizeof(label), "CPU %zu   N/A", cpu);
        else
            (void)snprintf(label, sizeof(label), page == 1 ? "CPU %zu   %.1f C" : "CPU %zu   %.0f%%",
                           cpu, latest < 0 ? 0 : latest);
        text_at(back, x, y - 5, label, font, trace);
        XSetForeground(display, gc, grid);
        for (int j = 1; j < 6; ++j) {
            XDrawLine(display, back, gc, x + j * plotw / 6, y,
                      x + j * plotw / 6, y + ploth);
            XDrawLine(display, back, gc, x, y + j * ploth / 6,
                      x + plotw, y + j * ploth / 6);
        }
        /* Draw each valid interval separately: missing sensors leave gaps. */
        for (size_t j = 1; j < count; ++j) {
            size_t first = (next + HISTORY - count + j - 1) % HISTORY;
            size_t second = (first + 1) % HISTORY;
            double v1 = values[cpu * HISTORY + first];
            double v2 = values[cpu * HISTORY + second];
            if (v1 < 0 || v2 < 0)
                continue;
            if (v1 > maximum) v1 = maximum;
            if (v2 > maximum) v2 = maximum;
            int x1 = x + (int)(HISTORY - count + j - 1) * plotw / (HISTORY - 1);
            int x2 = x + (int)(HISTORY - count + j) * plotw / (HISTORY - 1);
            int y1 = y + ploth - (int)(v1 * ploth / maximum);
            int y2 = y + ploth - (int)(v2 * ploth / maximum);
            XPoint polygon[4] = {
                {(short)x1, (short)(y + ploth)}, {(short)x1, (short)y1},
                {(short)x2, (short)y2}, {(short)x2, (short)(y + ploth)}
            };
            XSetForeground(display, gc, area);
            XFillPolygon(display, back, gc, polygon, 4, Convex, CoordModeOrigin);
            XSetForeground(display, gc, trace);
            XDrawLine(display, back, gc, x1, y1, x2, y2);
        }
        XSetForeground(display, gc, trace);
        XDrawRectangle(display, back, gc, x, y, (unsigned int)plotw, (unsigned int)ploth);
    }
    text_at(back, 24, (int)height - 32, page ? temp_status : status, font, ink);
    text_at(back, 24, (int)height - 12, "60 seconds ago                                             Now", font, ink);
    XCopyArea(display, back, XtWindow(canvas), gc, 0, 0, width, height, 0, 0);
    XFreePixmap(display, back);
}

static void render(void)
{
    render_page(0);
    render_page(1);
    render_memory();
    render_network();
}

static void sample_temperature(void)
{
    size_t available = 0;
    double hottest = -1;
    for (size_t cpu = 0; cpu < cpu_count; ++cpu) {
        char name[80];
        int raw = 0;
        size_t bytes = sizeof(raw);
        double celsius = -1;
        (void)snprintf(name, sizeof(name), "dev.cpu.%zu.temperature", cpu);
        /* FreeBSD's IK sysctl values are integer tenths of kelvin. */
        if (sysctlbyname(name, &raw, &bytes, NULL, 0) == 0 && bytes == sizeof(raw)) {
            double value = ((double)raw - 2731.5) / 10.0;
            if (value >= 0 && value <= 200) {
                celsius = value;
                ++available;
                if (value > hottest) hottest = value;
            }
        }
        temperature[cpu * HISTORY + temp_position] = celsius;
    }
    temp_position = (temp_position + 1) % HISTORY;
    if (temp_samples < HISTORY) ++temp_samples;
    if (available)
        (void)snprintf(temp_status, sizeof(temp_status),
            "Sensors %zu/%zu     Hottest %.1f C     Scale 0-120 C", available, cpu_count, hottest);
    else
        (void)snprintf(temp_status, sizeof(temp_status),
            "No CPU sensors available: load coretemp (Intel) or amdtemp (AMD)");
}

/* Read exact FreeBSD sysctl types; page counts are unsigned integers. */
static int read_uint(const char *name, unsigned int *value)
{
    size_t bytes = sizeof(*value);
    return sysctlbyname(name, value, &bytes, NULL, 0) == 0 && bytes == sizeof(*value);
}

static void sample_memory(void)
{
    static const char *nodes[3] = {
        "vm.stats.vm.v_active_count", "vm.stats.vm.v_wire_count",
        "vm.stats.vm.v_inactive_count"
    };
    unsigned int page_size = 0;
    unsigned long physical = 0;
    size_t bytes = sizeof(physical);
    Boolean ram_valid = read_uint("vm.stats.vm.v_page_size", &page_size) && page_size > 0;
    if (sysctlbyname("hw.physmem", &physical, &bytes, NULL, 0) != 0 ||
        bytes != sizeof(physical) || physical == 0)
        ram_valid = False;
    ram_mib = ram_valid ? (double)physical / 1048576.0 : 0;
    size_t available = 0;
    for (size_t graph = 0; graph < 3; ++graph) {
        unsigned int pages = 0;
        double percent = -1;
        memory_mib[graph] = 0;
        if (ram_valid && read_uint(nodes[graph], &pages)) {
            memory_mib[graph] = (double)pages * page_size / 1048576.0;
            percent = 100.0 * memory_mib[graph] / ram_mib;
            ++available;
        }
        memory_history[graph * HISTORY + memory_position] = percent;
    }
    unsigned int free_pages = 0;
    memory_mib[4] = memory_mib[5] = 0;
    memory_history[4 * HISTORY + memory_position] = -1;
    memory_history[5 * HISTORY + memory_position] = -1;
    if (ram_valid && available == 3 && read_uint("vm.stats.vm.v_free_count", &free_pages)) {
        memory_mib[4] = (double)free_pages * page_size / 1048576.0;
        double remainder = ram_mib - memory_mib[0] - memory_mib[1] - memory_mib[2] - memory_mib[4];
        memory_mib[5] = remainder > 0 ? remainder : 0;
        memory_history[4 * HISTORY + memory_position] = 100.0 * memory_mib[4] / ram_mib;
        memory_history[5 * HISTORY + memory_position] = 100.0 * memory_mib[5] / ram_mib;
    }
    /* A sysctl-only live kvm handle avoids opening /dev/mem or /dev/kmem. */
    if (!swap_handle) {
        char error[_POSIX2_LINE_MAX];
        swap_handle = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, error);
    }
    struct kvm_swap total;
    memset(&total, 0, sizeof(total));
    swap_valid = swap_handle && page_size > 0 &&
        kvm_getswapinfo(swap_handle, &total, 1, 0) >= 0;
    memory_mib[3] = swap_mib = 0;
    double percent = -1;
    if (swap_valid) {
        swap_mib = (double)total.ksw_total * page_size / 1048576.0;
        memory_mib[3] = (double)total.ksw_used * page_size / 1048576.0;
        percent = total.ksw_total ? 100.0 * (double)total.ksw_used / total.ksw_total : 0;
        ++available;
    } else if (swap_handle) {
        (void)kvm_close(swap_handle);
        swap_handle = NULL;
    }
    memory_history[3 * HISTORY + memory_position] = percent;
    memory_position = (memory_position + 1) % HISTORY;
    if (memory_samples < HISTORY) ++memory_samples;
    (void)snprintf(memory_status, sizeof(memory_status),
        "%zu/4 readings available. RAM: %% of physical memory. Swap: %% of configured swap.%s",
        available, swap_valid && swap_mib == 0 ? " No swap configured." : "");
}

static int read_ticks(void)
{
    size_t bytes = tick_bytes;
    if (sysctlbyname("kern.cp_times", current, &bytes, NULL, 0) == -1)
        return -1;
    if (bytes != tick_bytes) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

static void sample(XtPointer data, XtIntervalId *id)
{
    (void)data;
    (void)id;
    if (!paused) {
        sample_temperature();
        sample_memory();
        if (read_ticks() == -1) {
            (void)snprintf(status, sizeof(status), "CPU sample unavailable: %s", strerror(errno));
            baseline = True;
        } else if (baseline) {
            memcpy(previous, current, tick_bytes);
            baseline = False;
            (void)snprintf(status, sizeof(status), "Collecting: %zu logical CPUs", cpu_count);
        } else {
            double average = 0;
            for (size_t cpu = 0; cpu < cpu_count; ++cpu) {
                double total = 0, idle = 0;
                Boolean reset = False;
                for (size_t state = 0; state < CPUSTATES; ++state) {
                    size_t i = cpu * CPUSTATES + state;
                    if (current[i] < previous[i])
                        reset = True;
                    double delta = (double)current[i] - (double)previous[i];
                    total += delta;
                    if (state == CP_IDLE)
                        idle = delta;
                }
                double busy = !reset && total > 0 ? 100.0 * (total - idle) / total : 0;
                if (busy < 0) busy = 0;
                if (busy > 100) busy = 100;
                history[cpu * HISTORY + position] = busy;
                average += busy;
            }
            position = (position + 1) % HISTORY;
            if (samples < HISTORY) ++samples;
            memcpy(previous, current, tick_bytes);
            (void)snprintf(status, sizeof(status), "Utilization %.1f%%     Logical CPUs %zu     Sampling every second",
                           average / (double)cpu_count, cpu_count);
        }
    }
    render();
    timer = XtAppAddTimeOut(context, 1000, sample, NULL);
}

static void redraw(Widget w, XtPointer data, XtPointer call)
{
    (void)w; (void)data; (void)call;
    render();
}

static void quit(Widget w, XtPointer data, XtPointer call)
{
    (void)w; (void)data; (void)call;
    XtAppSetExitFlag(context);
}

static void show_about(Widget w, XtPointer data, XtPointer call)
{
    (void)w; (void)data; (void)call;
    if (!about_dialog) {
        XmString title = XmStringCreateLocalized("About XSysMeter");
        XmString message = XmStringCreateLocalized(
            "XSysMeter 1.0\n"
            "CPU, Memory and Network Monitor for FreeBSD\n\n"
            "OpenMotif controls and Xlib history graphs.\n"
            "CPU Usage: per-logical-CPU utilization.\n"
            "CPU Temperature: available hardware sensors.\n"
            "Memory: Active, Wired, Inactive and Swap.\n"
            "Network: interface receive, transmit and combined rates.\n\n"
            "History: approximately 60 seconds.\n"
            "Network: 10 samples per second (100 ms).\n"
            "CPU, memory and temperature: once per second.\n"
            "Temperature sensors may be shared by cores or packages.");
        Arg args[3];
        Cardinal n = 0;
        XtSetArg(args[n], XmNdialogTitle, title); ++n;
        XtSetArg(args[n], XmNmessageString, message); ++n;
        XtSetArg(args[n], XmNautoUnmanage, True); ++n;
        about_dialog = XmCreateInformationDialog(shell, "about", args, n);
        XmStringFree(title);
        XmStringFree(message);
        XtUnmanageChild(XmMessageBoxGetChild(about_dialog, XmDIALOG_CANCEL_BUTTON));
        XtUnmanageChild(XmMessageBoxGetChild(about_dialog, XmDIALOG_HELP_BUTTON));
    }
    XtManageChild(about_dialog);
    XtPopup(XtParent(about_dialog), XtGrabNone);
}

static void set_window_icon(void)
{
    int result = XpmCreatePixmapFromData(display,
        RootWindowOfScreen(XtScreen(shell)), meter_xpm,
        &icon_pixmap, &icon_mask, NULL);
    if (result == XpmSuccess) {
        XtVaSetValues(shell, XtNiconPixmap, icon_pixmap,
            XtNiconMask, icon_mask, XtNiconName, "XSysMeter", NULL);
    } else {
        fprintf(stderr, "XSysMeter: cannot create icon: %s\n", XpmGetErrorString(result));
    }
}

static void toggle_pause(Widget w, XtPointer data, XtPointer call)
{
    XmString s;
    (void)w; (void)data; (void)call;
    paused = !paused;
    net_rate.ready = 0;
    baseline = True;
    s = XmStringCreateLocalized(paused ? "Resume" : "Pause");
    XtVaSetValues(pause_button, XmNlabelString, s, NULL);
    XmStringFree(s);
    (void)snprintf(status, sizeof(status), "%s", paused ? "Paused" : "Resuming CPU sampling");
    (void)snprintf(temp_status, sizeof(temp_status), "%s", paused ? "Paused" : "Resuming temperature sampling");
    (void)snprintf(memory_status, sizeof(memory_status), "%s", paused ? "Paused" : "Resuming memory sampling");
    render();
}

static void clear_history(Widget w, XtPointer data, XtPointer call)
{
    (void)w; (void)data; (void)call;
    net_reset();
    memset(history, 0, cpu_count * HISTORY * sizeof(*history));
    for (size_t i = 0; i < cpu_count * HISTORY; ++i) temperature[i] = -1;
    for (size_t i = 0; i < 6 * HISTORY; ++i) memory_history[i] = -1;
    memory_position = memory_samples = 0;
    temp_position = temp_samples = 0;
    position = samples = 0;
    baseline = True;
    render();
}

static void keys(Widget w, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)data; (void)dispatch;
    if (event->type == KeyPress) {
        KeySym key = XLookupKeysym(&event->xkey, 0);
        if (key == XK_q || key == XK_Q || key == XK_Escape)
            quit(w, NULL, NULL);
        else if (key == XK_space)
            toggle_pause(w, NULL, NULL);
        else if (key == XK_c || key == XK_C)
            clear_history(w, NULL, NULL);
    }
}

int main(int argc, char **argv)
{
    Widget form, toolbar, clear, about, exit_button, notebook;
    Atom wm_delete;
    size_t length = sizeof(model);
    String resources[] = {
        "*background: #a9bfd8", "*foreground: #000000",
        "*fontList: fixed", "*topShadowColor: #eef7ff", "*bottomShadowColor: #476b8f", "*usage.background: #bfe1ff",
        "*temperature.background: #bfe1ff", "*memory.background: #bfe1ff", NULL
    };
    if (sysctlbyname("kern.cp_times", NULL, &tick_bytes, NULL, 0) == -1)
        fatal("kern.cp_times");
    if (tick_bytes == 0 || tick_bytes % (CPUSTATES * sizeof(long)) != 0) {
        fputs("Unexpected kern.cp_times size\n", stderr);
        return EXIT_FAILURE;
    }
    cpu_count = tick_bytes / (CPUSTATES * sizeof(long));
    /* Keep X11 coordinates and initial Motif dimensions within their limits. */
    if (cpu_count > 1024) {
        fputs("XSysMeter supports up to 1024 logical CPUs\n", stderr);
        return EXIT_FAILURE;
    }
    previous = calloc(1, tick_bytes);
    current = calloc(1, tick_bytes);
    history = calloc(cpu_count * HISTORY, sizeof(*history));
    temperature = malloc(cpu_count * HISTORY * sizeof(*temperature));
    if (!previous || !current || !history || !temperature) fatal("calloc");
    for (size_t i = 0; i < cpu_count * HISTORY; ++i) temperature[i] = -1;
    if (sysctlbyname("hw.model", model, &length, NULL, 0) == -1)
        (void)snprintf(model, sizeof(model), "FreeBSD processor");
    for (size_t i = 0; i < 6 * HISTORY; ++i) memory_history[i] = -1;
    model[sizeof(model) - 1] = '\0';
    shell = XtVaAppInitialize(&context, "XSysMeter", NULL, 0, &argc, argv,
                             resources, XmNtitle, "XSysMeter", NULL);
    if (argc != 1) {
        fputs("Usage: xsysmeter [standard Xt options, e.g. -display or -geometry]\n", stderr);
        return EXIT_FAILURE;
    }
    form = XtVaCreateManagedWidget("form", xmFormWidgetClass, shell, NULL);
    toolbar = XtVaCreateManagedWidget("toolbar", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
        XmNbottomAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    pause_button = XtVaCreateManagedWidget("Pause", xmPushButtonWidgetClass, toolbar, NULL);
    clear = XtVaCreateManagedWidget("Clear", xmPushButtonWidgetClass, toolbar, NULL);
    about = XtVaCreateManagedWidget("About", xmPushButtonWidgetClass, toolbar, NULL);
    exit_button = XtVaCreateManagedWidget("Quit", xmPushButtonWidgetClass, toolbar, NULL);
    XtAddCallback(pause_button, XmNactivateCallback, toggle_pause, NULL);
    XtAddCallback(clear, XmNactivateCallback, clear_history, NULL);
    XtAddCallback(about, XmNactivateCallback, show_about, NULL);
    XtAddCallback(exit_button, XmNactivateCallback, quit, NULL);
    size_t rows = (cpu_count + COLUMNS - 1) / COLUMNS;
    if (rows < 2) rows = 2;
    notebook = XtVaCreateManagedWidget("notebook", xmNotebookWidgetClass, form,
        XmNbindingType, XmNONE, XmNbackPageSize, 0,
        XmNorientation, XmHORIZONTAL, XmNcurrentPageNumber, 1,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, toolbar, NULL);
    for (int page = 0; page < 4; ++page) {
        if (page == 3) {
            Widget network_page = XtVaCreateManagedWidget("networkPage", xmFormWidgetClass, notebook,
                XmNnotebookChildType, XmPAGE, XmNpageNumber, 4, NULL);
            Widget controls = XtVaCreateManagedWidget("networkControls", xmRowColumnWidgetClass, network_page,
                XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
                XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, NULL);
            net_selector = XtVaCreateManagedWidget("Interface", xmLabelWidgetClass, controls, NULL);
            Widget next = XtVaCreateManagedWidget("Next interface", xmPushButtonWidgetClass, controls, NULL);
            Widget refresh = XtVaCreateManagedWidget("Refresh", xmPushButtonWidgetClass, controls, NULL);
            XtAddCallback(next, XmNactivateCallback, net_next, NULL);
            XtAddCallback(refresh, XmNactivateCallback, net_refresh, NULL);
            canvases[page] = XtVaCreateManagedWidget("network", xmDrawingAreaWidgetClass, network_page,
                XmNwidth, (Dimension)760, XmNheight, (Dimension)(158 + rows * 140),
                XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, controls,
                XmNbottomAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, NULL);
        } else {
            canvases[page] = XtVaCreateManagedWidget(page == 2 ? "memory" : page == 1 ? "temperature" : "usage",
                xmDrawingAreaWidgetClass, notebook,
                XmNnotebookChildType, XmPAGE, XmNpageNumber, page + 1,
                XmNwidth, (Dimension)760, XmNheight, (Dimension)(158 + rows * 140), NULL);
        }
        (void)XtVaCreateManagedWidget(page == 3 ? "Network Traffic" : page == 2 ? "Memory" : page == 1 ? "CPU Temperature" : "CPU Usage",
            xmPushButtonWidgetClass, notebook,
            XmNnotebookChildType, XmMAJOR_TAB, XmNpageNumber, page + 1, NULL);
        XtAddCallback(canvases[page], XmNexposeCallback, redraw, NULL);
        XtAddCallback(canvases[page], XmNresizeCallback, redraw, NULL);
        XtAddEventHandler(canvases[page], KeyPressMask, False, keys, NULL);
    }
    XtAddEventHandler(shell, KeyPressMask, False, keys, NULL);
    XtRealizeWidget(shell);
    display = XtDisplay(shell);
    set_window_icon();
    canvas_background = color("#bfe1ff", WhitePixel(display, DefaultScreen(display)));
    ink = color("#000000", BlackPixel(display, DefaultScreen(display)));
    teal = color("#0052a3", ink);
    grid = color("#7da3c4", canvas_background);
    fill = color("#84bcea", canvas_background);
    orange = color("#a53b00", ink);
    warm_fill = color("#efbc86", canvas_background);
    memory_colors[0] = color("#f3ad70", ink);
    memory_colors[1] = color("#b92920", ink);
    memory_colors[2] = color("#ead68c", ink);
    memory_colors[3] = color("#245eaa", ink);
    swap_outline = color("#ffffff", canvas_background);
    memory_colors[4] = color("#e8edf2", ink);
    memory_colors[5] = color("#a799c9", ink);
    for (int page = 0; page < 4; ++page)
        XtVaSetValues(canvases[page], XmNbackground, canvas_background,
                      XmNforeground, ink, NULL);
    net_colors[0] = color("#007f8b", ink);
    net_colors[1] = color("#a53b00", ink);
    net_colors[2] = color("#0052a3", ink);
    net_fill = color("#91bfe3", canvas_background);
    net_refresh(NULL, NULL, NULL);
    gc = XCreateGC(display, XtWindow(canvases[0]), 0, NULL);
    font = XLoadQueryFont(display, "-*-helvetica-medium-r-normal--14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(display, "fixed");
    heading = XLoadQueryFont(display, "-*-helvetica-medium-r-normal--34-*-*-*-*-*-*-*");
    if (!heading) heading = XLoadQueryFont(display, "9x15bold");
    if (!heading) heading = XLoadQueryFont(display, "fixed");
    wm_delete = XmInternAtom(display, "WM_DELETE_WINDOW", False);
    XtVaSetValues(shell, XmNdeleteResponse, XmDO_NOTHING, NULL);
    XmAddWMProtocolCallback(shell, wm_delete, quit, NULL);
    sample(NULL, NULL);
    sample_network_timer(NULL, NULL);
    XtAppMainLoop(context);
    XtRemoveTimeOut(timer);
    XtRemoveTimeOut(network_timer);
    XFreeGC(display, gc);
    if (font) XFreeFont(display, font);
    if (heading) XFreeFont(display, heading);
    if (swap_handle) (void)kvm_close(swap_handle);
    XtDestroyWidget(shell);
    if (icon_pixmap != None) XFreePixmap(display, icon_pixmap);
    if (icon_mask != None) XFreePixmap(display, icon_mask);
    XtDestroyApplicationContext(context);
    free(net_interfaces);
    free(previous); free(current); free(history); free(temperature);
    return EXIT_SUCCESS;
}
