/* SPDX-License-Identifier: MIT
 * cc -std=c11 -O2 -Wall -Wextra -Werror -pthread mouse-latency.c -lX11 -o mouse-latency
 * Measures virtual evdev input to observed cursor-register programming.
 * It does NOT measure monitor photons or verify the rendered cursor pixels.
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ABS_MAX_X 1919
#define ABS_MAX_Y 1079
#define MAX_POINTS 200
#define MAX_SAMPLES 100000
#define MAX_EVENTS 8192
#define MATCH_TOLERANCE 2
#define NS_PER_MS UINT64_C(1000000)

struct hw_position {
    int x, y, hotspot_x, hotspot_y, effective_x, effective_y;
    unsigned format;
};
struct hw_sample { uint64_t ns; struct hw_position pos; };
struct kernel_sample { uint64_t ns; int x, y; };
struct point {
    uint64_t submit_ns, submitted_ns, kernel_ns, matched_ns;
    int x, y, logical_x, logical_y, hw_x, hw_y;
};
struct calibration {
    int raw_x, raw_y, logical_x, logical_y;
    struct hw_position hw;
};
struct lab {
    Display *display;
    int ui, event, mem, pipe, width, height, saved_x, saved_y;
    bool created, pointer_saved, calibrated, thread_started;
    void *mapping;
    size_t mapping_size;
    volatile const unsigned char *regs;
    char input_name[128], event_path[PATH_MAX];
    struct calibration cal[2];
    double scale_x, scale_y, offset_x, offset_y, hotspot_x, hotspot_y;
    pthread_t thread;
    atomic_bool stop;
    struct hw_sample *samples;
    struct kernel_sample *events;
    size_t sample_count, event_count;
    unsigned overflow, syn_dropped;
    int event_x, event_y, sampler_error;
};
static volatile sig_atomic_t interrupted;
static struct lab *x_error_lab;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void sleep_until(uint64_t ns)
{
    struct timespec t = { (time_t)(ns / UINT64_C(1000000000)),
                          (long)(ns % UINT64_C(1000000000)) };
    int ret;
    do { ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); }
    while (ret == EINTR && !interrupted);
}
static void sleep_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) { }
}
static void signal_flag(int signo) { (void)signo; interrupted = 1; }
static int error(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    return -1;
}
static int x_io_error(Display *display)
{
    (void)display;
    /* Xlib invokes this in the querying thread, not in a signal handler. */
    if (x_error_lab && x_error_lab->created)
        ioctl(x_error_lab->ui, UI_DEV_DESTROY);
    fprintf(stderr, "X connection lost; virtual device destroyed; pointer restoration unavailable\n");
    _Exit(1);
}
static int rounded(double value)
{
    return (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
}
static int clamp(int value, int max)
{
    return value < 0 ? 0 : value > max ? max : value;
}
static int normalized(int extent, double fraction)
{
    return clamp(rounded((extent - 1) * fraction), extent - 1);
}
static struct calibration outer_point(struct lab *lab, double x, double y)
{
    /* Before calibration, scale desired logical coordinates into the fixed
     * uinput ABS range. The two measurements then establish the real map. */
    return (struct calibration) {
        .raw_x = clamp(rounded((double)normalized(lab->width, x) *
                               ABS_MAX_X / (lab->width - 1)), ABS_MAX_X),
        .raw_y = clamp(rounded((double)normalized(lab->height, y) *
                               ABS_MAX_Y / (lab->height - 1)), ABS_MAX_Y),
    };
}
static int query_pointer(struct lab *lab, int *x, int *y)
{
    Window root, child;
    int wx, wy;
    unsigned mask;
    if (!XQueryPointer(lab->display, DefaultRootWindow(lab->display),
                       &root, &child, x, y, &wx, &wy, &mask)) {
        fprintf(stderr, "XQueryPointer: pointer is on another X screen\n");
        return -1;
    }
    return 0;
}
static uint32_t reg32(struct lab *lab, unsigned offset)
{
    return *(volatile const uint32_t *)(lab->regs + offset);
}
static struct hw_position read_hw(struct lab *lab)
{
    unsigned pos_reg = lab->pipe ? 0x1690U : 0x1540U;
    unsigned cfg_reg = lab->pipe ? 0x1670U : 0x1520U;
    uint32_t pos = reg32(lab, pos_reg), cfg = reg32(lab, cfg_reg);
    uint32_t again = reg32(lab, pos_reg);
    if (again != pos) { pos = again; cfg = reg32(lab, cfg_reg); }
    struct hw_position hw = {
        .x = (int)(pos & 0xfff), .y = (int)((pos >> 16) & 0xfff),
        .hotspot_x = (int)((cfg >> 16) & 0x3f),
        .hotspot_y = (int)((cfg >> 8) & 0x3f), .format = cfg & 3U,
    };
    hw.effective_x = hw.x - hw.hotspot_x;
    hw.effective_y = hw.y - hw.hotspot_y;
    return hw;
}
static int read_number(const char *path, unsigned long *value)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = fscanf(f, "%lx", value);
    fclose(f);
    if (n != 1) { errno = EINVAL; return -1; }
    return 0;
}
static int map_registers(struct lab *lab, const char *pci, int requested_pipe)
{
    char path[PATH_MAX];
    unsigned long vendor, device;
    unsigned long long start, end, flags;
    long page = sysconf(_SC_PAGESIZE);
    snprintf(path, sizeof(path), "%s/vendor", pci);
    if (read_number(path, &vendor)) return error("PCI vendor");
    snprintf(path, sizeof(path), "%s/device", pci);
    if (read_number(path, &device)) return error("PCI device");
    if (vendor != 0x0014 || device != 0x7a36 || page <= 0) {
        fprintf(stderr, "Register reader requires the 0014:7a36 DC\n"); return -1;
    }
    snprintf(path, sizeof(path), "%s/resource", pci);
    FILE *f = fopen(path, "r");
    if (!f) return error(path);
    int n = fscanf(f, "%llx %llx %llx", &start, &end, &flags);
    fclose(f);
    if (n != 3 || end < start || end - start < 0x1ba3U || !(flags & 0x200U) ||
        start > (unsigned long long)LLONG_MAX) {
        fprintf(stderr, "Invalid DC BAR0 resource\n"); return -1;
    }
    uint64_t map_base = (uint64_t)start & ~((uint64_t)page - 1);
    size_t offset = (size_t)((uint64_t)start - map_base);
    lab->mapping_size = ((offset + 0x1ba4U + (size_t)page - 1) / (size_t)page) * (size_t)page;
    lab->mem = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (lab->mem < 0) return error("/dev/mem O_RDONLY");
    lab->mapping = mmap(NULL, lab->mapping_size, PROT_READ, MAP_SHARED,
                        lab->mem, (off_t)map_base);
    if (lab->mapping == MAP_FAILED) { lab->mapping = NULL; return error("MMIO read-only mmap"); }
    lab->regs = (volatile const unsigned char *)lab->mapping + offset;
    unsigned hpd = reg32(lab, 0x1ba0);
    if (requested_pipe >= 0) lab->pipe = requested_pipe;
    else {
        int best = -1;
        for (int i = 0; i < 2; ++i) {
            unsigned cfg = reg32(lab, i ? 0x1250 : 0x1240);
            int score = ((cfg & (1U << 8)) ? 4 : 0) + ((hpd & (1U << i)) ? 2 : 0);
            if (reg32(lab, i ? 0x1670 : 0x1520) & 3U) ++score;
            if (score > best) { best = score; lab->pipe = i; }
        }
    }
    if (!(reg32(lab, lab->pipe ? 0x1250 : 0x1240) & (1U << 8))) {
        fprintf(stderr, "Selected pipe has no active scanout\n"); return -1;
    }
    printf("# bar0=0x%llx access=O_RDONLY|O_SYNC/PROT_READ pipe=%d hpd=0x%x\n", start, lab->pipe, hpd);
    return 0;
}
static int emit_absolute(struct lab *lab, int x, int y)
{
    struct input_event events[3] = {
        { .type = EV_ABS, .code = ABS_X, .value = x },
        { .type = EV_ABS, .code = ABS_Y, .value = y },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
    };
    ssize_t n;
    do { n = write(lab->ui, events, sizeof(events)); } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(events)) {
        if (n >= 0) errno = EIO;
        return error("uinput event write");
    }
    return 0;
}
static int find_event(struct lab *lab)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s", lab->input_name);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *entry;
    int ret = -1;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5)) continue;
        int len = snprintf(lab->event_path, sizeof(lab->event_path), "/dev/input/%s", entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(lab->event_path)) break;
        lab->event = open(lab->event_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (lab->event >= 0) { ret = 0; break; }
    }
    closedir(dir);
    return ret;
}
static int create_mouse(struct lab *lab)
{
    struct uinput_setup setup = {0};
    struct uinput_abs_setup abs_setup = {0};
    lab->ui = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (lab->ui < 0) return error("/dev/uinput");
    if (ioctl(lab->ui, UI_SET_EVBIT, EV_ABS) ||
        ioctl(lab->ui, UI_SET_ABSBIT, ABS_X) || ioctl(lab->ui, UI_SET_ABSBIT, ABS_Y) ||
        ioctl(lab->ui, UI_SET_EVBIT, EV_KEY) || ioctl(lab->ui, UI_SET_KEYBIT, BTN_LEFT) ||
        ioctl(lab->ui, UI_SET_KEYBIT, BTN_RIGHT) ||
        ioctl(lab->ui, UI_SET_PROPBIT, INPUT_PROP_POINTER)) return error("uinput capabilities");
    abs_setup.code = ABS_X; abs_setup.absinfo.maximum = ABS_MAX_X;
    if (ioctl(lab->ui, UI_ABS_SETUP, &abs_setup)) return error("ABS_X setup");
    abs_setup.code = ABS_Y; abs_setup.absinfo.maximum = ABS_MAX_Y;
    if (ioctl(lab->ui, UI_ABS_SETUP, &abs_setup)) return error("ABS_Y setup");
    setup.id.bustype = BUS_USB; setup.id.vendor = 0xfeed; setup.id.product = 0x7a36;
    snprintf(setup.name, sizeof(setup.name), "graphic-lab-absolute-latency");
    if (ioctl(lab->ui, UI_DEV_SETUP, &setup) || ioctl(lab->ui, UI_DEV_CREATE)) return error("UI_DEV_CREATE");
    lab->created = true;
    if (ioctl(lab->ui, UI_GET_SYSNAME(sizeof(lab->input_name)), lab->input_name) < 0)
        return error("UI_GET_SYSNAME");
    uint64_t deadline = now_ns() + 2000 * NS_PER_MS;
    while (!interrupted && find_event(lab)) {
        if (now_ns() >= deadline) return error("virtual evdev node enumeration");
        sleep_ms(20);
    }
    if (interrupted) return -1;
    int clock_id = CLOCK_MONOTONIC;
    if (ioctl(lab->event, EVIOCSCLOCKID, &clock_id)) return error("evdev monotonic clock");
    printf("# virtual_input=%s event=%s; waiting for udev/libinput enumeration\n",
           lab->input_name, lab->event_path);
    sleep_ms(1500);
    return interrupted ? -1 : 0;
}
static int sample_calibration(struct lab *lab, struct calibration *c)
{
    if (query_pointer(lab, &c->logical_x, &c->logical_y)) return -1;
    struct hw_position first = read_hw(lab);
    sleep_ms(10);
    c->hw = read_hw(lab);
    if (interrupted) return -1;
    /* A hidden or old cursor is not a calibration point. The selected
     * backend has a 64x64 cursor, so its logical hotspot must be in it. */
    int dx = c->logical_x - c->hw.effective_x;
    int dy = c->logical_y - c->hw.effective_y;
    return first.format && first.format == c->hw.format &&
           first.x == c->hw.x && first.y == c->hw.y &&
           first.hotspot_x == c->hw.hotspot_x && first.hotspot_y == c->hw.hotspot_y &&
           dx >= -2 && dx <= 65 && dy >= -2 && dy <= 65;
}
static int calibrate(struct lab *lab)
{
    bool have_offset = false;
    int reference_x = 0, reference_y = 0;
    /* Device hotplug can hide the glyph on the first event. Warm up before
     * recording the two calibration points; no timed trajectory starts yet. */
    for (int i = 0; i < 2; ++i) {
        struct calibration warm = outer_point(lab, i ? 0.85 : 0.12,
                                               i ? 0.92 : 0.90);
        if (interrupted || emit_absolute(lab, warm.raw_x, warm.raw_y)) return -1;
        sleep_ms(250);
        int settled = sample_calibration(lab, &warm);
        if (settled < 0) return -1;
        printf("# warmup%d logical=%d,%d hw_origin=%d,%d format=%u settled=%d\n",
               i, warm.logical_x, warm.logical_y, warm.hw.effective_x,
               warm.hw.effective_y, warm.hw.format, settled);
        if (settled) {
            reference_x = warm.logical_x - warm.hw.effective_x;
            reference_y = warm.logical_y - warm.hw.effective_y;
            have_offset = true;
        }
    }
    /* These are (100,850) and (1800,1000) on the 1920x1080 lab screen,
     * outside the central greeter controls. A/B runs must use the same UI. */
    lab->cal[0] = outer_point(lab, 100.0 / 1919.0, 850.0 / 1079.0);
    lab->cal[1] = outer_point(lab, 1800.0 / 1919.0, 1000.0 / 1079.0);
    for (int i = 0; i < 2; ++i) {
        struct calibration *c = &lab->cal[i];
        if (interrupted || emit_absolute(lab, c->raw_x, c->raw_y)) return -1;
        bool accepted = false;
        for (int attempt = 0; attempt < 3 && !interrupted; ++attempt) {
            sleep_ms(250);
            int settled = sample_calibration(lab, c);
            if (settled < 0) return -1;
            int dx = c->logical_x - c->hw.effective_x;
            int dy = c->logical_y - c->hw.effective_y;
            accepted = settled && (!have_offset ||
                       (abs(dx - reference_x) <= 3 && abs(dy - reference_y) <= 3));
            printf("# calibration%d attempt=%d raw=%d,%d logical=%d,%d hw_origin=%d,%d hw_hotspot=%d,%d format=%u settled=%d accepted=%d\n",
                   i, attempt + 1, c->raw_x, c->raw_y, c->logical_x, c->logical_y,
                   c->hw.effective_x, c->hw.effective_y, c->hw.hotspot_x,
                   c->hw.hotspot_y, c->hw.format, settled, accepted);
            if (accepted) {
                reference_x = dx; reference_y = dy; have_offset = true;
                break;
            }
        }
        if (!accepted) {
            fprintf(stderr, "Calibration point %d did not settle within three attempts; no trajectory sent\n", i);
            return -1;
        }
    }
    struct calibration *a = &lab->cal[0], *b = &lab->cal[1];
    if (!a->hw.format || !b->hw.format || b->logical_x - a->logical_x < lab->width / 2 ||
        b->logical_y - a->logical_y < lab->height / 16 ||
        b->logical_y <= a->logical_y || b->raw_x <= a->raw_x || b->raw_y <= a->raw_y ||
        abs((a->logical_x - a->hw.effective_x) - (b->logical_x - b->hw.effective_x)) > 3 ||
        abs((a->logical_y - a->hw.effective_y) - (b->logical_y - b->hw.effective_y)) > 3) {
        fprintf(stderr, "ABS mouse calibration failed: device may not be recognized, pipe may differ, or hardware has not settled. No trajectory sent; consider a virtual REL mouse separately.\n");
        return -1;
    }
    lab->scale_x = (double)(b->logical_x - a->logical_x) / (b->raw_x - a->raw_x);
    lab->scale_y = (double)(b->logical_y - a->logical_y) / (b->raw_y - a->raw_y);
    lab->offset_x = a->logical_x - lab->scale_x * a->raw_x;
    lab->offset_y = a->logical_y - lab->scale_y * a->raw_y;
    lab->hotspot_x = ((a->logical_x - a->hw.effective_x) + (b->logical_x - b->hw.effective_x)) / 2.0;
    lab->hotspot_y = ((a->logical_y - a->hw.effective_y) + (b->logical_y - b->hw.effective_y)) / 2.0;
    lab->calibrated = true;
    printf("# mapping logical=(%.8f*ABS_X%+.5f,%.8f*ABS_Y%+.5f) logical_minus_hw_origin=%.3f,%.3f\n",
           lab->scale_x, lab->offset_x, lab->scale_y, lab->offset_y, lab->hotspot_x, lab->hotspot_y);
    printf("# constant-glyph soft-check=calibration-offset-consistency; MMIO hotspot is clipping, not the X theme hotspot; image identity is not observed\n");
    return 0;
}
static void drain_events(struct lab *lab, bool save)
{
    struct input_event events[64];
    ssize_t n;
    while ((n = read(lab->event, events, sizeof(events))) > 0) {
        if ((size_t)n % sizeof(events[0])) { lab->sampler_error = EIO; return; }
        for (size_t i = 0; i < (size_t)n / sizeof(events[0]); ++i) {
            struct input_event *e = &events[i];
            if (e->type == EV_ABS && e->code == ABS_X) lab->event_x = e->value;
            if (e->type == EV_ABS && e->code == ABS_Y) lab->event_y = e->value;
            if (e->type == EV_SYN && e->code == SYN_DROPPED) ++lab->syn_dropped;
            if (!save || e->type != EV_SYN || e->code != SYN_REPORT) continue;
            if (lab->event_count >= MAX_EVENTS) { ++lab->overflow; continue; }
            struct kernel_sample *k = &lab->events[lab->event_count++];
            k->ns = (uint64_t)e->input_event_sec * UINT64_C(1000000000) +
                    (uint64_t)e->input_event_usec * 1000;
            k->x = lab->event_x; k->y = lab->event_y;
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) lab->sampler_error = errno;
}
static void *sample_thread(void *arg)
{
    struct lab *lab = arg;
    uint64_t next = now_ns();
    while (!atomic_load_explicit(&lab->stop, memory_order_relaxed)) {
        drain_events(lab, true);
        uint64_t before = now_ns();
        struct hw_position pos = read_hw(lab);
        uint64_t after = now_ns();
        if (lab->sample_count < MAX_SAMPLES)
            lab->samples[lab->sample_count++] = (struct hw_sample){ before + (after - before) / 2, pos };
        else ++lab->overflow;
        next += NS_PER_MS;
        if (next < after) next = after + NS_PER_MS;
        sleep_until(next);
    }
    drain_events(lab, true);
    return NULL;
}
static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int report(struct lab *lab, struct point *points, unsigned count, unsigned interval)
{
    double latencies[MAX_POINTS], max_schedule_late_us = 0;
    unsigned matched = 0, kernel_seen = 0, max_backlog = 0;
    uint64_t max_gap = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct point *p = &points[i];
        for (size_t j = 0; j < lab->event_count; ++j) {
            struct kernel_sample *k = &lab->events[j];
            if (k->x == p->x && k->y == p->y && k->ns + 1000 >= p->submit_ns) {
                p->kernel_ns = k->ns; ++kernel_seen; break;
            }
        }
        if (!p->kernel_ns) continue;
        for (size_t j = 0; j < lab->sample_count; ++j) {
            struct hw_sample *s = &lab->samples[j];
            if (s->ns < p->kernel_ns || !s->pos.format) continue;
            if (abs(s->pos.effective_x - p->hw_x) <= MATCH_TOLERANCE &&
                abs(s->pos.effective_y - p->hw_y) <= MATCH_TOLERANCE) {
                p->matched_ns = s->ns;
                latencies[matched++] = (double)(s->ns - p->kernel_ns) / 1000.0;
                break;
            }
        }
    }
    for (size_t j = 0; j < lab->sample_count; ++j) {
        int latest = -1, observed = -1;
        for (unsigned i = 0; i < count; ++i) {
            if (points[i].kernel_ns && points[i].kernel_ns <= lab->samples[j].ns) latest = (int)i;
            if (points[i].matched_ns && points[i].matched_ns <= lab->samples[j].ns) observed = (int)i;
        }
        if (latest > observed && (unsigned)(latest - observed) > max_backlog)
            max_backlog = (unsigned)(latest - observed);
        if (j && lab->samples[j].ns - lab->samples[j - 1].ns > max_gap)
            max_gap = lab->samples[j].ns - lab->samples[j - 1].ns;
    }
    for (unsigned i = 1; i < count; ++i) {
        uint64_t target = points[0].submit_ns + (uint64_t)i * interval * NS_PER_MS;
        double late = points[i].submit_ns > target ? (points[i].submit_ns - target) / 1000.0 : 0;
        if (late > max_schedule_late_us) max_schedule_late_us = late;
    }
    printf("# INPUT,index,submit_ns,write_done_ns,kernel_ns,abs_x,abs_y,expected_logical_x,expected_logical_y,expected_hw_x,expected_hw_y,matched_hw_ns,latency_us\n");
    for (unsigned i = 0; i < count; ++i) {
        struct point *p = &points[i];
        double latency = p->matched_ns ? (double)(p->matched_ns - p->kernel_ns) / 1000.0 : -1.0;
        printf("INPUT,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d,%d,%d,%" PRIu64 ",%.3f\n",
               i, p->submit_ns, p->submitted_ns, p->kernel_ns, p->x, p->y,
               p->logical_x, p->logical_y, p->hw_x, p->hw_y, p->matched_ns, latency);
    }
    printf("# HW,monotonic_ns,x,y,hotspot_x,hotspot_y,effective_x,effective_y,format\n");
    for (size_t j = 0; j < lab->sample_count; ++j) {
        struct hw_sample *s = &lab->samples[j];
        printf("HW,%" PRIu64 ",%d,%d,%d,%d,%d,%d,%u\n", s->ns, s->pos.x, s->pos.y,
               s->pos.hotspot_x, s->pos.hotspot_y, s->pos.effective_x, s->pos.effective_y, s->pos.format);
    }
    qsort(latencies, matched, sizeof(latencies[0]), compare_double);
    double p50 = matched ? latencies[(matched - 1) / 2] : -1;
    double p95 = matched ? latencies[(95U * matched + 99U) / 100U - 1] : -1;
    printf("SUMMARY,{\"points\":%u,\"interval_ms\":%u,\"kernel_seen\":%u,\"hardware_matched\":%u,\"unobserved_or_coalesced\":%u,\"p50_us_observed_subset\":%.3f,\"p95_us_observed_subset\":%.3f,\"max_backlog_points\":%u,\"max_sample_gap_us\":%.3f,\"max_injection_lateness_us\":%.3f,\"overflow\":%u,\"syn_dropped\":%u}\n",
           count, interval, kernel_seen, matched, count - matched, p50, p95, max_backlog,
           max_gap / 1000.0, max_schedule_late_us, lab->overflow, lab->syn_dropped);
    if (kernel_seen != count || !matched) {
        fprintf(stderr, "Evidence incomplete: kernel_seen=%u/%u hardware_matched=%u\n",
                kernel_seen, count, matched);
        return -1;
    }
    return 0;
}
static int restore_pointer(struct lab *lab)
{
    if (!lab->created || !lab->pointer_saved) return 0;
    double sx = lab->calibrated ? lab->scale_x : (double)(lab->width - 1) / ABS_MAX_X;
    double sy = lab->calibrated ? lab->scale_y : (double)(lab->height - 1) / ABS_MAX_Y;
    double ox = lab->calibrated ? lab->offset_x : 0, oy = lab->calibrated ? lab->offset_y : 0;
    int rx = clamp(rounded((lab->saved_x - ox) / sx), ABS_MAX_X);
    int ry = clamp(rounded((lab->saved_y - oy) / sy), ABS_MAX_Y);
    int x = -1, y = -1;
    for (int i = 0; i < 3; ++i) {
        if (emit_absolute(lab, rx, ry)) return -1;
        sleep_ms(150);
        if (query_pointer(lab, &x, &y)) return -1;
        if (abs(x - lab->saved_x) <= 1 && abs(y - lab->saved_y) <= 1) {
            printf("# pointer_restored logical=%d,%d saved=%d,%d tolerance=1px\n", x, y, lab->saved_x, lab->saved_y);
            return 0;
        }
        rx = clamp(rx + rounded((lab->saved_x - x) / sx), ABS_MAX_X);
        ry = clamp(ry + rounded((lab->saved_y - y) / sy), ABS_MAX_Y);
    }
    fprintf(stderr, "Pointer restoration mismatch: saved=%d,%d actual=%d,%d\n", lab->saved_x, lab->saved_y, x, y);
    return -1;
}
int main(int argc, char **argv)
{
    struct lab lab = { .ui = -1, .event = -1, .mem = -1, .pipe = -1 };
    struct point points[MAX_POINTS] = {{0}};
    const char *display_name = ":0", *pci = "/sys/bus/pci/devices/0000:00:06.1";
    unsigned count = 160, interval = 2, sent = 0;
    int requested_pipe = -1, requested_lane = -1, lane_y, lane_start, lane_end, status = 1;
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--run")) run = true;
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) display_name = argv[++i];
        else if (!strcmp(argv[i], "--pci") && i + 1 < argc) pci = argv[++i];
        else if (!strcmp(argv[i], "--points") && i + 1 < argc) count = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) interval = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--lane-y") && i + 1 < argc) {
            char *end;
            errno = 0;
            long value = strtol(argv[++i], &end, 10);
            if (errno || !*argv[i] || *end || value < 0 || value > INT_MAX) goto usage;
            requested_lane = (int)value;
        }
        else if (!strcmp(argv[i], "--pipe") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "0")) requested_pipe = 0;
            else if (!strcmp(p, "1")) requested_pipe = 1;
            else goto usage;
        } else goto usage;
    }
    if (!run || count < 100 || count > MAX_POINTS || (interval != 2 && interval != 4)) goto usage;
    struct sigaction action = {0};
    action.sa_handler = signal_flag; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) || sigaction(SIGHUP, &action, NULL))
        return error("sigaction"), 1;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return error("SIGPIPE handler"), 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    atomic_init(&lab.stop, false);
    setenv("XAUTHORITY", "/run/lightdm/root/:0", 0);
    lab.display = XOpenDisplay(display_name);
    if (!lab.display) { fprintf(stderr, "Cannot open X display %s\n", display_name); goto cleanup; }
    x_error_lab = &lab; XSetIOErrorHandler(x_io_error);
    lab.width = DisplayWidth(lab.display, DefaultScreen(lab.display));
    lab.height = DisplayHeight(lab.display, DefaultScreen(lab.display));
    if (lab.width < 2 || lab.height < 2 || query_pointer(&lab, &lab.saved_x, &lab.saved_y)) goto cleanup;
    lab.pointer_saved = true;
    lane_y = requested_lane < 0 ? normalized(lab.height, 0.88) : clamp(requested_lane, lab.height - 1);
    lane_start = normalized(lab.width, 0.15);
    lane_end = normalized(lab.width, 0.80);
    if ((lane_end - lane_start) / (int)(count - 1) <= 2 * MATCH_TOLERANCE) {
        fprintf(stderr, "Screen width is too small to distinguish trajectory points at this tolerance\n");
        goto cleanup;
    }
    printf("# scope=evdev-kernel-input-to-first-observed-hardware-cursor-position NOT-photon-latency\n");
    printf("# first-observed timestamps include sampling delay; unmatched points are not included in latency percentiles\n");
    printf("# coalesced/unobserved points have unknown latency; observed-subset percentiles cannot describe all inputs\n");
    printf("# trajectory=lower-horizontal-strip logical_start=%d,%d logical_end=%d,%d; operator must verify this strip is blank and use the same UI context for A/B\n",
           lane_start, lane_y, lane_end, lane_y);
    printf("# XQueryPointer-is-logical-only screen=%dx%d saved_pointer=%d,%d sample_target_hz=1000 tolerance_px=%d\n",
           lab.width, lab.height, lab.saved_x, lab.saved_y, MATCH_TOLERANCE);
    if (map_registers(&lab, pci, requested_pipe)) goto cleanup;
    lab.samples = calloc(MAX_SAMPLES, sizeof(*lab.samples));
    lab.events = calloc(MAX_EVENTS, sizeof(*lab.events));
    if (!lab.samples || !lab.events) { error("sample allocation"); goto cleanup; }
    if (create_mouse(&lab) || calibrate(&lab) || interrupted) goto cleanup;
    lab.event_x = lab.cal[1].raw_x; lab.event_y = lab.cal[1].raw_y;
    drain_events(&lab, false);
    lab.syn_dropped = 0;
    int ret = pthread_create(&lab.thread, NULL, sample_thread, &lab);
    if (ret) { errno = ret; error("pthread_create"); goto cleanup; }
    lab.thread_started = true;
    uint64_t start = now_ns() + 20 * NS_PER_MS;
    for (unsigned i = 0; i < count && !interrupted; ++i) {
        struct point *p = &points[i];
        int desired_x = lane_start + (int)((uint64_t)(lane_end - lane_start) * i / (count - 1));
        p->x = clamp(rounded((desired_x - lab.offset_x) / lab.scale_x), ABS_MAX_X);
        p->y = clamp(rounded((lane_y - lab.offset_y) / lab.scale_y), ABS_MAX_Y);
        p->logical_x = rounded(lab.scale_x * p->x + lab.offset_x);
        p->logical_y = rounded(lab.scale_y * p->y + lab.offset_y);
        p->hw_x = rounded(lab.scale_x * p->x + lab.offset_x - lab.hotspot_x);
        p->hw_y = rounded(lab.scale_y * p->y + lab.offset_y - lab.hotspot_y);
        sleep_until(start + (uint64_t)i * interval * NS_PER_MS);
        if (interrupted) break;
        p->submit_ns = now_ns();
        if (emit_absolute(&lab, p->x, p->y)) goto cleanup;
        p->submitted_ns = now_ns();
        ++sent;
    }
    if (!interrupted) {
        int x, y;
        /* No X requests on the timed injection path. This endpoint may block
         * in Xorg; the independent sampler continues throughout that wait. */
        if (query_pointer(&lab, &x, &y)) goto cleanup;
        printf("# endpoint logical=%d,%d monotonic_ns=%" PRIu64 "\n", x, y, now_ns());
        sleep_ms(300);
    }
    status = interrupted ? 1 : 0;
cleanup:
    if (lab.thread_started) {
        atomic_store_explicit(&lab.stop, true, memory_order_relaxed);
        pthread_join(lab.thread, NULL);
    }
    if (restore_pointer(&lab)) status = 1;
    if (lab.created && ioctl(lab.ui, UI_DEV_DESTROY)) { error("UI_DEV_DESTROY"); status = 1; }
    lab.created = false;
    if (lab.event >= 0) close(lab.event);
    if (lab.ui >= 0) close(lab.ui);
    if (lab.mapping) munmap(lab.mapping, lab.mapping_size);
    if (lab.mem >= 0) close(lab.mem);
    if (lab.display) XCloseDisplay(lab.display);
    if (sent && report(&lab, points, sent, interval)) status = 1;
    if (lab.sampler_error || lab.overflow || lab.syn_dropped) {
        fprintf(stderr, "Sampler evidence invalid: errno=%d overflow=%u SYN_DROPPED=%u\n",
                lab.sampler_error, lab.overflow, lab.syn_dropped);
        status = 1;
    }
    free(lab.samples); free(lab.events);
    printf("# virtual-device-removed result=%s\n", status ? "FAIL" : "DONE");
    return status;
usage:
    fprintf(stderr, "Usage: %s --run [--points 100..200] [--interval-ms 2|4] [--lane-y LOGICAL_Y] [--display :0] [--pipe 0|1] [--pci SYSFS_PATH]\nDefault lane: x=15%%..80%% of screen width, y=88%% of screen height. --lane-y is clamped to the screen.\nNo injection occurs without --run. No buttons are pressed; real mouse settings are untouched.\n", argv[0]);
    return 1;
}
