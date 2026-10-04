// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone KMS smoke test; no hardware register knowledge is used here.
 * Build on the target:
 * cc -std=c11 -O2 -Wall -Wextra -Werror tests/kms-smoke.c \
 *    -o /tmp/graphic-kms-smoke $(pkg-config --cflags --libs libdrm)
 * Run with exclusive KMS ownership, after stopping the display manager:
 * /tmp/graphic-kms-smoke [/dev/dri/card0] [--no-cursor]
 *
 * The original CRTC and its connector routing are saved; the original cursor
 * is not recoverable through the legacy cursor API. Event waits have a 2 s
 * deadline. A kernel ioctl that itself hangs is outside that userspace bound.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define WIDTH 1920
#define HEIGHT 1080
#define CURSOR_SIZE 64
#define EVENT_TIMEOUT_MS 2000
#define FLIP_COUNT 10

struct buffer {
	const char *name;
	uint32_t handle;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	size_t size;
	uint8_t *map;
};

struct test {
	int fd;
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeCrtc *old_crtc;
	drmModeModeInfo mode;
	uint32_t crtc_id;
	unsigned int crtc_index;
	uint32_t *old_connectors;
	int old_connector_count;
	bool modeset_attempted;
	bool cursor_enabled;
	bool cursor_skipped;
	struct buffer buffers[3];
};

static volatile sig_atomic_t interrupted;

static void handle_signal(int signo)
{
	interrupted = signo;
}

static int fail_errno(const char *operation)
{
	int error = errno;

	fprintf(stderr, "FAIL %s: %s (errno=%d)\n", operation,
		strerror(error), error);
	return -1;
}

static int64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -1;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int observe_ms(unsigned int duration)
{
	struct timespec delay = {
		.tv_sec = duration / 1000,
		.tv_nsec = (long)(duration % 1000) * 1000000,
	};

	while (!interrupted && nanosleep(&delay, &delay)) {
		if (errno != EINTR)
			return fail_errno("observation delay");
	}
	return interrupted ? -1 : 0;
}

static int crtc_index(const drmModeRes *resources, uint32_t id)
{
	int i;

	for (i = 0; i < resources->count_crtcs; i++)
		if (resources->crtcs[i] == id)
			return i;
	return -1;
}

static int select_crtc(struct test *test, drmModeConnector *connector)
{
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;
	int i, j, index;

	/* Preserve an existing route before considering an unused CRTC. */
	if (connector->encoder_id) {
		encoder = drmModeGetEncoder(test->fd, connector->encoder_id);
		if (encoder) {
			index = crtc_index(test->resources, encoder->crtc_id);
			if (index >= 0 && index < 32 &&
			    (encoder->possible_crtcs & (UINT32_C(1) << index))) {
				drmModeFreeEncoder(encoder);
				return index;
			}
			drmModeFreeEncoder(encoder);
		}
	}

	for (i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(test->fd, connector->encoders[i]);
		if (!encoder)
			continue;
		for (j = 0; j < test->resources->count_crtcs && j < 32; j++) {
			if (!(encoder->possible_crtcs & (UINT32_C(1) << j)))
				continue;
			crtc = drmModeGetCrtc(test->fd, test->resources->crtcs[j]);
			if (!crtc)
				continue;
			if (!crtc->mode_valid) {
				drmModeFreeCrtc(crtc);
				drmModeFreeEncoder(encoder);
				return j;
			}
			drmModeFreeCrtc(crtc);
		}
		drmModeFreeEncoder(encoder);
	}
	return -1;
}

static int select_output(struct test *test)
{
	drmModeConnector *connector;
	int i, j, index;

	test->resources = drmModeGetResources(test->fd);
	if (!test->resources)
		return fail_errno("drmModeGetResources");

	for (i = 0; i < test->resources->count_connectors; i++) {
		connector = drmModeGetConnector(test->fd,
					       test->resources->connectors[i]);
		if (!connector)
			continue;
		if (connector->connection != DRM_MODE_CONNECTED ||
		    (connector->connector_type != DRM_MODE_CONNECTOR_HDMIA &&
		     connector->connector_type != DRM_MODE_CONNECTOR_HDMIB))
			goto next;
		for (j = 0; j < connector->count_modes; j++) {
			if (connector->modes[j].hdisplay != WIDTH ||
			    connector->modes[j].vdisplay != HEIGHT)
				continue;
			index = select_crtc(test, connector);
			if (index < 0)
				continue;
			test->connector = connector;
			test->mode = connector->modes[j];
			test->crtc_index = (unsigned int)index;
			test->crtc_id = test->resources->crtcs[index];
			printf("PASS output: connector=%u CRTC=%u index=%u "
			       "mode=%s clock=%u kHz h=%u/%u/%u/%u "
			       "v=%u/%u/%u/%u flags=0x%x\n",
			       connector->connector_id, test->crtc_id,
			       test->crtc_index, test->mode.name, test->mode.clock,
			       test->mode.hdisplay, test->mode.hsync_start,
			       test->mode.hsync_end, test->mode.htotal,
			       test->mode.vdisplay, test->mode.vsync_start,
			       test->mode.vsync_end, test->mode.vtotal,
			       test->mode.flags);
			return 0;
		}
next:
		drmModeFreeConnector(connector);
	}
	fprintf(stderr, "FAIL no connected HDMI with an advertised "
		"1920x1080 mode and a usable encoder/CRTC\n");
	return -1;
}

static int save_state(struct test *test)
{
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	int i;

	test->old_crtc = drmModeGetCrtc(test->fd, test->crtc_id);
	if (!test->old_crtc)
		return fail_errno("save CRTC");
	test->old_connectors = calloc((size_t)test->resources->count_connectors,
				      sizeof(*test->old_connectors));
	if (!test->old_connectors)
		return fail_errno("save connector allocation");
	for (i = 0; i < test->resources->count_connectors; i++) {
		connector = drmModeGetConnector(test->fd,
					       test->resources->connectors[i]);
		if (!connector)
			return fail_errno("save connector");
		if (connector->encoder_id) {
			encoder = drmModeGetEncoder(test->fd,
						    connector->encoder_id);
			if (!encoder) {
				drmModeFreeConnector(connector);
				return fail_errno("save encoder");
			}
			if (encoder->crtc_id == test->crtc_id)
				test->old_connectors[test->old_connector_count++] =
					connector->connector_id;
			drmModeFreeEncoder(encoder);
		}
		drmModeFreeConnector(connector);
	}
	if (test->old_crtc->mode_valid &&
	    (!test->old_crtc->buffer_id || !test->old_connector_count)) {
		fprintf(stderr, "FAIL active original CRTC cannot be restored\n");
		return -1;
	}
	printf("PASS save: active=%d fb=%u connectors=%d\n",
	       test->old_crtc->mode_valid, test->old_crtc->buffer_id,
	       test->old_connector_count);
	return 0;
}

static uint64_t buffer_hash(const struct buffer *buffer)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	size_t i;

	for (i = 0; i < buffer->size; i++) {
		hash ^= buffer->map[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int create_buffer(struct test *test, struct buffer *buffer,
			 const char *name, uint32_t width, uint32_t height,
			 uint32_t format)
{
	struct drm_mode_create_dumb create = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	size_t i;

	buffer->name = name;
	buffer->width = width;
	buffer->height = height;
	create.width = width;
	create.height = height;
	create.bpp = 32;
	if (drmIoctl(test->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
		return fail_errno("CREATE_DUMB");
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	if (!create.size || create.size > SIZE_MAX ||
	    create.pitch < width * 4 || create.pitch % 4 ||
	    create.size < (uint64_t)create.pitch * height) {
		fprintf(stderr, "FAIL %s invalid pitch/size: %u/%" PRIu64 "\n",
			name, create.pitch, (uint64_t)create.size);
		return -1;
	}
	buffer->size = (size_t)create.size;
	map.handle = buffer->handle;
	if (drmIoctl(test->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return fail_errno("MAP_DUMB");
	if (map.offset > INT64_MAX) {
		fprintf(stderr, "FAIL %s mmap offset exceeds off_t\n", name);
		return -1;
	}
	buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, test->fd, (off_t)map.offset);
	if (buffer->map == MAP_FAILED) {
		buffer->map = NULL;
		return fail_errno("mmap dumb buffer");
	}
	for (i = 0; i < buffer->size; i++) {
		if (buffer->map[i]) {
			fprintf(stderr, "FAIL %s initial-zero: offset=%zu byte=%02x\n",
				name, i, buffer->map[i]);
			return -1;
		}
	}
	printf("PASS %s initial-zero: %zu bytes hash=%016" PRIx64 "\n",
	       name, buffer->size, buffer_hash(buffer));
	handles[0] = buffer->handle;
	pitches[0] = buffer->pitch;
	if (drmModeAddFB2(test->fd, width, height, format, handles, pitches,
			 offsets, &buffer->fb_id, 0))
		return fail_errno("drmModeAddFB2");
	printf("PASS %s create/map/AddFB2: handle=%u fb=%u %ux%u pitch=%u\n",
	       name, buffer->handle, buffer->fb_id, width, height, buffer->pitch);
	return 0;
}

static void fill_buffer(struct buffer *buffer, unsigned int pattern)
{
	static const uint32_t colors[] = {
		0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
		0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00202020,
	};
	uint32_t x, y, pixel;

	for (y = 0; y < buffer->height; y++) {
		uint32_t *row = (uint32_t *)(buffer->map +
					   (size_t)y * buffer->pitch);

		for (x = 0; x < buffer->width; x++) {
			if (pattern == 0)
				pixel = colors[x * 8 / buffer->width];
			else if (pattern == 1)
				pixel = ((x / 32 + y / 32) & 1) ?
					0x00e0e0e0 : 0x00303030;
			else if (x < 3 || y < 3 || x >= CURSOR_SIZE - 3 ||
				 y >= CURSOR_SIZE - 3)
				pixel = 0xffff0000;
			else if (x == y || x + y == CURSOR_SIZE - 1)
				pixel = 0xffffffff;
			else
				pixel = 0;
			row[x] = pixel;
		}
	}
	printf("PASS %s fill: pattern=%u hash=%016" PRIx64 "\n",
	       buffer->name, pattern, buffer_hash(buffer));
}

static int wait_event(struct test *test, uint32_t type, uint64_t cookie,
		      struct drm_event_vblank *result)
{
	struct pollfd pfd = { .fd = test->fd, .events = POLLIN };
	uint8_t data[4096];
	struct drm_event header;
	struct drm_event_vblank event;
	int64_t start = monotonic_ms(), now, remaining;
	size_t offset;
	ssize_t count;
	int ret;

	if (start < 0)
		return fail_errno("clock_gettime");
	while (!interrupted) {
		now = monotonic_ms();
		if (now < 0)
			return fail_errno("clock_gettime");
		remaining = EVENT_TIMEOUT_MS - (now - start);
		if (remaining <= 0) {
			fprintf(stderr, "FAIL event timeout after %d ms: "
				"type=%u cookie=%" PRIu64 "\n",
				EVENT_TIMEOUT_MS, type, cookie);
			return -1;
		}
		ret = poll(&pfd, 1, (int)remaining);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return fail_errno("poll DRM event");
		}
		if (!ret)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "FAIL DRM event poll revents=0x%x\n",
				pfd.revents);
			return -1;
		}
		count = read(test->fd, data, sizeof(data));
		if (count < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return fail_errno("read DRM event");
		}
		if (!count) {
			fprintf(stderr, "FAIL DRM event EOF\n");
			return -1;
		}
		for (offset = 0; offset < (size_t)count; offset += header.length) {
			if ((size_t)count - offset < sizeof(header))
				goto malformed;
			memcpy(&header, data + offset, sizeof(header));
			if (header.length < sizeof(header) ||
			    header.length > (size_t)count - offset)
				goto malformed;
			if (header.type != type)
				continue;
			if (header.length < sizeof(event))
				goto malformed;
			memcpy(&event, data + offset, sizeof(event));
			if (event.user_data != cookie)
				continue;
			if ((event.crtc_id && event.crtc_id != test->crtc_id) ||
			    event.tv_usec >= 1000000)
				goto malformed;
			*result = event;
			return 0;
		}
	}
	return -1;

malformed:
	fprintf(stderr, "FAIL malformed or mismatched DRM event\n");
	return -1;
}

static int test_flips(struct test *test)
{
	drmVBlank vblank = { 0 };
	struct drm_event_vblank event;
	drmModeCrtc *crtc;
	uint32_t previous, fb_id;
	uintptr_t cookie;
	int64_t start;
	unsigned int i;

	vblank.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE |
		DRM_VBLANK_EVENT | (test->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT));
	vblank.request.sequence = 1;
	vblank.request.signal = 1;
	if (drmWaitVBlank(test->fd, &vblank))
		return fail_errno("drmWaitVBlank(EVENT)");
	if (wait_event(test, DRM_EVENT_VBLANK, 1, &event))
		return -1;
	previous = event.sequence;
	printf("PASS vblank: seq=%u timestamp=%u.%06u CRTC=%u\n",
	       event.sequence, event.tv_sec, event.tv_usec, event.crtc_id);

	for (i = 0; i < FLIP_COUNT && !interrupted; i++) {
		fb_id = test->buffers[(i + 1) % 2].fb_id;
		cookie = 0x100 + i;
		start = monotonic_ms();
		if (start < 0)
			return fail_errno("clock_gettime");
		if (drmModePageFlip(test->fd, test->crtc_id, fb_id,
				    DRM_MODE_PAGE_FLIP_EVENT, (void *)cookie))
			return fail_errno("drmModePageFlip(EVENT)");
		if (wait_event(test, DRM_EVENT_FLIP_COMPLETE, cookie, &event))
			return -1;
		if ((int32_t)(event.sequence - previous) <= 0) {
			fprintf(stderr, "FAIL flip %u sequence did not advance: "
				"%u -> %u\n", i + 1, previous, event.sequence);
			return -1;
		}
		previous = event.sequence;
		crtc = drmModeGetCrtc(test->fd, test->crtc_id);
		if (!crtc)
			return fail_errno("read CRTC after flip");
		if (crtc->buffer_id != fb_id) {
			fprintf(stderr, "FAIL flip %u reported fb=%u expected=%u\n",
				i + 1, crtc->buffer_id, fb_id);
			drmModeFreeCrtc(crtc);
			return -1;
		}
		drmModeFreeCrtc(crtc);
		printf("PASS flip %u/%d: fb=%u seq=%u timestamp=%u.%06u "
		       "elapsed=%" PRId64 " ms\n", i + 1, FLIP_COUNT, fb_id,
		       event.sequence, event.tv_sec, event.tv_usec,
		       monotonic_ms() - start);
	}
	return interrupted ? -1 : 0;
}

static int test_cursor(struct test *test)
{
	struct buffer *cursor = &test->buffers[2];

	if (cursor->pitch != CURSOR_SIZE * 4) {
		fprintf(stderr, "SKIP legacy cursor: pitch=%u, API requires %u\n",
			cursor->pitch, CURSOR_SIZE * 4);
		test->cursor_skipped = true;
		return observe_ms(1500);
	}
	if (drmModeSetCursor2(test->fd, test->crtc_id, cursor->handle,
			      CURSOR_SIZE, CURSOR_SIZE, 0, 0)) {
		if (errno == ENOSYS || errno == EOPNOTSUPP || errno == ENOTTY) {
			printf("SKIP legacy cursor: %s\n", strerror(errno));
			test->cursor_skipped = true;
			return observe_ms(1500);
		}
		return fail_errno("drmModeSetCursor2");
	}
	test->cursor_enabled = true;
	printf("PASS ARGB cursor: 64x64 handle=%u\n", cursor->handle);
	if (drmModeMoveCursor(test->fd, test->crtc_id, 100, 100))
		return fail_errno("cursor move (100,100)");
	printf("PASS cursor move: (100,100)\n");
	if (observe_ms(750))
		return -1;
	if (drmModeMoveCursor(test->fd, test->crtc_id, -16, -16))
		return fail_errno("cursor move (-16,-16)");
	printf("PASS cursor move: (-16,-16)\n");
	return observe_ms(750);
}

static int destroy_buffer(int fd, struct buffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = { .handle = buffer->handle };
	int ret = 0;

	if (buffer->fb_id && drmModeRmFB(fd, buffer->fb_id))
		ret = fail_errno("drmModeRmFB");
	if (buffer->map && munmap(buffer->map, buffer->size))
		ret = fail_errno("munmap dumb buffer");
	if (buffer->handle && drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy))
		ret = fail_errno("DESTROY_DUMB");
	if (buffer->handle && !ret)
		printf("PASS %s cleanup: RmFB/munmap/destroy\n", buffer->name);
	return ret;
}

static int cleanup(struct test *test)
{
	bool detached = !test->modeset_attempted;
	bool restore_allowed = true;
	int i, ret = 0;

	if (test->cursor_enabled) {
		if (drmModeSetCursor2(test->fd, test->crtc_id, 0, 0, 0, 0, 0)) {
			ret = fail_errno("disable cursor");
			restore_allowed = false;
		} else {
			printf("PASS cursor disable\n");
		}
	}
	if (test->modeset_attempted) {
		/* Drain pending scanout before any framebuffer/BO destruction. */
		if (drmModeSetCrtc(test->fd, test->crtc_id, 0, 0, 0, NULL, 0, NULL)) {
			ret = fail_errno("disable CRTC before cleanup");
		} else {
			detached = true;
			printf("PASS CRTC disable: test scanout stopped\n");
		}
		if (detached && restore_allowed && test->old_crtc->mode_valid) {
			if (drmModeSetCrtc(test->fd, test->crtc_id,
					   test->old_crtc->buffer_id,
					   test->old_crtc->x, test->old_crtc->y,
					   test->old_connectors,
					   test->old_connector_count,
					   &test->old_crtc->mode))
				ret = fail_errno("restore original CRTC");
			else
				printf("PASS original CRTC/routing restored\n");
		}
	}
	if (detached) {
		for (i = 2; i >= 0; i--)
			if (destroy_buffer(test->fd, &test->buffers[i]))
				ret = -1;
	} else {
		/* Kernel KMS references retain active BOs after file release. */
		fprintf(stderr, "FAIL scanout detach unconfirmed: skip explicit "
			"RmFB/munmap/DESTROY_DUMB; leaving references to DRM "
			"file release\n");
	}
	free(test->old_connectors);
	if (test->old_crtc)
		drmModeFreeCrtc(test->old_crtc);
	if (test->connector)
		drmModeFreeConnector(test->connector);
	if (test->resources)
		drmModeFreeResources(test->resources);
	return ret;
}

int main(int argc, char **argv)
{
	struct test test = { .fd = -1 };
	struct sigaction action = { .sa_handler = handle_signal };
	const char *node = "/dev/dri/card0";
	drmVersionPtr version;
	uint64_t capability;
	bool have_node = false, no_cursor = false, master = false;
	int i, ret = EXIT_FAILURE;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-cursor"))
			no_cursor = true;
		else if (argv[i][0] != '-' && !have_node) {
			node = argv[i];
			have_node = true;
		} else {
			fprintf(stderr, "Usage: %s [DRM-node] [--no-cursor]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL)) {
		fail_errno("sigaction");
		return EXIT_FAILURE;
	}
	test.fd = open(node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (test.fd < 0) {
		fail_errno("open DRM node");
		return EXIT_FAILURE;
	}
	version = drmGetVersion(test.fd);
	if (!version) {
		fail_errno("drmGetVersion");
		goto out;
	}
	printf("INFO node=%s driver=%.*s version=%d.%d.%d\n", node,
	       version->name_len, version->name, version->version_major,
	       version->version_minor, version->version_patchlevel);
	drmFreeVersion(version);
	if (drmSetMaster(test.fd)) {
		fail_errno("drmSetMaster (exclusive KMS ownership required)");
		goto out;
	}
	master = true;
	if (drmGetCap(test.fd, DRM_CAP_DUMB_BUFFER, &capability)) {
		fail_errno("DRM_CAP_DUMB_BUFFER");
		goto out;
	}
	if (!capability) {
		fprintf(stderr, "FAIL dumb buffers unsupported\n");
		goto out;
	}
	printf("PASS DRM master and dumb-buffer capability\n");
	if (select_output(&test) || save_state(&test))
		goto out;
	if (create_buffer(&test, &test.buffers[0], "color-bars", WIDTH, HEIGHT,
			  DRM_FORMAT_XRGB8888) ||
	    create_buffer(&test, &test.buffers[1], "checkerboard", WIDTH, HEIGHT,
			  DRM_FORMAT_XRGB8888) ||
	    create_buffer(&test, &test.buffers[2], "cursor", CURSOR_SIZE,
			  CURSOR_SIZE, DRM_FORMAT_ARGB8888))
		goto out;
	for (i = 0; i < 3; i++)
		fill_buffer(&test.buffers[i], (unsigned int)i);
	if (interrupted)
		goto out;
	test.modeset_attempted = true;
	if (drmModeSetCrtc(test.fd, test.crtc_id, test.buffers[0].fb_id, 0, 0,
			   &test.connector->connector_id, 1, &test.mode)) {
		fail_errno("drmModeSetCrtc");
		goto out;
	}
	printf("PASS modeset: color-bars fb=%u\n", test.buffers[0].fb_id);
	if (observe_ms(500) || test_flips(&test))
		goto out;
	if (no_cursor) {
		test.cursor_skipped = true;
		printf("SKIP cursor operations: --no-cursor\n");
		if (observe_ms(1500))
			goto out;
	} else if (test_cursor(&test)) {
		goto out;
	}
	ret = EXIT_SUCCESS;
out:
	if (cleanup(&test))
		ret = EXIT_FAILURE;
	if (master && drmDropMaster(test.fd)) {
		fail_errno("drmDropMaster");
		ret = EXIT_FAILURE;
	}
	if (close(test.fd)) {
		fail_errno("close DRM node");
		ret = EXIT_FAILURE;
	}
	if (interrupted) {
		fprintf(stderr, "FAIL interrupted by signal %d\n", interrupted);
		ret = EXIT_FAILURE;
	}
	printf("%s result: modeset/vblank/%d flips/buffer cleanup; cursor=%s\n",
	       ret == EXIT_SUCCESS ? "PASS" : "FAIL", FLIP_COUNT,
	       test.cursor_skipped ? "SKIP" :
	       test.cursor_enabled && ret == EXIT_SUCCESS ? "PASS" : "not passed");
	return ret;
}
