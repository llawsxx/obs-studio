#include "output.hpp"
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static void test_output(dvc::PixelFormat format, bool follow, bool card, bool use_obs_interval = false)
{
	video_output_info info = {};
	info.name = "DVC test video";
	info.format = VIDEO_FORMAT_BGRA;
	info.fps_num = 60000;
	info.fps_den = 1001;
	info.width = 1280;
	info.height = 720;
	info.cache_size = 6;
	info.colorspace = VIDEO_CS_709;
	info.range = VIDEO_RANGE_FULL;
	video_t *video = nullptr;
	CHECK(video_output_open(&video, &info) == VIDEO_OUTPUT_SUCCESS);
	GUID guid;
	wchar_t session[40];
	CHECK(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, session, 40));
	char session_text[40];
	CHECK(WideCharToMultiByte(CP_UTF8, 0, session, -1, session_text, sizeof(session_text), nullptr, nullptr));
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "session", session_text);
	obs_data_set_int(settings, "fps_num", 24);
	obs_data_set_int(settings, "fps_den", 1);
	obs_data_set_bool(settings, "follow_obs_fps", follow);
	obs_data_set_bool(settings, "use_obs_interval", use_obs_interval);
	obs_data_set_int(settings, "pixel_format", int(format));
	obs_data_set_int(settings, follow ? "max_frames" : "slots", follow ? 5 : 3);
	obs_data_set_int(settings, "target_frames", card ? 3 : 2);
	obs_data_set_int(settings, "test_card", card ? 1 : 0);
	obs_output_t *output = obs_output_create("dynamic_virtualcam_output", "DVC test output", settings, nullptr);
	obs_data_release(settings);
	CHECK(output);
	obs_output_set_media(output, video, nullptr);
	CHECK(obs_output_start(output));
	auto *context = static_cast<dvc::Output *>(obs_obj_get_data(output));
	CHECK(context && context->queue);
	CHECK(context->rate.numerator == ((follow || use_obs_interval) ? 60000U : 24U));
	CHECK(context->rate.denominator == ((follow || use_obs_interval) ? 1001U : 1U));
	CHECK(context->rate.fixed_interval == use_obs_interval);
	CHECK(context->queue->header()->fixed_interval == uint32_t(use_obs_interval));
	if (use_obs_interval) {
		CHECK(context->follow_obs_fps);
		CHECK(context->rate.offset(1) == video_output_get_frame_time(video));
		CHECK(context->rate.offset(60000) == video_output_get_frame_time(video) * 60000);
	}
	CHECK(context->queue->header()->pixel_format == uint32_t(format));
	CHECK(context->queue->header()->target_frames == (card ? 3U : 2U));
	CHECK(context->queue->header()->slots == (follow ? 5U : 3U));
	CHECK(obs_output_get_type_data(output) == nullptr);
	dvc::Queue reader;
	CHECK(reader.open(session));
	video_frame frame;
	const uint64_t source_timestamp = 123456789012345ULL;
	CHECK(video_output_lock_frame(video, &frame, 1, source_timestamp));
	for (uint32_t row = 0; row < info.height; ++row) {
		memset(frame.data[0] + size_t(row) * frame.linesize[0], 0, info.width * 4);
		for (uint32_t column = 0; column < info.width; ++column) {
			frame.data[0][size_t(row) * frame.linesize[0] + column * 4 + 3] = 255;
		}
	}
	video_output_unlock_frame(video);
	const uint64_t until = dvc::now_ns() + 3000000000;
	while (!dvc::load(reader.header()->published) && dvc::now_ns() < until) {
		Sleep(1);
	}
	CHECK(dvc::load(reader.header()->published) == 1);
	uint64_t consumed = 0, skipped = 0;
	std::vector<uint8_t> pixels;
	const uint64_t timestamp = dvc::load(reader.header()->frames[0].timestamp);
	CHECK(timestamp == source_timestamp);
	CHECK(reader.read(consumed, timestamp, pixels, skipped));
	CHECK(pixels.size() == dvc::image_size(format, info.width, info.height));
	if (card) {
		CHECK(pixels[0] > 128);
		CHECK(!context->card_failed);
	} else if (format == dvc::PixelFormat::NV12) {
		CHECK(pixels[0] == 16 && pixels[size_t(info.width) * info.height] == 128);
	} else if (format == dvc::PixelFormat::YUY2) {
		CHECK(pixels[0] == 16 && pixels[1] == 128 && pixels[2] == 16 && pixels[3] == 128);
	} else {
		CHECK(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0);
	}
	dvc::Handle stopped(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	CHECK(stopped);
	signal_handler_connect(
		obs_output_get_signal_handler(output), "deactivate",
		[](void *data, calldata_t *) { SetEvent(static_cast<HANDLE>(data)); }, stopped);
	obs_output_force_stop(output);
	CHECK(WaitForSingleObject(stopped, 3000) == WAIT_OBJECT_0);
	obs_output_release(output);
	video_output_close(video);
	printf("OBS producer fixed_interval=%d format=%u follow=%d card=%d: conversion, IPC and FPS passed\n",
	       use_obs_interval, uint32_t(format), follow, card);
}

struct BenchmarkStatistics {
	std::vector<uint64_t> values;
	void print(const char *name) const
	{
		auto sorted = values;
		std::sort(sorted.begin(), sorted.end());
		uint64_t sum = 0;
		for (const uint64_t value : sorted) {
			sum += value;
		}
		printf(" %s_mean_us=%.3f %s_p95_us=%.3f %s_max_us=%.3f", name, double(sum) / sorted.size() / 1000.0,
		       name, double(sorted[(sorted.size() - 1) * 95 / 100]) / 1000.0, name,
		       double(sorted.back()) / 1000.0);
	}
};

static void benchmark_output(video_format source_format, dvc::PixelFormat format, uint32_t clients)
{
	constexpr uint32_t width = 1920, height = 1080, warmup = 30, samples = 120;
	const dvc::Rate rate{60, 1, true};
	const auto destination_format = [](dvc::PixelFormat value) {
		switch (value) {
		case dvc::PixelFormat::NV12:
			return VIDEO_FORMAT_NV12;
		case dvc::PixelFormat::YUY2:
			return VIDEO_FORMAT_YUY2;
		case dvc::PixelFormat::RGB24:
			return VIDEO_FORMAT_BGR3;
		case dvc::PixelFormat::RGB32:
			return VIDEO_FORMAT_BGRX;
		case dvc::PixelFormat::ARGB32:
			return VIDEO_FORMAT_BGRA;
		}
		return VIDEO_FORMAT_NONE;
	}(format);
	video_scale_info from = {}, to = {};
	from.format = source_format;
	from.width = to.width = width;
	from.height = to.height = height;
	from.colorspace = to.colorspace = VIDEO_CS_709;
	from.range = source_format == VIDEO_FORMAT_NV12 ? VIDEO_RANGE_PARTIAL : VIDEO_RANGE_FULL;
	to.format = destination_format;
	to.range = format == dvc::PixelFormat::NV12 || format == dvc::PixelFormat::YUY2 ? VIDEO_RANGE_PARTIAL
											: VIDEO_RANGE_FULL;
	video_frame source = {};
	video_frame_init(&source, source_format, width, height);
	CHECK(source.data[0]);
	memset(source.data[0], 96, size_t(source.linesize[0]) * height);
	if (source_format == VIDEO_FORMAT_NV12) {
		memset(source.data[1], 128, size_t(source.linesize[1]) * height / 2);
	}
	video_scaler_t *scaler = nullptr;
	std::array<video_frame, 3> converted = {};
	if (source_format != destination_format) {
		CHECK(video_scaler_create(&scaler, &to, &from, VIDEO_SCALE_FAST_BILINEAR) == VIDEO_SCALER_SUCCESS);
		for (auto &frame : converted) {
			video_frame_init(&frame, destination_format, width, height);
			CHECK(frame.data[0]);
		}
	}
	GUID guid;
	wchar_t session[40];
	CHECK(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, session, 40));
	dvc::Queue writer;
	CHECK(writer.create(session, width, height, 5, rate, format));
	dvc::Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	CHECK(stop);
	std::array<dvc::Queue, 2> readers;
	std::array<std::thread, 2> threads;
	std::array<std::atomic<uint64_t>, 2> captured{};
	for (uint32_t index = 0; index < clients; ++index) {
		CHECK(readers[index].open(session));
		CHECK(readers[index].attach(index + 1) == int(index));
		threads[index] = std::thread([&, index] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
			HANDLE events[] = {stop, readers[index].frame_event(int(index))};
			uint64_t cursor = 0, generation_start = 0;
			dvc::FrameStamp stamp;
			std::vector<uint8_t> image;
			while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
				while (readers[index].next(cursor, stamp, generation_start, image)) {
					++captured[index];
				}
				const DWORD result = WaitForMultipleObjects(2, events, FALSE, 100);
				if (result != WAIT_OBJECT_0 + 1 && result != WAIT_TIMEOUT) {
					break;
				}
			}
		});
	}
	dvc::Handle timer(CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS));
	CHECK(timer);
	auto wait = [&](uint64_t deadline) {
		while (dvc::now_ns() < deadline) {
			const uint64_t current = dvc::now_ns();
			if (current >= deadline) {
				break;
			}
			LARGE_INTEGER due;
			due.QuadPart = -LONGLONG((deadline - current + 99) / 100);
			CHECK(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE));
			CHECK(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0);
		}
	};
	BenchmarkStatistics conversion, publishing, total, notification;
	uint64_t dropped = 0;
	const uint64_t epoch = dvc::now_ns();
	for (uint32_t index = 0; index < warmup + samples; ++index) {
		wait(epoch + rate.offset(index));
		source.data[0][index % width] ^= uint8_t(index + 1);
		const uint64_t start = dvc::now_ns();
		auto &frame = scaler ? converted[index % converted.size()] : source;
		if (scaler) {
			CHECK(video_scaler_scale(scaler, frame.data, frame.linesize, source.data, source.linesize));
		}
		const uint64_t converted_time = dvc::now_ns();
		const bool published =
			writer.write_planes(frame.data, frame.linesize, 1000000000000ULL + rate.offset(index));
		const uint64_t end = dvc::now_ns();
		if (index >= warmup) {
			conversion.values.push_back(scaler ? converted_time - start : 0);
			publishing.values.push_back(end - converted_time);
			total.values.push_back(end - start);
			dropped += !published;
		}
	}
	const uint64_t notify_epoch = dvc::now_ns();
	for (uint32_t index = 0; index < 30; ++index) {
		wait(notify_epoch + rate.offset(index));
		const uint64_t start = dvc::now_ns();
		for (uint32_t client = 0; client < dvc::max_clients; ++client) {
			CHECK(SetEvent(writer.frame_event(int(client))));
		}
		notification.values.push_back(dvc::now_ns() - start);
	}
	SetEvent(stop);
	for (uint32_t index = 0; index < clients; ++index) {
		threads[index].join();
		readers[index].detach(int(index), index + 1);
	}
	printf("BENCH source=%s destination=%u clients=%u samples=%u dropped=%llu",
	       source_format == VIDEO_FORMAT_NV12 ? "NV12" : "BGRA", uint32_t(format), clients, samples,
	       static_cast<unsigned long long>(dropped));
	conversion.print("conversion");
	publishing.print("publish");
	total.print("total");
	notification.print("notify32");
	for (uint32_t index = 0; index < clients; ++index) {
		printf(" captured%u=%llu", index, static_cast<unsigned long long>(captured[index].load()));
	}
	putchar('\n');
	video_scaler_destroy(scaler);
	for (auto &frame : converted) {
		video_frame_free(&frame);
	}
	video_frame_free(&source);
}

int main(int count, char **arguments)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	CHECK(obs_startup("en-US", nullptr, nullptr));
	if (count == 2 && strcmp(arguments[1], "--benchmark") == 0) {
		for (const auto source : {VIDEO_FORMAT_BGRA, VIDEO_FORMAT_NV12}) {
			for (const auto format :
			     {dvc::PixelFormat::NV12, dvc::PixelFormat::YUY2, dvc::PixelFormat::RGB24,
			      dvc::PixelFormat::RGB32, dvc::PixelFormat::ARGB32}) {
				for (uint32_t clients = 0; clients <= 2; ++clients) {
					benchmark_output(source, format, clients);
				}
			}
		}
		obs_shutdown();
		return 0;
	}
	obs_video_info video = {};
	video.graphics_module = "./libobs-d3d11";
	video.base_width = video.output_width = 16;
	video.base_height = video.output_height = 16;
	video.fps_num = 60000;
	video.fps_den = 1001;
	video.output_format = VIDEO_FORMAT_BGRA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	video.scale_type = OBS_SCALE_BILINEAR;
	CHECK(obs_reset_video(&video) == OBS_VIDEO_SUCCESS);
	dvc::Output::register_type();
	for (const auto format : {dvc::PixelFormat::NV12, dvc::PixelFormat::YUY2, dvc::PixelFormat::RGB24,
				  dvc::PixelFormat::RGB32, dvc::PixelFormat::ARGB32}) {
		test_output(format, true, false);
		test_output(format, false, false);
		test_output(format, true, true);
		test_output(format, false, false, true);
		test_output(format, false, true, true);
	}
	obs_shutdown();
	puts("OBS producer preconversion, follow/manual rational FPS and test card tests passed");
}
