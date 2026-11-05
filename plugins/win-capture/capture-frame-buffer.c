#include "capture-frame-buffer.h"

#include <inttypes.h>
#include <string.h>
#include <util/platform.h>

#define BUFFER_ENABLED "capture_buffer_enabled"
#define BUFFER_TARGET "capture_buffer_target_frames"
#define BUFFER_MAX "capture_buffer_max_frames"
#define BUFFER_REBUILD "capture_buffer_rebuild_timestamps"
#define BUFFER_NUMERATOR "capture_buffer_fps_numerator"
#define BUFFER_DENOMINATOR "capture_buffer_fps_denominator"

void capture_frame_buffer_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, BUFFER_ENABLED, false);
	obs_data_set_default_int(settings, BUFFER_TARGET, 2);
	obs_data_set_default_int(settings, BUFFER_MAX, 5);
	obs_data_set_default_bool(settings, BUFFER_REBUILD, false);
	obs_data_set_default_int(settings, BUFFER_NUMERATOR, 60);
	obs_data_set_default_int(settings, BUFFER_DENOMINATOR, 1);
}

static bool buffer_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool enabled = obs_data_get_bool(settings, BUFFER_ENABLED);
	const bool rebuild = enabled && obs_data_get_bool(settings, BUFFER_REBUILD);
	int target = (int)obs_data_get_int(settings, BUFFER_TARGET);
	if (target < 1)
		target = 1;
	if (target > OBS_ASYNC_BUFFER_MAX_FRAMES)
		target = OBS_ASYNC_BUFFER_MAX_FRAMES;
	if (obs_data_get_int(settings, BUFFER_MAX) < target)
		obs_data_set_int(settings, BUFFER_MAX, target);
	obs_property_int_set_limits(obs_properties_get(properties, BUFFER_MAX), target, OBS_ASYNC_BUFFER_MAX_FRAMES, 1);
	obs_property_set_visible(obs_properties_get(properties, BUFFER_TARGET), enabled);
	obs_property_set_visible(obs_properties_get(properties, BUFFER_MAX), enabled);
	obs_property_set_visible(obs_properties_get(properties, BUFFER_REBUILD), enabled);
	obs_property_set_visible(obs_properties_get(properties, BUFFER_NUMERATOR), rebuild);
	obs_property_set_visible(obs_properties_get(properties, BUFFER_DENOMINATOR), rebuild);
	return true;
}

void capture_frame_buffer_properties(obs_properties_t *properties)
{
	obs_property_t *property =
		obs_properties_add_bool(properties, BUFFER_ENABLED, obs_module_text("CaptureBuffer.Enabled"));
	obs_property_set_long_description(property, obs_module_text("CaptureBuffer.Description"));
	obs_property_set_modified_callback(property, buffer_modified);
	property = obs_properties_add_int(properties, BUFFER_TARGET, obs_module_text("CaptureBuffer.Target"), 1,
					  OBS_ASYNC_BUFFER_MAX_FRAMES, 1);
	obs_property_set_modified_callback(property, buffer_modified);
	obs_properties_add_int(properties, BUFFER_MAX, obs_module_text("CaptureBuffer.Maximum"), 1,
			       OBS_ASYNC_BUFFER_MAX_FRAMES, 1);
	property = obs_properties_add_bool(properties, BUFFER_REBUILD, obs_module_text("CaptureBuffer.Rebuild"));
	obs_property_set_modified_callback(property, buffer_modified);
	obs_properties_add_int(properties, BUFFER_NUMERATOR, obs_module_text("CaptureBuffer.Numerator"), 1,
			       OBS_ASYNC_BUFFER_MAX_RATE_COMPONENT, 1);
	obs_properties_add_int(properties, BUFFER_DENOMINATOR, obs_module_text("CaptureBuffer.Denominator"), 1,
			       OBS_ASYNC_BUFFER_MAX_RATE_COMPONENT, 1);
}

void capture_frame_buffer_clear(struct capture_frame_buffer *buffer)
{
	for (size_t index = 0; index < OBS_ASYNC_BUFFER_MAX_FRAMES + 1; index++) {
		gs_texture_destroy(buffer->textures[index]);
		buffer->textures[index] = NULL;
	}
	buffer->count = 0;
	buffer->selected = NULL;
	buffer->width = 0;
	buffer->height = 0;
	buffer->next_capture = 0;
	buffer->capture_remainder = 0;
	async_frame_buffer_reset(&buffer->clock);
}

void capture_frame_buffer_free(struct capture_frame_buffer *buffer)
{
	if (buffer->loss_registered) {
		gs_unregister_loss_callbacks(buffer);
		buffer->loss_registered = false;
	}
	capture_frame_buffer_clear(buffer);
}

static void buffer_device_loss(void *param)
{
	capture_frame_buffer_clear(param);
}

static void buffer_device_rebuild(void *device, void *param)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(param);
}

void capture_frame_buffer_update(struct capture_frame_buffer *buffer, struct capture_worker *worker,
				 obs_data_t *settings)
{
	struct obs_async_buffer_config config = {0};
	config.enabled = obs_data_get_bool(settings, BUFFER_ENABLED);
	config.target_frames = (uint32_t)obs_data_get_int(settings, BUFFER_TARGET);
	config.max_frames = (uint32_t)obs_data_get_int(settings, BUFFER_MAX);
	config.rebuild_timestamps = obs_data_get_bool(settings, BUFFER_REBUILD);
	config.fps_numerator = (uint32_t)obs_data_get_int(settings, BUFFER_NUMERATOR);
	config.fps_denominator = (uint32_t)obs_data_get_int(settings, BUFFER_DENOMINATOR);
	config = async_frame_buffer_normalize(config);
	if (buffer->config.enabled != config.enabled || buffer->config.target_frames != config.target_frames ||
	    buffer->config.max_frames != config.max_frames ||
	    buffer->config.rebuild_timestamps != config.rebuild_timestamps ||
	    buffer->config.fps_numerator != config.fps_numerator ||
	    buffer->config.fps_denominator != config.fps_denominator) {
		capture_frame_buffer_clear(buffer);
		buffer->config = config;
	}
	if (config.enabled && !buffer->loss_registered && gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
		const struct gs_device_loss callbacks = {buffer_device_loss, buffer_device_rebuild, buffer};
		gs_register_loss_callbacks(&callbacks);
		buffer->loss_registered = true;
	}
	if (!capture_worker_set_enabled(worker, config.enabled && !worker->event_driven)) {
		blog(LOG_ERROR, "[capture-buffer] Failed to start capture worker; using unbuffered capture.");
		buffer->config.enabled = false;
	}
}

static void discard_frames(struct capture_frame_buffer *buffer, size_t count)
{
	buffer->count -= count;
	memmove(buffer->slots, buffer->slots + count, buffer->count * sizeof(*buffer->slots));
	memmove(buffer->timestamps, buffer->timestamps + count, buffer->count * sizeof(*buffer->timestamps));
}

void capture_frame_buffer_push(void *param, gs_texture_t *texture, uint64_t timestamp)
{
	struct capture_frame_buffer *buffer = param;
	if (!buffer->config.enabled)
		return;
	if (!texture) {
		capture_frame_buffer_clear(buffer);
		return;
	}
	const uint32_t width = gs_texture_get_width(texture);
	const uint32_t height = gs_texture_get_height(texture);
	const enum gs_color_format format = gs_texture_get_color_format(texture);
	if (buffer->width != width || buffer->height != height || buffer->format != format) {
		capture_frame_buffer_clear(buffer);
		buffer->width = width;
		buffer->height = height;
		buffer->format = format;
	}
	if (async_frame_buffer_arrived(&buffer->clock, timestamp, buffer->config.rebuild_timestamps)) {
		buffer->discarded += buffer->count;
		buffer->count = 0;
		buffer->clock.playing = false;
		buffer->rebuffers++;
	}
	const size_t trim = async_frame_buffer_trim(&buffer->config, buffer->count + 1);
	if (trim) {
		discard_frames(buffer, trim);
		buffer->discarded += trim;
		buffer->clock.playing = false;
		buffer->rebuffers++;
	}
	uint32_t slot = 0;
	for (; slot < OBS_ASYNC_BUFFER_MAX_FRAMES + 1; slot++) {
		if (buffer->textures[slot] && buffer->textures[slot] == buffer->selected)
			continue;
		bool queued = false;
		for (size_t index = 0; index < buffer->count; index++) {
			if (buffer->slots[index] == slot) {
				queued = true;
				break;
			}
		}
		if (!queued)
			break;
	}
	if (slot == OBS_ASYNC_BUFFER_MAX_FRAMES + 1)
		return;
	if (!buffer->textures[slot])
		buffer->textures[slot] = gs_texture_create(width, height, format, 1, NULL, 0);
	if (!buffer->textures[slot])
		return;
	gs_copy_texture(buffer->textures[slot], texture);
	gs_flush();
	buffer->slots[buffer->count] = slot;
	buffer->timestamps[buffer->count++] = timestamp;
	buffer->received++;
}

bool capture_frame_buffer_select_at(struct capture_frame_buffer *buffer, uint64_t timestamp, uint64_t interval)
{
	if (!buffer->config.enabled)
		return false;
	const bool playing = buffer->clock.playing;
	const struct async_frame_buffer_selection selection = async_frame_buffer_select(
		&buffer->clock, &buffer->config, buffer->timestamps, buffer->count, timestamp, interval);
	if (selection.output) {
		buffer->selected = buffer->textures[buffer->slots[selection.discard]];
		buffer->delivered++;
	} else {
		buffer->repeated++;
	}
	buffer->discarded += selection.discard;
	discard_frames(buffer, selection.discard + (selection.output ? 1 : 0));
	if (playing && !buffer->clock.playing)
		buffer->rebuffers++;
	if (!buffer->selected && buffer->count && timestamp >= buffer->timestamps[buffer->count - 1] &&
	    timestamp - buffer->timestamps[buffer->count - 1] > 250000000ULL) {
		buffer->selected = buffer->textures[buffer->slots[buffer->count - 1]];
		buffer->discarded += buffer->count - 1;
		buffer->count = 0;
		buffer->delivered++;
		return true;
	}
	return selection.output;
}

void capture_frame_buffer_tick(struct capture_frame_buffer *buffer, obs_source_t *source)
{
	if (!buffer->config.enabled)
		return;
	capture_frame_buffer_select_at(buffer, obs_get_video_frame_time(), obs_get_frame_interval_ns());
	const uint64_t now = os_gettime_ns();
	if (now >= buffer->next_log) {
		blog(LOG_INFO,
		     "[capture-buffer: '%s'] received=%" PRIu64 ", delivered=%" PRIu64 ", repeated=%" PRIu64
		     ", discarded=%" PRIu64 ", rebuffers=%" PRIu64 ", queued=%zu, target=%u, max=%u",
		     obs_source_get_name(source), buffer->received, buffer->delivered, buffer->repeated,
		     buffer->discarded, buffer->rebuffers, buffer->count, buffer->config.target_frames,
		     buffer->config.max_frames);
		buffer->next_log = now + 5000000000ULL;
	}
}

bool capture_frame_buffer_due(struct capture_frame_buffer *buffer, uint64_t now)
{
	const uint64_t interval = buffer->config.rebuild_timestamps ? async_frame_buffer_interval(&buffer->config)
								    : obs_get_frame_interval_ns();
	if (buffer->next_capture && now < buffer->next_capture)
		return false;
	if (!buffer->next_capture)
		buffer->next_capture = now;
	buffer->next_capture += interval;
	if (buffer->config.rebuild_timestamps) {
		buffer->capture_remainder +=
			1000000000ULL * buffer->config.fps_denominator % buffer->config.fps_numerator;
		buffer->next_capture += buffer->capture_remainder / buffer->config.fps_numerator;
		buffer->capture_remainder %= buffer->config.fps_numerator;
	}
	if (buffer->next_capture <= now) {
		buffer->next_capture = now + interval;
		buffer->capture_remainder = 0;
	}
	return true;
}

static DWORD WINAPI capture_thread(void *param)
{
	struct capture_worker *worker = param;
	os_set_thread_name("obs-capture-buffer");
	while (WaitForSingleObject(worker->stop, InterlockedCompareExchange(&worker->enabled, 0, 0) ? 1 : 50) ==
	       WAIT_TIMEOUT) {
		pthread_mutex_lock(&worker->mutex);
		if (InterlockedCompareExchange(&worker->enabled, 0, 0)) {
			obs_enter_graphics();
			worker->capture(worker->param);
			obs_leave_graphics();
		}
		pthread_mutex_unlock(&worker->mutex);
	}
	return 0;
}

bool capture_worker_init(struct capture_worker *worker, void (*capture)(void *), void *param)
{
	pthread_mutex_init(&worker->mutex, NULL);
	worker->capture = capture;
	worker->param = param;
	worker->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	return worker->stop != NULL;
}

bool capture_worker_set_enabled(struct capture_worker *worker, bool enabled)
{
	if (enabled && !worker->thread) {
		worker->thread = CreateThread(NULL, 0, capture_thread, worker, 0, NULL);
		if (!worker->thread)
			return false;
	}
	InterlockedExchange(&worker->enabled, enabled);
	return true;
}

void capture_worker_free(struct capture_worker *worker)
{
	if (worker->stop)
		SetEvent(worker->stop);
	if (worker->thread) {
		WaitForSingleObject(worker->thread, INFINITE);
		CloseHandle(worker->thread);
	}
	if (worker->stop)
		CloseHandle(worker->stop);
	pthread_mutex_destroy(&worker->mutex);
}
