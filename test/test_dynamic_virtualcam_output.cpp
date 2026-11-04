#include "output.hpp"
#include <media-io/video-frame.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static void test_output(dvc::PixelFormat format, bool follow, bool card)
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
	CHECK(context->rate.numerator == (follow ? 60000U : 24U));
	CHECK(context->rate.denominator == (follow ? 1001U : 1U));
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
	printf("OBS producer format=%u follow=%d card=%d: conversion, IPC and FPS passed\n", uint32_t(format), follow,
	       card);
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	CHECK(obs_startup("en-US", nullptr, nullptr));
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
	}
	obs_shutdown();
	puts("OBS producer preconversion, follow/manual rational FPS and test card tests passed");
}
