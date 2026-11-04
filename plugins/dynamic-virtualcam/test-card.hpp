#pragma once

#include "ipc.hpp"
#include <memory>

namespace dvc {

enum class TestCardMode { Off, Motion };

struct CardFrame {
	uint64_t sequence;
	uint64_t generated_ns;
	uint64_t source_timestamp;
	Rate producer_rate;
	Rate output_rate;
};

class TestCard {
	struct Graphics;
	std::unique_ptr<Graphics> graphics;
	uint32_t width, height;
	void rectangle(int left, int top, int right, int bottom, COLORREF color);
	void text(int left, int top, const std::wstring &value);
	void motion(const CardFrame &frame, int top, int bottom);

public:
	TestCard(uint32_t width, uint32_t height);
	~TestCard();
	bool valid() const;
	const uint8_t *draw(TestCardMode mode, const CardFrame &frame);
	size_t stride() const { return size_t(width) * 4; }
};

}
