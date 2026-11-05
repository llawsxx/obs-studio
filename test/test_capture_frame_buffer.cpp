#include "capture-frame-buffer.h"
#include "graphics-hook-info.h"
#include <winrt-capture.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <filesystem>

OBS_DECLARE_MODULE()

const char *obs_module_text(const char *text)
{
	return text;
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static constexpr uint64_t interval = 16666666;
static constexpr uint64_t epoch = 10000000000ULL;

static capture_frame_buffer make_buffer(bool rebuild = false)
{
	capture_frame_buffer buffer = {};
	buffer.config = {true, 2, 5, rebuild, 60, 1};
	return buffer;
}

static void push(capture_frame_buffer &buffer, uint64_t timestamp, uint8_t value, uint32_t size = 4)
{
	std::array<uint8_t, 8 * 8 * 4> pixels;
	pixels.fill(value);
	const uint8_t *data = pixels.data();
	gs_texture_t *texture = gs_texture_create(size, size, GS_BGRA, 1, &data, 0);
	CHECK(texture);
	capture_frame_buffer_push(&buffer, texture, timestamp);
	gs_texture_destroy(texture);
}

static uint8_t value_of(gs_texture_t *texture)
{
	CHECK(texture);
	gs_stagesurf_t *stage =
		gs_stagesurface_create(gs_texture_get_width(texture), gs_texture_get_height(texture), GS_BGRA);
	CHECK(stage);
	gs_stage_texture(stage, texture);
	uint8_t *pixels = nullptr;
	uint32_t stride = 0;
	CHECK(gs_stagesurface_map(stage, &pixels, &stride));
	const uint8_t value = pixels[0];
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
	return value;
}

static void test_prefill_hold_overflow(bool rebuild)
{
	auto buffer = make_buffer(rebuild);
	push(buffer, epoch, 1);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch, interval));
	CHECK(!buffer.selected && buffer.count == 1);
	push(buffer, epoch + interval, 2);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 2 * interval, interval));
	CHECK(value_of(buffer.selected) == 1 && buffer.count == 1);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch + 2 * interval + 1000, interval));
	for (uint8_t value = 3; value <= 7; value++) {
		push(buffer, epoch + uint64_t(value - 1) * interval, value);
	}
	CHECK(buffer.count == 2 && buffer.discarded == 4);
	CHECK(value_of(buffer.selected) == 1);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 9 * interval, interval));
	CHECK(value_of(buffer.selected) == 6);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 10 * interval, interval));
	CHECK(value_of(buffer.selected) == 7 && buffer.count == 0);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch + 11 * interval, interval));
	push(buffer, epoch + 7 * interval, 8);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch + 12 * interval, interval));
	CHECK(value_of(buffer.selected) == 7);
	push(buffer, epoch + 8 * interval, 9);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 13 * interval, interval));
	CHECK(value_of(buffer.selected) == 8);
	capture_frame_buffer_free(&buffer);
}

static void test_limits_resize_discontinuity()
{
	for (uint32_t count : {1U, 2U, 30U}) {
		auto buffer = make_buffer();
		buffer.config.target_frames = buffer.config.max_frames = count;
		for (uint32_t index = 0; index <= count; index++) {
			push(buffer, epoch + index * interval, uint8_t(index));
		}
		CHECK(buffer.count == count && buffer.discarded == 1);
		CHECK(capture_frame_buffer_select_at(&buffer, epoch + 100 * interval, interval));
		CHECK(value_of(buffer.selected) == 1);
		push(buffer, epoch + 101 * interval, 42, 8);
		CHECK(buffer.count == 1 && !buffer.selected && buffer.width == 8);
		push(buffer, epoch - 1, 43, 8);
		CHECK(buffer.count == 1 && buffer.timestamps[0] == epoch - 1);
		capture_frame_buffer_free(&buffer);
	}
}

static void test_static_start()
{
	auto buffer = make_buffer();
	push(buffer, epoch, 11);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch + 200000000, interval));
	CHECK(!buffer.selected);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 300000000, interval));
	CHECK(value_of(buffer.selected) == 11 && buffer.count == 0 && !buffer.clock.playing);
	push(buffer, epoch + 301000000, 12);
	CHECK(!capture_frame_buffer_select_at(&buffer, epoch + 302000000, interval));
	push(buffer, epoch + 318000000, 13);
	CHECK(capture_frame_buffer_select_at(&buffer, epoch + 319000000, interval));
	CHECK(value_of(buffer.selected) == 12);
	capture_frame_buffer_free(&buffer);
}

static void test_jittered_arrivals(bool rebuild)
{
	for (uint64_t phase : {1ULL, interval / 2, interval - 1}) {
		auto buffer = make_buffer(rebuild);
		uint64_t next_frame = 0;
		uint64_t previous_delivered = 0;
		for (uint64_t tick = 0; tick < 18000; tick++) {
			const uint64_t output = epoch + tick * interval;
			while (true) {
				const uint64_t jitter = next_frame % 2 ? interval / 3 : 0;
				const uint64_t timestamp = epoch + next_frame * interval;
				if (timestamp + phase + jitter > output) {
					break;
				}
				push(buffer, timestamp, uint8_t(next_frame));
				next_frame++;
			}
			capture_frame_buffer_select_at(&buffer, output, interval);
			if (tick > 5) {
				CHECK(buffer.delivered == previous_delivered + 1);
			}
			previous_delivered = buffer.delivered;
		}
		CHECK(buffer.discarded == 0);
		capture_frame_buffer_free(&buffer);
	}
}

static void test_capture_schedule()
{
	auto buffer = make_buffer(true);
	buffer.config.fps_numerator = 60000;
	buffer.config.fps_denominator = 1001;
	CHECK(capture_frame_buffer_due(&buffer, epoch));
	for (uint64_t index = 1; index <= 60000; index++) {
		const uint64_t due = epoch + index * 1001000000000ULL / 60000;
		CHECK(buffer.next_capture == due);
		CHECK(!capture_frame_buffer_due(&buffer, due - 1));
		CHECK(capture_frame_buffer_due(&buffer, due + 500000));
	}
	const uint64_t stalled = buffer.next_capture + 100000000;
	CHECK(capture_frame_buffer_due(&buffer, stalled));
	CHECK(buffer.next_capture > stalled);
	CHECK(!capture_frame_buffer_due(&buffer, stalled));
}

static void test_selection_window(bool rebuild)
{
	const uint64_t period = 20000000;
	for (int64_t difference : {-12000001LL, -12000000LL, 12000000LL, 12000001LL}) {
		auto buffer = make_buffer(rebuild);
		buffer.config.fps_numerator = 50;
		for (uint8_t index = 0; index < 3; index++) {
			push(buffer, epoch + index * period, index + 1);
		}
		CHECK(capture_frame_buffer_select_at(&buffer, epoch, period));
		const uint64_t output = uint64_t(int64_t(epoch + period) - difference);
		const bool selected = capture_frame_buffer_select_at(&buffer, output, period);
		CHECK(selected == (difference <= 12000000));
		if (selected) {
			CHECK(value_of(buffer.selected) == (difference < -12000000 ? 3 : 2));
		} else {
			CHECK(value_of(buffer.selected) == 1);
		}
		capture_frame_buffer_free(&buffer);
	}
}

static void test_hook_timestamps()
{
	hook_frame_info frame = {};
	CHECK(hook_frame_timestamp(&frame) == 0);
	std::thread writer([&frame] {
		for (uint32_t index = 1; index <= 100000; index++) {
			hook_frame_publish(&frame, (uint64_t(index) << 32) | (index ^ 0xa5a5a5a5U));
		}
	});
	for (uint32_t index = 0; index < 100000; index++) {
		const uint64_t timestamp = hook_frame_timestamp(&frame);
		if (timestamp) {
			CHECK(uint32_t(timestamp) == (uint32_t(timestamp >> 32) ^ 0xa5a5a5a5U));
		}
	}
	writer.join();
	CHECK(uint32_t(hook_frame_timestamp(&frame) >> 32) == 100000);
}

static void capture_count(void *param)
{
	InterlockedIncrement(static_cast<volatile LONG *>(param));
}

static void test_worker()
{
	for (int iteration = 0; iteration < 10; iteration++) {
		volatile LONG calls = 0;
		capture_worker worker = {};
		CHECK(capture_worker_init(&worker, capture_count, const_cast<LONG *>(&calls)));
		CHECK(!worker.thread);
		CHECK(capture_worker_set_enabled(&worker, true));
		const uint64_t until = os_gettime_ns() + 1000000000;
		while (!InterlockedCompareExchange(&calls, 0, 0) && os_gettime_ns() < until) {
			Sleep(1);
		}
		CHECK(InterlockedCompareExchange(&calls, 0, 0) > 0);
		capture_worker_free(&worker);
	}
}

static void test_settings()
{
	obs_data_t *settings = obs_data_create();
	capture_frame_buffer_defaults(settings);
	CHECK(!obs_data_get_bool(settings, "capture_buffer_enabled"));
	CHECK(obs_data_get_int(settings, "capture_buffer_target_frames") == 2);
	CHECK(obs_data_get_int(settings, "capture_buffer_max_frames") == 5);
	capture_worker worker = {};
	volatile LONG calls = 0;
	CHECK(capture_worker_init(&worker, capture_count, const_cast<LONG *>(&calls)));
	worker.event_driven = true;
	capture_frame_buffer buffer = {};
	capture_frame_buffer_update(&buffer, &worker, settings);
	CHECK(!buffer.config.enabled && !worker.thread && !buffer.loss_registered);
	obs_data_set_bool(settings, "capture_buffer_enabled", true);
	capture_frame_buffer_update(&buffer, &worker, settings);
	CHECK(buffer.config.enabled && !worker.thread && buffer.loss_registered);
	push(buffer, epoch, 17);
	obs_data_set_bool(settings, "capture_buffer_enabled", false);
	capture_frame_buffer_update(&buffer, &worker, settings);
	CHECK(!buffer.config.enabled && !buffer.count && !buffer.selected);
	for (auto *texture : buffer.textures) {
		CHECK(!texture);
	}
	capture_frame_buffer_free(&buffer);
	CHECK(!buffer.loss_registered);
	capture_worker_free(&worker);
	obs_data_release(settings);
}

static void test_desktop_duplication()
{
	gs_duplicator_t *first = gs_duplicator_create(0);
	if (!first) {
		puts("Desktop Duplication integration unavailable on this adapter");
		return;
	}
	gs_duplicator_t *second = gs_duplicator_create(0);
	CHECK(second == first);
	uint64_t timestamp = 0;
	for (int attempt = 0; attempt < 1000 && !timestamp; attempt++) {
		CHECK(gs_duplicator_poll_frame(first, &timestamp));
		if (!timestamp) {
			Sleep(1);
		}
	}
	gs_texture_t *texture = gs_duplicator_get_texture(first);
	CHECK(texture && timestamp);
	auto first_buffer = make_buffer();
	auto second_buffer = make_buffer();
	capture_frame_buffer_push(&first_buffer, texture, timestamp);
	uint64_t next_timestamp = 0;
	CHECK(gs_duplicator_poll_frame(second, &next_timestamp));
	CHECK(next_timestamp >= timestamp);
	capture_frame_buffer_push(&second_buffer, gs_duplicator_get_texture(second), next_timestamp);
	CHECK(first_buffer.count == 1 && second_buffer.count == 1);
	CHECK(first_buffer.textures[first_buffer.slots[0]] != second_buffer.textures[second_buffer.slots[0]]);
	capture_frame_buffer_free(&first_buffer);
	capture_frame_buffer_free(&second_buffer);
	gs_duplicator_destroy(second);
	gs_duplicator_destroy(first);
	puts("Desktop Duplication polling and independent consumer queues passed");
}

struct wgc_test {
	decltype(&winrt_capture_init_monitor) init;
	decltype(&winrt_capture_free) free;
	decltype(&winrt_capture_set_frame_callback) set_callback;
	decltype(&winrt_capture_render_texture) render;
	winrt_capture *capture = nullptr;
	capture_frame_buffer buffer = make_buffer();
	bool select = false;
};

static void wgc_task(void *param)
{
	auto &test = *static_cast<wgc_test *>(param);
	obs_enter_graphics();
	if (!test.select) {
		const POINT point = {};
		test.capture = test.init(false, MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), true);
		if (test.capture) {
			test.set_callback(test.capture, capture_frame_buffer_push, &test.buffer);
		}
	} else if (test.capture) {
		capture_frame_buffer_select_at(&test.buffer, obs_get_video_frame_time(), obs_get_frame_interval_ns());
		if (test.buffer.selected) {
			gs_texrender_t *target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
			const uint32_t width = gs_texture_get_width(test.buffer.selected);
			const uint32_t height = gs_texture_get_height(test.buffer.selected);
			CHECK(target && gs_texrender_begin(target, width, height));
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
			test.render(test.capture, test.buffer.selected);
			gs_texrender_end(target);
			gs_texrender_destroy(target);
		}
	}
	obs_leave_graphics();
}

static void wgc_free_task(void *param)
{
	auto &test = *static_cast<wgc_test *>(param);
	obs_enter_graphics();
	test.free(test.capture);
	capture_frame_buffer_free(&test.buffer);
	obs_leave_graphics();
}

static void test_wgc(const char *backend)
{
	const auto path = std::filesystem::path(backend).parent_path() / "libobs-winrt.dll";
	void *module = os_dlopen(path.string().c_str());
	CHECK(module);
	wgc_test test;
	test.init = reinterpret_cast<decltype(test.init)>(os_dlsym(module, "winrt_capture_init_monitor"));
	test.free = reinterpret_cast<decltype(test.free)>(os_dlsym(module, "winrt_capture_free"));
	test.set_callback =
		reinterpret_cast<decltype(test.set_callback)>(os_dlsym(module, "winrt_capture_set_frame_callback"));
	test.render = reinterpret_cast<decltype(test.render)>(os_dlsym(module, "winrt_capture_render_texture"));
	CHECK(test.init && test.free && test.set_callback && test.render);
	for (int iteration = 0; iteration < 3; iteration++) {
		test.select = false;
		test.capture = nullptr;
		obs_queue_task(OBS_TASK_GRAPHICS, wgc_task, &test, true);
		if (!test.capture) {
			puts("WGC integration unavailable on this desktop");
			break;
		}
		test.select = true;
		for (int tick = 0; tick < 30; tick++) {
			Sleep(17);
			obs_queue_task(OBS_TASK_GRAPHICS, wgc_task, &test, true);
		}
		obs_enter_graphics();
		CHECK(test.buffer.received && test.buffer.selected);
		printf("WGC iteration %d: received=%llu, selected=%llu\n", iteration,
		       (unsigned long long)test.buffer.received, (unsigned long long)test.buffer.delivered);
		obs_leave_graphics();
		obs_queue_task(OBS_TASK_GRAPHICS, wgc_free_task, &test, true);
	}
	os_dlclose(module);
}

int main(int count, char **arguments)
{
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	setvbuf(stdout, nullptr, _IONBF, 0);
	CHECK(count == 2);
	CHECK(obs_startup("en-US", nullptr, nullptr));
	obs_video_info video = {};
	video.graphics_module = arguments[1];
	video.fps_num = 60;
	video.fps_den = 1;
	video.base_width = video.output_width = 64;
	video.base_height = video.output_height = 64;
	video.output_format = VIDEO_FORMAT_NV12;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	CHECK(obs_reset_video(&video) == OBS_VIDEO_SUCCESS);
	obs_enter_graphics();
	test_prefill_hold_overflow(false);
	test_prefill_hold_overflow(true);
	test_limits_resize_discontinuity();
	test_static_start();
	test_jittered_arrivals(false);
	test_jittered_arrivals(true);
	test_capture_schedule();
	test_selection_window(false);
	test_selection_window(true);
	test_hook_timestamps();
	test_settings();
	test_desktop_duplication();
	obs_leave_graphics();
	test_worker();
	test_wgc(arguments[1]);
	obs_shutdown();
	puts("Capture GPU queue, phase/jitter, prefill, overflow, static image and worker lifecycle checks passed");
}
