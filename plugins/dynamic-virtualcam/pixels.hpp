#pragma once

#include <cstddef>
#include <cstdint>

namespace dvc {

enum class PixelFormat { RGB24, RGB32, ARGB32, YUY2, NV12 };

inline bool valid_pixel_format(uint32_t format)
{
	return format <= uint32_t(PixelFormat::NV12);
}

inline size_t image_stride(PixelFormat format, uint32_t width)
{
	if (format == PixelFormat::NV12) {
		return width;
	}
	if (format == PixelFormat::YUY2) {
		return size_t(width) * 2;
	}
	return format == PixelFormat::RGB24 ? (size_t(width) * 3 + 3) & ~size_t(3) : size_t(width) * 4;
}

inline size_t image_size(PixelFormat format, uint32_t width, uint32_t height)
{
	if (format == PixelFormat::RGB24) {
		return size_t((width * 3 + 3) & ~3U) * height;
	}
	if (format == PixelFormat::YUY2) {
		return size_t(width) * height * 2;
	}
	if (format == PixelFormat::NV12) {
		return size_t(width) * height * 3 / 2;
	}
	return size_t(width) * height * 4;
}

void convert(const uint8_t *bgra, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
	     bool bottom_up);
void convert_scalar(const uint8_t *bgra, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
		    bool bottom_up);
void copy_image(const uint8_t *source, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
		bool bottom_up);

}
