#pragma once

#include <obs-module.h>
#include <util/async-frame-buffer.h>
#include <util/threading.h>
#include <util/platform.h>
#include <windows.h>

struct capture_frame_buffer {
	struct obs_async_buffer_config config;
	struct async_frame_buffer clock;
	gs_texture_t *textures[OBS_ASYNC_BUFFER_MAX_FRAMES + 1];
	uint64_t timestamps[OBS_ASYNC_BUFFER_MAX_FRAMES];
	uint32_t slots[OBS_ASYNC_BUFFER_MAX_FRAMES];
	size_t count;
	gs_texture_t *selected;
	uint32_t width;
	uint32_t height;
	enum gs_color_format format;
	uint64_t next_capture;
	uint64_t capture_remainder;
	bool loss_registered;
	uint64_t received;
	uint64_t discarded;
	uint64_t delivered;
	uint64_t repeated;
	uint64_t rebuffers;
	uint64_t next_log;
};

struct capture_worker {
	pthread_mutex_t mutex;
	HANDLE stop;
	HANDLE thread;
	volatile LONG enabled;
	bool event_driven;
	void (*capture)(void *param);
	void *param;
};

#ifdef __cplusplus
extern "C" {
#endif

void capture_frame_buffer_defaults(obs_data_t *settings);
void capture_frame_buffer_properties(obs_properties_t *properties);
void capture_frame_buffer_update(struct capture_frame_buffer *buffer, struct capture_worker *worker,
				 obs_data_t *settings);
void capture_frame_buffer_clear(struct capture_frame_buffer *buffer);
void capture_frame_buffer_free(struct capture_frame_buffer *buffer);
void capture_frame_buffer_push(void *param, gs_texture_t *texture, uint64_t timestamp);
void capture_frame_buffer_tick(struct capture_frame_buffer *buffer, obs_source_t *source);
bool capture_frame_buffer_select_at(struct capture_frame_buffer *buffer, uint64_t timestamp, uint64_t interval);
bool capture_frame_buffer_due(struct capture_frame_buffer *buffer, uint64_t now);
bool capture_worker_init(struct capture_worker *worker, void (*capture)(void *), void *param);
bool capture_worker_set_enabled(struct capture_worker *worker, bool enabled);
void capture_worker_free(struct capture_worker *worker);

#ifdef __cplusplus
}
#endif
