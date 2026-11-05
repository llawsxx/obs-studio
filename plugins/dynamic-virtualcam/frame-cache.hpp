#pragma once

#include "frame-buffer.hpp"
#include <array>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace dvc {

class FrameCache {
	struct Frame {
		FrameStamp stamp;
		std::vector<uint8_t> pixels;
	};
	mutable std::mutex mutex;
	std::deque<Frame> frames;
	std::vector<uint8_t> spare;
	FrameBuffer buffer;
	Rate rate;
	uint32_t target;
	uint32_t maximum;
	uint64_t latest = 0;
	uint64_t generation = 0;
	uint64_t generation_start = 0;

	void discard_consumed()
	{
		while (!frames.empty() && (frames.front().stamp.generation != generation ||
					   frames.front().stamp.sequence <= buffer.consumed)) {
			if (spare.empty()) {
				spare.swap(frames.front().pixels);
			}
			frames.pop_front();
		}
	}

public:
	struct Statistics {
		uint64_t buffered;
		uint64_t rebuffers;
		uint64_t discarded;
	};

	FrameCache(Rate rate, uint32_t target, uint32_t maximum) : rate(rate), target(target), maximum(maximum)
	{
		if (!rate.valid() || !target || target > maximum || maximum > max_buffer_frames) {
			throw std::invalid_argument("Invalid virtual camera frame cache configuration");
		}
	}

	void push(FrameStamp stamp, uint64_t start, std::vector<uint8_t> &image)
	{
		std::lock_guard lock(mutex);
		if (stamp.sequence <= latest || !stamp.generation || !start || stamp.sequence < start) {
			return;
		}
		latest = stamp.sequence;
		generation = stamp.generation;
		generation_start = start;
		buffer.observe(latest, generation, generation_start, target, maximum);
		discard_consumed();
		frames.push_back({stamp, {}});
		frames.back().pixels.swap(image);
		image.swap(spare);
	}

	bool select(uint64_t timestamp, std::vector<uint8_t> &image)
	{
		std::lock_guard lock(mutex);
		if (frames.empty()) {
			return false;
		}
		if (!buffer.playing && frames.front().stamp.sequence > buffer.consumed + 1) {
			buffer.rebuffer(frames.front().stamp.sequence - 1, latest);
		}
		std::array<FrameStamp, max_buffer_frames> stamps;
		for (size_t index = 0; index < frames.size(); ++index) {
			stamps[index] = frames[index].stamp;
		}
		const FrameStamp selected = buffer.select(stamps.data(), frames.size(), latest, generation,
							  generation_start, timestamp, rate, target, maximum);
		if (!selected.sequence) {
			discard_consumed();
			return false;
		}
		for (auto &frame : frames) {
			if (frame.stamp.sequence == selected.sequence) {
				image.swap(frame.pixels);
				buffer.consume(selected.sequence, latest);
				break;
			}
		}
		discard_consumed();
		return true;
	}

	Statistics statistics() const
	{
		std::lock_guard lock(mutex);
		return {uint64_t(frames.size()), buffer.rebuffers, buffer.discarded};
	}
};

}
