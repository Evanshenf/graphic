// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone LS7A2000 fault/recovery test. Requires root and exclusive KMS.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror tests/kms-fault.c \
 *           -o /tmp/graphic-kms-fault $(pkg-config --cflags --libs libdrm)
 * Run: timeout --kill-after=15s 45s /tmp/graphic-kms-fault /dev/dri/card0
 *
 * EXPECT one driver flip timeout in the supervisor's new dmesg capture.
 * Blocking ioctls are bounded by the outer supervisor, not the event poll's
 * deadline. A SIGKILL cannot execute restoration. The original framebuffer
 * must remain owned by its original client; the legacy API cannot save the
 * original cursor image, so cleanup hides the test cursor.
 *
 * Manual V1.3: DC interrupt 0x1570 has enables in its upper half and W1C
 * statuses in its lower half. The only direct register write here is zero:
 * it masks interrupts without acknowledging statuses. Driver modesets must
 * restore the mask; writing a saved mask over its later software state would
 * be incorrect because vblank references and disable timers have changed.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define PCI_DEVICE "0000:00:06.1"
#define IRQ_REG 0x1570
#define FB_CONFIG 0x1240
#define VSYNC_COUNTER 0x1a00
#define FB_ENABLE (UINT32_C(1) << 8)
#define CURSOR_SIZE 64
#define CURSOR_LOCATION0 0x1540
#define CURSOR_LOCATION1 0x1690
#define CURSOR_LOCATION_MASK UINT32_C(0x0fff0fff)
#define MOVE_SAMPLES 300
#define DC_BYTES 0x2000

struct buffer {
	uint32_t handle;
	uint32_t fb;
	uint32_t pitch;
	size_t size;
	void *map;
};

struct notice {
	const char *name;
	unsigned int count;
};

struct test {
	int fd;
	int mem_fd;
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeCrtc *saved_crtc;
	uint32_t *saved_connectors;
	int saved_connector_count;
	unsigned int pipe;
	uint32_t crtc;
	drmModeModeInfo mode;
	struct buffer buffers[4];
	void *mapping;
	size_t mapping_size;
	volatile uint8_t *dc;
	uint32_t original_enable;
	uint32_t injected_enable;
	bool modeset_attempted;
	bool injected;
	struct notice long_wait;
	struct notice flip_a;
	struct notice final_flip;
};

static volatile sig_atomic_t interrupted;

static void handle_signal(int number)
{
	interrupted = number;
}

static int fail(const char *operation)
{
	fprintf(stderr, "FAIL %s: %s\n", operation, strerror(errno));
	return -1;
}

static uint64_t now_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static uint32_t reg_read(struct test *test, unsigned int offset)
{
	uint32_t value = *(volatile uint32_t *)(test->dc + offset);

	atomic_thread_fence(memory_order_seq_cst);
	return value;
}

static int open_mmio(struct test *test)
{
	char path[128], link[PATH_MAX];
	uint64_t base, end, flags, aligned;
	struct stat status;
	ssize_t length;
	long page_size;
	FILE *resource;
	drmVersion *version;

	version = drmGetVersion(test->fd);
	if (!version)
		return fail("DRM version");
	if (version->name_len != 7 || memcmp(version->name, "graphic", 7)) {
		drmFreeVersion(version);
		fprintf(stderr, "FAIL this test requires the graphic driver\n");
		return -1;
	}
	drmFreeVersion(version);
	if (fstat(test->fd, &status))
		return fail("DRM device stat");
	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device",
		 major(status.st_rdev), minor(status.st_rdev));
	length = readlink(path, link, sizeof(link) - 1);
	if (length < 0)
		return fail("DRM PCI identity");
	link[length] = '\0';
	if (!strrchr(link, '/') || strcmp(strrchr(link, '/') + 1, PCI_DEVICE)) {
		fprintf(stderr, "FAIL DRM node is not PCI %s\n", PCI_DEVICE);
		return -1;
	}
	resource = fopen("/sys/bus/pci/devices/" PCI_DEVICE "/resource", "r");
	if (!resource)
		return fail("PCI BAR resource");
	if (fscanf(resource, "%" SCNx64 " %" SCNx64 " %" SCNx64,
		   &base, &end, &flags) != 3) {
		fclose(resource);
		fprintf(stderr, "FAIL malformed BAR0 resource\n");
		return -1;
	}
	fclose(resource);
	if (!(flags & 0x200) || end < base || end - base + 1 < DC_BYTES) {
		fprintf(stderr, "FAIL BAR0 is not a sufficient memory resource\n");
		return -1;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return fail("page size");
	aligned = base & ~((uint64_t)page_size - 1);
	if (aligned > INT64_MAX)
		return fail("BAR0 cannot be represented by mmap offset");
	test->mapping_size = (size_t)(base - aligned + DC_BYTES + page_size - 1) &
			     ~((size_t)page_size - 1);
	test->mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (test->mem_fd < 0)
		return fail("open /dev/mem");
	test->mapping = mmap(NULL, test->mapping_size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, test->mem_fd, (off_t)aligned);
	if (test->mapping == MAP_FAILED) {
		test->mapping = NULL;
		return fail("map DC BAR0");
	}
	test->dc = (volatile uint8_t *)test->mapping + base - aligned;
	test->original_enable = reg_read(test, IRQ_REG) >> 16;
	printf("INFO BAR0=%#" PRIx64 " initial IRQ enables=%#x\n",
	       base, test->original_enable);
	return 0;
}

static int select_and_save(struct test *test)
{
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	int i, j;

	test->resources = drmModeGetResources(test->fd);
	if (!test->resources)
		return fail("resources");
	for (i = 0; i < test->resources->count_connectors; i++) {
		connector = drmModeGetConnector(test->fd, test->resources->connectors[i]);
		if (!connector)
			return fail("connector");
		if (connector->connection != DRM_MODE_CONNECTED ||
		    !connector->count_modes || !connector->encoder_id) {
			drmModeFreeConnector(connector);
			continue;
		}
		encoder = drmModeGetEncoder(test->fd, connector->encoder_id);
		if (!encoder) {
			drmModeFreeConnector(connector);
			return fail("encoder");
		}
		for (j = 0; j < test->resources->count_crtcs && j < 2; j++) {
			if (test->resources->crtcs[j] == encoder->crtc_id &&
			    (encoder->possible_crtcs & (UINT32_C(1) << j)))
				break;
		}
		drmModeFreeEncoder(encoder);
		if (j >= test->resources->count_crtcs || j >= 2) {
			drmModeFreeConnector(connector);
			continue;
		}
		test->connector = connector;
		test->pipe = (unsigned int)j;
		test->crtc = test->resources->crtcs[j];
		test->mode = connector->modes[0];
		break;
	}
	if (!test->connector) {
		fprintf(stderr, "FAIL no connected, currently routed output\n");
		return -1;
	}
	test->saved_crtc = drmModeGetCrtc(test->fd, test->crtc);
	if (!test->saved_crtc)
		return fail("save CRTC");
	test->saved_connectors = calloc((size_t)test->resources->count_connectors,
					 sizeof(*test->saved_connectors));
	if (!test->saved_connectors)
		return fail("save routes allocation");
	for (i = 0; i < test->resources->count_connectors; i++) {
		connector = drmModeGetConnector(test->fd, test->resources->connectors[i]);
		if (!connector)
			return fail("save connector route");
		if (connector->encoder_id) {
			encoder = drmModeGetEncoder(test->fd, connector->encoder_id);
			if (!encoder) {
				drmModeFreeConnector(connector);
				return fail("save encoder route");
			}
			if (encoder->crtc_id == test->crtc)
				test->saved_connectors[test->saved_connector_count++] =
					connector->connector_id;
			drmModeFreeEncoder(encoder);
		}
		drmModeFreeConnector(connector);
	}
	printf("INFO connector=%u CRTC=%u pipe=%u mode=%s\n",
	       test->connector->connector_id, test->crtc, test->pipe, test->mode.name);
	return 0;
}

static int create_buffer(struct test *test, struct buffer *buffer,
			 uint32_t width, uint32_t height, uint32_t color, bool primary)
{
	struct drm_mode_create_dumb create = { .width = width, .height = height, .bpp = 32 };
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint32_t x, y;

	if (drmIoctl(test->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
		return fail("create dumb");
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	if (create.size > SIZE_MAX || create.pitch < width * 4 ||
	    (!primary && create.pitch != 256)) {
		fprintf(stderr, "FAIL unsupported dumb allocation/pitch\n");
		return -1;
	}
	buffer->size = (size_t)create.size;
	map.handle = buffer->handle;
	if (drmIoctl(test->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return fail("map dumb offset");
	buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   test->fd, (off_t)map.offset);
	if (buffer->map == MAP_FAILED) {
		buffer->map = NULL;
		return fail("map dumb");
	}
	memset(buffer->map, 0, buffer->size);
	for (y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)buffer->map + y * buffer->pitch);

		for (x = 0; x < width; x++)
			row[x] = color;
	}
	atomic_thread_fence(memory_order_seq_cst);
	if (!primary)
		return 0;
	handles[0] = buffer->handle;
	pitches[0] = buffer->pitch;
	if (drmModeAddFB2(test->fd, width, height, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &buffer->fb, 0))
		return fail("add primary FB");
	return 0;
}

static int close_buffer(struct test *test, struct buffer *buffer)
{
	struct drm_gem_close close = { .handle = buffer->handle };
	int result = 0;

	if (buffer->fb && drmModeRmFB(test->fd, buffer->fb))
		result = fail("remove FB");
	buffer->fb = 0;
	if (buffer->map && munmap(buffer->map, buffer->size))
		result = fail("unmap BO");
	buffer->map = NULL;
	if (buffer->handle && drmIoctl(test->fd, DRM_IOCTL_GEM_CLOSE, &close))
		result = fail("close BO handle");
	buffer->handle = 0;
	return result;
}

static void event_handler(int fd, unsigned int sequence, unsigned int seconds,
			  unsigned int useconds, void *data)
{
	struct notice *notice = data;

	(void)fd;
	(void)seconds;
	(void)useconds;
	if (notice) {
		notice->count++;
		printf("EVENT %s sequence=%u\n", notice->name, sequence);
	}
}

static int wait_event(struct test *test, struct notice *notice)
{
	drmEventContext events = { .version = DRM_EVENT_CONTEXT_VERSION,
		.vblank_handler = event_handler, .page_flip_handler = event_handler };
	uint64_t deadline = now_ns() + UINT64_C(2000000000);
	struct pollfd pollfd = { .fd = test->fd, .events = POLLIN };
	int result;

	while (!notice->count && !interrupted) {
		uint64_t now = now_ns();

		if (now >= deadline) {
			fprintf(stderr, "FAIL event poll deadline: %s\n", notice->name);
			return -1;
		}
		result = poll(&pollfd, 1, (int)((deadline - now + 999999) / 1000000));
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			return fail("event poll");
		if (result && (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "FAIL DRM poll fd error\n");
			return -1;
		}
		if (result && (pollfd.revents & POLLIN) && drmHandleEvent(test->fd, &events))
			return fail("handle DRM events");
	}
	return interrupted ? -1 : 0;
}

static drmVBlankSeqType vblank_type(struct test *test, unsigned int flags)
{
	return (drmVBlankSeqType)(flags |
		((test->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK));
}

static uint32_t cursor_location(struct test *test)
{
	return reg_read(test, test->pipe ? CURSOR_LOCATION1 : CURSOR_LOCATION0) &
		CURSOR_LOCATION_MASK;
}

static int observe_cursor_position(struct test *test, const char *label,
				   uint32_t expected, uint64_t submitted,
				   uint64_t returned, uint32_t initial_count)
{
	uint64_t pixels, frame_ns, deadline, observed, remaining;
	uint32_t count_before, count_after, location;
	uint32_t last_location = UINT32_MAX, last_count = UINT32_MAX;
	unsigned int samples = 0;
	struct timespec delay;

	if (!test->mode.clock || !test->mode.htotal || !test->mode.vtotal) {
		fprintf(stderr, "FAIL cannot derive a frame interval for position observation\n");
		return -1;
	}
	pixels = (uint64_t)test->mode.htotal * test->mode.vtotal;
	/* clock is kHz, so pixels * 1,000,000 / clock gives nanoseconds. */
	frame_ns = (pixels * UINT64_C(1000000) + test->mode.clock - 1) /
		   test->mode.clock;
	deadline = returned + 2 * frame_ns + UINT64_C(5000000);
	printf("OBSERVE %s: expected=%#x sample_delay_us=100 frame_ms=%.6f deadline_after_return_ms=%.6f\n",
	       label, expected, frame_ns / 1e6, (2 * frame_ns + 5000000) / 1e6);
	while (!interrupted) {
		count_before = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
		location = cursor_location(test);
		count_after = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
		observed = now_ns();
		if (!observed || observed < returned)
			return fail("position observation clock");
		samples++;
		/* Print the initial sample and changes, not every 100 us poll. */
		if (location != last_location || count_after != last_count) {
			printf("SAMPLE %s: after_submit_ms=%.6f after_return_ms=%.6f location=%#x counter=%u..%u advance=%u\n",
			       label, (observed - submitted) / 1e6,
			       (observed - returned) / 1e6, location,
			       count_before, count_after, count_after - initial_count);
			last_location = location;
			last_count = count_after;
		}
		if (location == expected && observed <= deadline) {
			printf("PASS %s settled: first_observed_after_submit_ms=%.6f after_return_ms=%.6f counter_advance=%u samples=%u\n",
			       label, (observed - submitted) / 1e6,
			       (observed - returned) / 1e6,
			       count_after - initial_count, samples);
			return 0;
		}
		if (observed >= deadline) {
			fprintf(stderr, "FAIL %s position observation deadline: location=%#x expected=%#x counter_advance=%u\n",
				label, location, expected, count_after - initial_count);
			return -1;
		}
		remaining = deadline - observed;
		delay.tv_sec = 0;
		delay.tv_nsec = (long)(remaining < 100000 ? remaining : 100000);
		while (nanosleep(&delay, &delay)) {
			if (errno != EINTR)
				return fail("position observation sleep");
			if (interrupted)
				return -1;
		}
	}
	return -1;
}

static int eventless_step(struct test *test, bool replace)
{
	unsigned int counter = VSYNC_COUNTER + test->pipe * 16;
	uint32_t before = reg_read(test, counter), after;
	uint64_t start = now_ns(), returned, elapsed;
	int result;

	if (replace)
		result = drmModeSetCursor2(test->fd, test->crtc, test->buffers[2].handle,
					   CURSOR_SIZE, CURSOR_SIZE, 0, 0);
	else
		result = drmModeMoveCursor(test->fd, test->crtc, 160, 96);
	returned = now_ns();
	elapsed = returned - start;
	after = reg_read(test, counter);
	if (result)
		return fail(replace ? "eventless cursor replacement" : "eventless cursor move");
	printf("OBSERVE eventless cursor %s: %.3f ms counter=%u->%u\n",
	       replace ? "replace" : "move", elapsed / 1e6, before, after);
	if (replace && after == before) {
		fprintf(stderr, "FAIL cursor BO replacement returned before a frame advanced\n");
		return -1;
	}
	if (!replace)
		return observe_cursor_position(test, "single position-only move",
					       (UINT32_C(96) << 16) | 160,
					       start, returned, before);
	return 0;
}

static int compare_latency(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static int measure_cursor_moves(struct test *test)
{
	uint64_t latency[MOVE_SAMPLES], sum = 0;
	uint64_t start = 0, finish = 0, wall_start, wall_end;
	unsigned int i, samples = 0, ioctl_errors = 0, immediate_matches = 0;
	unsigned int observed_changes = 0;
	uint32_t max_x, max_y, before, after, settled, expected = 0, actual, previous;
	uint32_t last_count = 0;
	int result, ioctl_errno, settle_result = -1;

	if (test->mode.hdisplay <= CURSOR_SIZE || test->mode.vdisplay <= CURSOR_SIZE) {
		fprintf(stderr, "FAIL mode has no room for fully visible cursor movement\n");
		return -1;
	}
	max_x = test->mode.hdisplay - CURSOR_SIZE;
	max_y = test->mode.vdisplay - CURSOR_SIZE;
	before = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
	previous = cursor_location(test);
	wall_start = now_ns();
	if (!wall_start)
		return fail("move measurement clock");
	for (i = 0; i < MOVE_SAMPLES && !interrupted; i++) {
		/* Every image stays fully onscreen; no BO or visibility changes. */
		uint32_t x = (i * 37 + 17) % (max_x + 1);
		uint32_t y = (i * 19 + 11) % (max_y + 1);

		last_count = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
		start = now_ns();
		if (!start)
			return fail("move start clock");
		result = drmModeMoveCursor(test->fd, test->crtc, (int)x, (int)y);
		ioctl_errno = errno;
		finish = now_ns();
		if (!finish || finish < start)
			return fail("move finish clock");
		latency[samples++] = finish - start;
		sum += finish - start;
		if (result) {
			if (!ioctl_errors)
				fprintf(stderr, "FAIL measured move %u: %s\n", i,
					strerror(ioctl_errno));
			ioctl_errors++;
			continue;
		}
		expected = (y << 16) | x;
		actual = cursor_location(test);
		if (actual == expected)
			immediate_matches++;
		if (actual != previous)
			observed_changes++;
		previous = actual;
	}
	wall_end = now_ns();
	after = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
	if (!samples || wall_end <= wall_start) {
		fprintf(stderr, "FAIL no valid cursor measurement interval\n");
		return -1;
	}
	/* One bounded observation after the batch; never wait after each move. */
	if (samples == MOVE_SAMPLES && !ioctl_errors && !interrupted)
		settle_result = observe_cursor_position(test, "batch final position",
						 expected, start, finish, last_count);
	settled = reg_read(test, VSYNC_COUNTER + test->pipe * 16);
	qsort(latency, samples, sizeof(latency[0]), compare_latency);
	/* Nearest-rank percentiles; ioctl times exclude position readback. */
	printf("MEASURE same-FB visible cursor: samples=%u ioctlErrors=%u immediate_matches=%u immediate_misses=%u observed_location_changes=%u "
	       "mean_ms=%.6f p50_ms=%.6f p95_ms=%.6f max_ms=%.6f "
	       "submission_total_ms=%.3f calls_per_second=%.3f counter_before=%u after_batch=%u after_settle=%u\n",
	       samples, ioctl_errors, immediate_matches,
	       samples - ioctl_errors - immediate_matches, observed_changes,
	       sum / (double)samples / 1e6,
	       latency[(samples * 50 + 99) / 100 - 1] / 1e6,
	       latency[(samples * 95 + 99) / 100 - 1] / 1e6,
	       latency[samples - 1] / 1e6, (wall_end - wall_start) / 1e6,
	       samples * 1e9 / (wall_end - wall_start), before, after, settled);
	printf("NOTE throughput includes readback but excludes final settle; immediate misses may reflect coalescing, not a proven latch rule\n");
	if (samples != MOVE_SAMPLES || ioctl_errors || settle_result || interrupted)
		return -1;
	printf("PASS 300 same-FB moves: zero ioctl errors and final hardware position confirmed within deadline\n");
	return 0;
}

static int experiment(struct test *test)
{
	drmVBlank hold = { 0 };
	uint32_t connector = test->connector->connector_id;
	uint32_t config, cursor, enabled;
	uint64_t start, elapsed;
	int result;

	if (create_buffer(test, &test->buffers[0], test->mode.hdisplay,
			  test->mode.vdisplay, 0xff163047, true) ||
	    create_buffer(test, &test->buffers[1], test->mode.hdisplay,
			  test->mode.vdisplay, 0xff472316, true) ||
	    create_buffer(test, &test->buffers[2], CURSOR_SIZE, CURSOR_SIZE,
			  0xffffcc00, false) ||
	    create_buffer(test, &test->buffers[3], CURSOR_SIZE, CURSOR_SIZE,
			  0xff00ccff, false))
		return -1;
	test->modeset_attempted = true;
	if (drmModeSetCrtc(test->fd, test->crtc, test->buffers[0].fb, 0, 0,
			   &connector, 1, &test->mode))
		return fail("initial modeset");
	if (drmModeSetCursor2(test->fd, test->crtc, test->buffers[2].handle,
			      CURSOR_SIZE, CURSOR_SIZE, 0, 0) ||
	    drmModeMoveCursor(test->fd, test->crtc, 64, 64))
		return fail("initial cursor");
	if (interrupted)
		return -1;

	/* The long core reference prevents A's vblank_get from unmasking IRQs. */
	hold.request.type = vblank_type(test, DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
	hold.request.sequence = 1000;
	hold.request.signal = (unsigned long)(uintptr_t)&test->long_wait;
	if (drmWaitVBlank(test->fd, &hold))
		return fail("long vblank event reference");
	test->injected_enable = reg_read(test, IRQ_REG) >> 16;
	enabled = UINT32_C(1) << (test->pipe ? 0 : 2);
	if (!(test->injected_enable & enabled)) {
		fprintf(stderr, "FAIL VSYNC interrupt was not enabled before injection\n");
		return -1;
	}
	printf("INJECT IRQ enables %#x -> 0; expect one driver flip timeout\n",
	       test->injected_enable);
	*(volatile uint32_t *)(test->dc + IRQ_REG) = 0;
	atomic_thread_fence(memory_order_seq_cst);
	test->injected = true;
	if (reg_read(test, IRQ_REG) >> 16) {
		fprintf(stderr, "FAIL IRQ mask did not remain zero\n");
		return -1;
	}
	if (drmModePageFlip(test->fd, test->crtc, test->buffers[1].fb,
			    DRM_MODE_PAGE_FLIP_EVENT, &test->flip_a))
		return fail("nonblocking flip A");

	/* B is blocking: its check can precede A's timeout and hardware finish. */
	start = now_ns();
	result = drmModeSetCursor2(test->fd, test->crtc, test->buffers[3].handle,
				  CURSOR_SIZE, CURSOR_SIZE, 0, 0);
	elapsed = now_ns() - start;
	config = reg_read(test, FB_CONFIG + test->pipe * 16);
	cursor = reg_read(test, test->pipe ? 0x1670 : 0x1520);
	printf("OBSERVE B: result=%d elapsed=%.3f ms FB_CONFIG=%#x CURSOR_CONFIG=%#x IRQ=%#x\n",
	       result, elapsed / 1e6, config, cursor, reg_read(test, IRQ_REG));
	if (result)
		return fail("blocking successor B");
	if (!(config & FB_ENABLE) || (cursor & 3) != 2) {
		fprintf(stderr, "FAIL B returned without restoring scanout and cursor\n");
		return -1;
	}
	if (elapsed < UINT64_C(9000000000)) {
		fprintf(stderr, "FAIL injection did not demonstrate the expected ~10 s wait\n");
		return -1;
	}
	printf("PASS recovery: B returned with FB_ENABLE and ARGB cursor enabled\n");
	printf("NOTE B check timing is inferred from blocking API ordering and elapsed time; no tracepoint assertion\n");
	if (interrupted || eventless_step(test, true) || eventless_step(test, false))
		return -1;
	/* Unmap first so closing the replaced image releases our final BO owner. */
	if (close_buffer(test, &test->buffers[3]))
		return -1;
	printf("PASS eventless cursor: replacement advanced a frame, move coordinates matched; old BO closed\n");
	if (drmModePageFlip(test->fd, test->crtc, test->buffers[0].fb,
			    DRM_MODE_PAGE_FLIP_EVENT, &test->final_flip))
		return fail("post-recovery page flip");
	if (wait_event(test, &test->final_flip))
		return -1;
	if (test->flip_a.count != 1) {
		fprintf(stderr, "FAIL timed-out flip A was not notified exactly once\n");
		return -1;
	}
	if (!(reg_read(test, FB_CONFIG + test->pipe * 16) & FB_ENABLE)) {
		fprintf(stderr, "FAIL post-recovery output disabled\n");
		return -1;
	}
	printf("PASS normal IRQ-driven page flip after recovery\n");
	return measure_cursor_moves(test);
}

static int cleanup(struct test *test)
{
	bool restored = !test->modeset_attempted;
	drmVBlank verify = { 0 };
	int result = 0, i;

	if (test->modeset_attempted) {
		/* Stop first. This also makes the driver rewrite its IRQ mask. */
		if (drmModeSetCrtc(test->fd, test->crtc, 0, 0, 0, NULL, 0, NULL))
			result = fail("cleanup stop CRTC");
		if (drmModeSetCursor2(test->fd, test->crtc, 0, 0, 0, 0, 0))
			result = fail("cleanup hide cursor");
		if (test->saved_crtc->mode_valid) {
			if (drmModeSetCrtc(test->fd, test->crtc, test->saved_crtc->buffer_id,
					   test->saved_crtc->x, test->saved_crtc->y,
					   test->saved_connectors, test->saved_connector_count,
					   &test->saved_crtc->mode)) {
				result = fail("restore original CRTC/connector route");
			} else if (!(reg_read(test, FB_CONFIG + test->pipe * 16) & FB_ENABLE)) {
				fprintf(stderr, "FAIL restored mode returned with output disabled\n");
				result = -1;
			} else {
				restored = true;
			}
		} else if (!(reg_read(test, FB_CONFIG + test->pipe * 16) & FB_ENABLE)) {
			restored = true;
		}
		if (restored && test->saved_crtc->mode_valid && !interrupted) {
			verify.request.type = vblank_type(test, DRM_VBLANK_RELATIVE);
			verify.request.sequence = 1;
			if (drmWaitVBlank(test->fd, &verify))
				result = fail("restored-route vblank/IRQ check");
		}
		if (test->injected)
			printf("RESTORE IRQ by driver stop/modeset: original=%#x injected=%#x final=%#x (enables only)\n",
			       test->original_enable, test->injected_enable,
			       reg_read(test, IRQ_REG) >> 16);
		if (restored && !result)
			printf("PASS original CRTC/connector route restored; cursor hidden\n");
	}
	if (!restored) {
		fprintf(stderr, "FAIL route restoration incomplete; supervisor must inspect display\n");
		result = -1;
	}
	/* Restoration is attempted before dropping our framebuffer owners. */
	for (i = 0; i < 4; i++)
		if (close_buffer(test, &test->buffers[i]))
			result = -1;
	if (test->mapping)
		munmap(test->mapping, test->mapping_size);
	if (test->mem_fd >= 0)
		close(test->mem_fd);
	if (test->saved_crtc)
		drmModeFreeCrtc(test->saved_crtc);
	if (test->connector)
		drmModeFreeConnector(test->connector);
	if (test->resources)
		drmModeFreeResources(test->resources);
	free(test->saved_connectors);
	if (test->fd >= 0) {
		drmDropMaster(test->fd);
		close(test->fd);
	}
	return result;
}

int main(int argc, char **argv)
{
	struct test test = { .fd = -1, .mem_fd = -1,
		.long_wait = { .name = "long-vblank-reference" },
		.flip_a = { .name = "timed-out-flip-A" },
		.final_flip = { .name = "normal-final-flip" } };
	struct sigaction action = { .sa_handler = handle_signal };
	const char *node = argc == 2 ? argv[1] : "/dev/dri/card0";
	int result = -1;

	if (argc > 2 || (argc == 2 && !strcmp(argv[1], "--help"))) {
		printf("Usage: timeout --kill-after=15s 45s %s [/dev/dri/card0]\n", argv[0]);
		return argc > 2 ? 2 : 0;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
		fail("signal handlers");
		return EXIT_FAILURE;
	}
	test.fd = open(node, O_RDWR | O_CLOEXEC);
	if (test.fd < 0) {
		fail("open DRM");
		goto out;
	}
	if (drmSetMaster(test.fd)) {
		fail("exclusive DRM master");
		goto out;
	}
	if (open_mmio(&test) || select_and_save(&test))
		goto out;
	result = experiment(&test);
out:
	if (cleanup(&test))
		result = -1;
	if (interrupted) {
		fprintf(stderr, "FAIL interrupted by signal %d\n", (int)interrupted);
		result = -1;
	}
	printf("%s fault/recovery test (supervisor must correlate new dmesg)\n",
	       result ? "FAIL" : "PASS");
	return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
