#pragma once

#include "timing.hpp"
#include <cstddef>

namespace dvc {

struct FrameStamp {
	uint64_t sequence = 0;
	uint64_t timestamp = 0;
	uint64_t generation = 0;
};

class FrameBuffer {
	uint64_t generation = 0;
	uint64_t source_anchor = 0;
	uint64_t clock_anchor = 0;
	bool primed_once = false;

	void discard_to(uint64_t sequence)
	{
		if (sequence > consumed) {
			discarded += sequence - consumed;
			consumed = sequence;
		}
	}

public:
	uint64_t consumed = 0;
	uint64_t discarded = 0;
	uint64_t rebuffers = 0;
	uint64_t buffered = 0;
	bool playing = false;

	FrameStamp select(const FrameStamp *frames, size_t count, uint64_t latest, uint64_t current_generation,
			  uint64_t generation_start, uint64_t clock, Rate rate, uint32_t target, uint32_t maximum)
	{
		if (!current_generation || !generation_start || latest < generation_start) {
			return {};
		}
		if (generation != current_generation) {
			if (generation) {
				discard_to(generation_start - 1);
			} else {
				consumed = generation_start - 1;
			}
			generation = current_generation;
			playing = false;
		}
		if (latest > consumed && latest - consumed > maximum) {
			discard_to(latest - target);
			playing = false;
		}
		buffered = latest > consumed ? latest - consumed : 0;
		if (!buffered) {
			playing = false;
			return {};
		}
		size_t first = 0;
		while (first < count && frames[first].sequence <= consumed) {
			++first;
		}
		if (!playing) {
			if (count - first < target || frames[first].sequence != consumed + 1) {
				return {};
			}
			source_anchor = frames[first].timestamp;
			clock_anchor = clock;
			rebuffers += primed_once;
			primed_once = true;
			playing = true;
		}
		const uint64_t elapsed = clock - clock_anchor;
		const uint64_t timestamp = elapsed > UINT64_MAX - source_anchor ? UINT64_MAX : source_anchor + elapsed;
		for (size_t index = first; index < count; ++index) {
			const auto &frame = frames[index];
			if (frame.timestamp > timestamp && frame.timestamp - timestamp > rate.window()) {
				return {};
			}
			if (eligible(frame.sequence, consumed, frame.timestamp, timestamp, rate)) {
				return frame;
			}
			discard_to(frame.sequence);
			buffered = latest - consumed;
		}
		if (!buffered) {
			playing = false;
		}
		return {};
	}

	void consume(uint64_t sequence, uint64_t latest)
	{
		if (sequence > consumed + 1) {
			discarded += sequence - consumed - 1;
		}
		consumed = sequence;
		buffered = latest > consumed ? latest - consumed : 0;
		if (!buffered) {
			playing = false;
		}
	}
};

}
