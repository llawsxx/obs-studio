#include "test-card.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dvc {

struct TestCard::Graphics {
	HDC dc = nullptr;
	HBITMAP bitmap = nullptr;
	HGDIOBJ old_bitmap = nullptr;
	HFONT font = nullptr;
	HGDIOBJ old_font = nullptr;
	HPEN pen = nullptr;
	HGDIOBJ old_pen = nullptr;
	uint8_t *pixels = nullptr;

	~Graphics()
	{
		if (old_pen) {
			SelectObject(dc, old_pen);
		}
		if (old_font) {
			SelectObject(dc, old_font);
		}
		if (old_bitmap) {
			SelectObject(dc, old_bitmap);
		}
		if (pen) {
			DeleteObject(pen);
		}
		if (font) {
			DeleteObject(font);
		}
		if (bitmap) {
			DeleteObject(bitmap);
		}
		if (dc) {
			DeleteDC(dc);
		}
	}
};

TestCard::TestCard(uint32_t width, uint32_t height)
	: graphics(std::make_unique<Graphics>()),
	  width(width),
	  height(height)
{
	if (!width || !height || width > 8192 || height > 8192) {
		return;
	}
	graphics->dc = CreateCompatibleDC(nullptr);
	if (!graphics->dc) {
		return;
	}
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = LONG(width);
	info.bmiHeader.biHeight = -LONG(height);
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	graphics->bitmap = CreateDIBSection(graphics->dc, &info, DIB_RGB_COLORS,
					    reinterpret_cast<void **>(&graphics->pixels), nullptr, 0);
	if (!graphics->bitmap) {
		return;
	}
	graphics->old_bitmap = SelectObject(graphics->dc, graphics->bitmap);
	const int font_height = std::clamp(int(height / 30), 10, 48);
	graphics->font = CreateFontW(-font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH,
				     L"Consolas");
	if (graphics->font) {
		graphics->old_font = SelectObject(graphics->dc, graphics->font);
	}
	graphics->pen = CreatePen(PS_SOLID, std::max(1, int(height / 300)), RGB(255, 255, 255));
	if (graphics->pen) {
		graphics->old_pen = SelectObject(graphics->dc, graphics->pen);
	}
	SetBkMode(graphics->dc, TRANSPARENT);
	SetTextColor(graphics->dc, RGB(255, 255, 255));
}

TestCard::~TestCard() = default;

bool TestCard::valid() const
{
	return graphics->dc && graphics->bitmap && graphics->pixels;
}

void TestCard::rectangle(int left, int top, int right, int bottom, COLORREF color)
{
	RECT area = {left, top, right, bottom};
	SetDCBrushColor(graphics->dc, color);
	FillRect(graphics->dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void TestCard::text(int left, int top, const std::wstring &value)
{
	TextOutW(graphics->dc, left, top, value.c_str(), int(value.size()));
}

void TestCard::motion(const CardFrame &frame, int top, int bottom)
{
	static constexpr uint64_t periods[] = {24, 16, 12, 8, 6, 5, 4, 3};
	const int cell_width = int(width / 4);
	const int cell_height = (bottom - top) / 2;
	const int radius = std::max(1, std::min(cell_width, cell_height) * 3 / 8);
	for (int dial = 0; dial < 8; ++dial) {
		const int center_x = cell_width * (dial % 4) + cell_width / 2;
		const int center_y = top + cell_height * (dial / 4) + cell_height / 2;
		for (uint64_t tick = 0; tick < periods[dial]; ++tick) {
			const double angle = double(tick) * 6.283185307179586 / double(periods[dial]);
			const int end_x = center_x + int(std::cos(angle) * radius);
			const int end_y = center_y + int(std::sin(angle) * radius);
			rectangle(end_x - 1, end_y - 1, end_x + 2, end_y + 2, RGB(180, 180, 180));
		}
		const double angle = double(frame.sequence % periods[dial]) * 6.283185307179586 / double(periods[dial]);
		MoveToEx(graphics->dc, center_x, center_y, nullptr);
		LineTo(graphics->dc, center_x + int(std::cos(angle) * radius),
		       center_y + int(std::sin(angle) * radius));
		text(center_x - radius, center_y + radius, std::to_wstring(periods[dial]) + L" frames/cycle");
	}
}

const uint8_t *TestCard::draw(TestCardMode mode, const CardFrame &frame)
{
	if (!valid() || mode == TestCardMode::Off) {
		return nullptr;
	}
	memset(graphics->pixels, 0, stride() * height);
	static constexpr COLORREF bars[] = {RGB(235, 235, 235), RGB(235, 235, 0), RGB(0, 235, 235), RGB(0, 235, 0),
					    RGB(235, 0, 235),   RGB(235, 0, 0),   RGB(0, 0, 235),   RGB(16, 16, 16)};
	const int bar_height = int(height / 8);
	for (int bar = 0; bar < 8; ++bar) {
		rectangle(int(uint64_t(width) * bar / 8), 0, int(uint64_t(width) * (bar + 1) / 8), bar_height,
			  bars[bar]);
	}
	const int line_height = std::clamp(int(height / 30), 10, 48) + 3;
	text(8, bar_height + 2, L"OBS DVC  FRAME " + std::to_wstring(frame.sequence));
	text(8, bar_height + line_height,
	     L"OBS " + std::to_wstring(frame.producer_rate.numerator) + L"/" +
		     std::to_wstring(frame.producer_rate.denominator) + L"  CAMERA " +
		     std::to_wstring(frame.output_rate.numerator) + L"/" +
		     std::to_wstring(frame.output_rate.denominator) + L" FPS");
	text(8, bar_height + line_height * 2, L"FRAME CLOCK ns " + std::to_wstring(frame.generated_ns));
	text(8, bar_height + line_height * 3, L"OBS PTS ns " + std::to_wstring(frame.source_timestamp));
	const int body_top = bar_height + line_height * 4 + 4;
	const int stripe_height = std::max(2, int(height / 30));
	const int body_bottom = int(height) - stripe_height * 3;
	motion(frame, body_top, body_bottom);
	const uint64_t gray = frame.sequence ^ (frame.sequence >> 1);
	for (int bit = 0; bit < 32; ++bit) {
		const bool white = (gray >> (31 - bit)) & 1;
		rectangle(int(uint64_t(width) * bit / 32), int(height) - stripe_height * 2,
			  int(uint64_t(width) * (bit + 1) / 32), int(height) - stripe_height,
			  white ? RGB(255, 255, 255) : RGB(16, 16, 16));
	}
	const int cursor = int((frame.sequence % 120) * width / 120);
	rectangle(cursor, int(height) - stripe_height, std::min(int(width), cursor + std::max(2, int(width / 120))),
		  int(height), RGB(255, 255, 0));
	GdiFlush();
	return graphics->pixels;
}

}
