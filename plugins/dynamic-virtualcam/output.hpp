#pragma once

#include "ipc.hpp"
#include "test-card.hpp"
#include <obs-module.h>
#include <atomic>
#include <memory>

namespace dvc {

struct Output {
	obs_output_t *output;
	std::unique_ptr<Queue> queue;
	std::wstring session;
	Rate rate;
	bool follow_obs_fps = true;
	PixelFormat pixel_format = PixelFormat::NV12;
	uint32_t slots;
	uint32_t target_frames = 2;
	uint64_t phase_ns = 0;
	std::atomic<TestCardMode> card_mode{TestCardMode::Off};
	std::atomic<bool> card_failed{false};
	std::unique_ptr<TestCard> card;
	std::vector<uint8_t> card_pixels;
	uint64_t card_sequence = 0;
	Rate producer_rate;

	static void register_type();
};

}
