#include "output.hpp"

#include <util/bmem.h>
#include <cstring>
#include <algorithm>

namespace dvc {

static const char *output_name(void *)
{
	return "Dynamic Virtual Camera Output";
}

static video_format output_format(PixelFormat format)
{
	switch (format) {
	case PixelFormat::NV12:
		return VIDEO_FORMAT_NV12;
	case PixelFormat::YUY2:
		return VIDEO_FORMAT_YUY2;
	case PixelFormat::RGB24:
		return VIDEO_FORMAT_BGR3;
	case PixelFormat::RGB32:
		return VIDEO_FORMAT_BGRX;
	case PixelFormat::ARGB32:
		return VIDEO_FORMAT_BGRA;
	}
	return VIDEO_FORMAT_NONE;
}

static void *create_output(obs_data_t *settings, obs_output_t *output)
{
	try {
		auto result = std::make_unique<Output>();
		result->output = output;
		const char *session = obs_data_get_string(settings, "session");
		result->session.assign(session, session + strlen(session));
		result->rate = {uint32_t(obs_data_get_int(settings, "fps_num")),
				uint32_t(obs_data_get_int(settings, "fps_den"))};
		result->follow_obs_fps = !obs_data_has_user_value(settings, "follow_obs_fps") ||
					 obs_data_get_bool(settings, "follow_obs_fps");
		const uint32_t format = obs_data_has_user_value(settings, "pixel_format")
						? uint32_t(obs_data_get_int(settings, "pixel_format"))
						: uint32_t(PixelFormat::NV12);
		if (!valid_pixel_format(format)) {
			return nullptr;
		}
		result->pixel_format = PixelFormat(format);
		result->slots = obs_data_has_user_value(settings, "max_frames")
					? uint32_t(obs_data_get_int(settings, "max_frames"))
				: obs_data_has_user_value(settings, "slots")
					? uint32_t(obs_data_get_int(settings, "slots"))
					: 5;
		if (obs_data_has_user_value(settings, "target_frames")) {
			result->target_frames = uint32_t(obs_data_get_int(settings, "target_frames"));
		}
		result->phase_ns = uint64_t(std::max<int64_t>(0, obs_data_get_int(settings, "phase_ns")));
		result->card_mode = TestCardMode(std::clamp(int(obs_data_get_int(settings, "test_card")), 0, 1));
		return result.release();
	} catch (...) {
		return nullptr;
	}
}

static bool start_output(void *data)
{
	auto &context = *static_cast<Output *>(data);
	if (!obs_output_can_begin_data_capture(context.output, 0)) {
		return false;
	}
	const uint32_t width = obs_output_get_width(context.output), height = obs_output_get_height(context.output);
	const auto *video_info = video_output_get_info(obs_output_video(context.output));
	context.producer_rate = {video_info->fps_num, video_info->fps_den};
	if (context.follow_obs_fps) {
		context.rate = context.producer_rate;
	}
	context.card.reset();
	context.card_sequence = 0;
	context.card_failed = false;
	try {
		context.queue = std::make_unique<Queue>();
		if (!context.queue->create(context.session, width, height, context.slots, context.rate,
					   context.pixel_format, context.target_frames)) {
			context.queue.reset();
			return false;
		}
		store(context.queue->header()->phase, std::min(context.phase_ns, context.rate.offset(1) - 1));
	} catch (...) {
		context.queue.reset();
		return false;
	}
	video_scale_info conversion = {};
	conversion.format = output_format(context.pixel_format);
	conversion.width = width;
	conversion.height = height;
	conversion.colorspace = VIDEO_CS_709;
	conversion.range = context.pixel_format == PixelFormat::NV12 || context.pixel_format == PixelFormat::YUY2
				   ? VIDEO_RANGE_PARTIAL
				   : VIDEO_RANGE_FULL;
	obs_output_set_video_conversion(context.output, &conversion);
	if (!obs_output_begin_data_capture(context.output, 0)) {
		context.queue.reset();
		return false;
	}
	blog(LOG_INFO, "[dynamic-virtualcam] Started %ux%u, %u/%u FPS, target=%u max=%u, format=%u, follow_obs_fps=%d",
	     width, height, context.rate.numerator, context.rate.denominator, context.target_frames, context.slots,
	     uint32_t(context.pixel_format), context.follow_obs_fps);
	return true;
}

static void stop_output(void *data, uint64_t)
{
	auto &context = *static_cast<Output *>(data);
	if (context.queue) {
		store(context.queue->header()->active, 0);
	}
	obs_output_end_data_capture(context.output);
	blog(LOG_INFO, "[dynamic-virtualcam] Stopped");
}

static void raw_video(void *data, video_data *frame)
{
	auto &context = *static_cast<Output *>(data);
	if (!context.queue) {
		return;
	}
	++context.card_sequence;
	const TestCardMode mode = context.card_mode.load();
	if (mode == TestCardMode::Off) {
		context.card_failed = false;
		context.queue->write_planes(frame->data, frame->linesize, frame->timestamp);
		return;
	}
	try {
		const auto *header = context.queue->header();
		if (!context.card) {
			context.card = std::make_unique<TestCard>(header->width, header->height);
		}
		const CardFrame information{context.card_sequence,
					    context.producer_rate.offset(context.card_sequence - 1), frame->timestamp,
					    context.producer_rate, context.rate};
		const uint8_t *image = context.card->draw(mode, information);
		if (image) {
			context.card_pixels.resize(image_size(context.pixel_format, header->width, header->height));
			convert(image, context.card_pixels.data(), context.pixel_format, header->width, header->height,
				false);
			context.queue->write(context.card_pixels.data(),
					     image_stride(context.pixel_format, header->width), frame->timestamp);
			context.card_failed = false;
			return;
		}
	} catch (...) {
	}
	if (!context.card_failed.exchange(true)) {
		blog(LOG_ERROR, "[dynamic-virtualcam] Could not draw test card; real content is not substituted");
	}
}

void Output::register_type()
{
	obs_output_info info = {};
	info.id = "dynamic_virtualcam_output";
	info.flags = OBS_OUTPUT_VIDEO;
	info.get_name = output_name;
	info.create = create_output;
	info.destroy = [](void *data) {
		delete static_cast<Output *>(data);
	};
	info.start = start_output;
	info.stop = stop_output;
	info.raw_video = raw_video;
	info.update = [](void *data, obs_data_t *settings) {
		auto &context = *static_cast<Output *>(data);
		if (obs_data_has_user_value(settings, "test_card")) {
			context.card_mode =
				TestCardMode(std::clamp(int(obs_data_get_int(settings, "test_card")), 0, 1));
		}
		if (obs_data_has_user_value(settings, "phase_ns")) {
			context.phase_ns = uint64_t(std::max<int64_t>(0, obs_data_get_int(settings, "phase_ns")));
			if (context.queue) {
				store(context.queue->header()->phase,
				      std::min(context.phase_ns, context.rate.offset(1) - 1));
			}
		}
	};
	obs_register_output(&info);
}

}
