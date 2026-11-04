#include "pixels.hpp"

#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <tmmintrin.h>

namespace dvc {

static inline uint8_t clamp_byte(int value)
{
	return uint8_t(std::max(0, std::min(255, value)));
}

static inline uint8_t luma(const uint8_t *pixel)
{
	return clamp_byte(((47 * pixel[2] + 157 * pixel[1] + 16 * pixel[0] + 128) >> 8) + 16);
}

static inline uint8_t chroma_u(int red, int green, int blue)
{
	return clamp_byte(((-26 * red - 87 * green + 113 * blue + 128) >> 8) + 128);
}

static inline uint8_t chroma_v(int red, int green, int blue)
{
	return clamp_byte(((112 * red - 102 * green - 10 * blue + 128) >> 8) + 128);
}

void convert_scalar(const uint8_t *bgra, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
		    bool bottom_up)
{
	if (format == PixelFormat::RGB24 || format == PixelFormat::RGB32 || format == PixelFormat::ARGB32) {
		const uint32_t bytes = format == PixelFormat::RGB24 ? 3 : 4;
		const size_t stride = (width * bytes + 3) & ~size_t(3);
		for (uint32_t row = 0; row < height; ++row) {
			auto *output = destination + (bottom_up ? height - row - 1 : row) * stride;
			const auto *input = bgra + size_t(row) * width * 4;
			for (uint32_t column = 0; column < width; ++column) {
				memcpy(output + column * bytes, input + column * 4, 3);
				if (bytes == 4) {
					output[column * bytes + 3] = format == PixelFormat::ARGB32 ? 255 : 0;
				}
			}
			memset(output + width * bytes, 0, stride - width * bytes);
		}
		return;
	}
	if (format == PixelFormat::YUY2) {
		for (size_t pixel = 0; pixel < size_t(width) * height; pixel += 2) {
			const auto *first = bgra + pixel * 4;
			const auto *second = first + 4;
			destination[pixel * 2] = luma(first);
			destination[pixel * 2 + 1] = chroma_u((first[2] + second[2] + 1) / 2,
							      (first[1] + second[1] + 1) / 2,
							      (first[0] + second[0] + 1) / 2);
			destination[pixel * 2 + 2] = luma(second);
			destination[pixel * 2 + 3] = chroma_v((first[2] + second[2] + 1) / 2,
							      (first[1] + second[1] + 1) / 2,
							      (first[0] + second[0] + 1) / 2);
		}
		return;
	}
	const size_t pixels = size_t(width) * height;
	for (size_t pixel = 0; pixel < pixels; ++pixel) {
		destination[pixel] = luma(bgra + pixel * 4);
	}
	for (uint32_t row = 0; row < height; row += 2) {
		for (uint32_t column = 0; column < width; column += 2) {
			int red = 0, green = 0, blue = 0;
			for (uint32_t vertical = 0; vertical < 2; ++vertical) {
				for (uint32_t horizontal = 0; horizontal < 2; ++horizontal) {
					const auto *pixel =
						bgra + (size_t(row + vertical) * width + column + horizontal) * 4;
					red += pixel[2];
					green += pixel[1];
					blue += pixel[0];
				}
			}
			auto *output = destination + pixels + size_t(row / 2) * width + column;
			output[0] = chroma_u((red + 2) / 4, (green + 2) / 4, (blue + 2) / 4);
			output[1] = chroma_v((red + 2) / 4, (green + 2) / 4, (blue + 2) / 4);
		}
	}
}

static bool has_ssse3()
{
	static const bool supported = [] {
		int registers[4];
		__cpuid(registers, 1);
		return (registers[2] & (1 << 9)) != 0;
	}();
	return supported;
}

static __m128i transform4(__m128i pixels, __m128i weights, int offset)
{
	const __m128i zero = _mm_setzero_si128();
	__m128i low = _mm_madd_epi16(_mm_unpacklo_epi8(pixels, zero), weights);
	__m128i high = _mm_madd_epi16(_mm_unpackhi_epi8(pixels, zero), weights);
	low = _mm_add_epi32(low, _mm_srli_si128(low, 4));
	high = _mm_add_epi32(high, _mm_srli_si128(high, 4));
	low = _mm_shuffle_epi32(low, _MM_SHUFFLE(2, 0, 2, 0));
	high = _mm_shuffle_epi32(high, _MM_SHUFFLE(2, 0, 2, 0));
	const __m128i values = _mm_srai_epi32(_mm_add_epi32(_mm_unpacklo_epi64(low, high), _mm_set1_epi32(offset)), 8);
	return _mm_packus_epi16(_mm_packs_epi32(values, zero), zero);
}

static __m128i luma4(__m128i pixels)
{
	return transform4(pixels, _mm_setr_epi16(16, 157, 47, 0, 16, 157, 47, 0), 16 * 256 + 128);
}

static __m128i chroma4(__m128i pixels)
{
	const __m128i chroma_u =
		transform4(pixels, _mm_setr_epi16(113, -87, -26, 0, 113, -87, -26, 0), 128 * 256 + 128);
	const __m128i chroma_v =
		transform4(pixels, _mm_setr_epi16(-10, -102, 112, 0, -10, -102, 112, 0), 128 * 256 + 128);
	return _mm_unpacklo_epi8(chroma_u, chroma_v);
}

__declspec(noinline) static void rgb24_row(const uint8_t *input, uint8_t *output, uint32_t width)
{
	const __m128i shuffle = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
	uint32_t column = 0;
	for (; column + 4 <= width; column += 4) {
		const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i *>(input + size_t(column) * 4));
		const __m128i packed = _mm_shuffle_epi8(pixels, shuffle);
		_mm_storel_epi64(reinterpret_cast<__m128i *>(output + size_t(column) * 3), packed);
		const uint32_t tail = uint32_t(_mm_cvtsi128_si32(_mm_srli_si128(packed, 8)));
		memcpy(output + size_t(column) * 3 + 8, &tail, sizeof(tail));
	}
	for (; column < width; ++column) {
		memcpy(output + size_t(column) * 3, input + size_t(column) * 4, 3);
	}
}

void convert(const uint8_t *bgra, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
	     bool bottom_up)
{
	if (format == PixelFormat::RGB24 && !has_ssse3()) {
		convert_scalar(bgra, destination, format, width, height, bottom_up);
		return;
	}
	if (format == PixelFormat::RGB24 || format == PixelFormat::RGB32 || format == PixelFormat::ARGB32) {
		const size_t stride = image_stride(format, width);
		const __m128i color_mask = _mm_set1_epi32(0x00ffffff);
		const __m128i alpha = format == PixelFormat::ARGB32 ? _mm_set1_epi32(int(0xff000000))
								    : _mm_setzero_si128();
		for (uint32_t row = 0; row < height; ++row) {
			auto *output = destination + size_t(bottom_up ? height - row - 1 : row) * stride;
			const auto *input = bgra + size_t(row) * width * 4;
			if (format == PixelFormat::RGB24) {
				rgb24_row(input, output, width);
				memset(output + size_t(width) * 3, 0, stride - size_t(width) * 3);
				continue;
			}
			uint32_t column = 0;
			for (; column + 4 <= width; column += 4) {
				const __m128i pixels =
					_mm_loadu_si128(reinterpret_cast<const __m128i *>(input + size_t(column) * 4));
				_mm_storeu_si128(reinterpret_cast<__m128i *>(output + size_t(column) * 4),
						 _mm_or_si128(_mm_and_si128(pixels, color_mask), alpha));
			}
			for (; column < width; ++column) {
				memcpy(output + size_t(column) * 4, input + size_t(column) * 4, 3);
				output[size_t(column) * 4 + 3] = format == PixelFormat::ARGB32 ? 255 : 0;
			}
		}
		return;
	}
	const size_t pixels = size_t(width) * height;
	if (format == PixelFormat::YUY2) {
		size_t pixel = 0;
		for (; pixel + 4 <= pixels; pixel += 4) {
			const __m128i colors = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bgra + pixel * 4));
			const __m128i average =
				_mm_avg_epu8(colors, _mm_shuffle_epi32(colors, _MM_SHUFFLE(2, 3, 0, 1)));
			const __m128i chroma = _mm_shufflelo_epi16(chroma4(average), _MM_SHUFFLE(2, 0, 2, 0));
			_mm_storel_epi64(reinterpret_cast<__m128i *>(destination + pixel * 2),
					 _mm_unpacklo_epi8(luma4(colors), chroma));
		}
		if (pixel < pixels) {
			convert_scalar(bgra + pixel * 4, destination + pixel * 2, format, uint32_t(pixels - pixel), 1,
				       false);
		}
		return;
	}
	size_t pixel = 0;
	for (; pixel + 4 <= pixels; pixel += 4) {
		const uint32_t packed = uint32_t(
			_mm_cvtsi128_si32(luma4(_mm_loadu_si128(reinterpret_cast<const __m128i *>(bgra + pixel * 4)))));
		memcpy(destination + pixel, &packed, sizeof(packed));
	}
	for (; pixel < pixels; ++pixel) {
		destination[pixel] = luma(bgra + pixel * 4);
	}
	const __m128i zero = _mm_setzero_si128();
	const __m128i round = _mm_set1_epi16(2);
	for (uint32_t row = 0; row < height; row += 2) {
		uint32_t column = 0;
		for (; column + 4 <= width; column += 4) {
			const auto *input = bgra + (size_t(row) * width + column) * 4;
			const __m128i top = _mm_loadu_si128(reinterpret_cast<const __m128i *>(input));
			const __m128i bottom =
				_mm_loadu_si128(reinterpret_cast<const __m128i *>(input + size_t(width) * 4));
			__m128i low = _mm_add_epi16(_mm_unpacklo_epi8(top, zero), _mm_unpacklo_epi8(bottom, zero));
			__m128i high = _mm_add_epi16(_mm_unpackhi_epi8(top, zero), _mm_unpackhi_epi8(bottom, zero));
			low = _mm_add_epi16(low, _mm_srli_si128(low, 8));
			high = _mm_add_epi16(high, _mm_srli_si128(high, 8));
			const __m128i average = _mm_srli_epi16(_mm_add_epi16(_mm_unpacklo_epi64(low, high), round), 2);
			const uint32_t packed = uint32_t(_mm_cvtsi128_si32(chroma4(_mm_packus_epi16(average, zero))));
			memcpy(destination + pixels + size_t(row / 2) * width + column, &packed, sizeof(packed));
		}
		for (; column < width; column += 2) {
			int red = 0, green = 0, blue = 0;
			for (uint32_t vertical = 0; vertical < 2; ++vertical) {
				for (uint32_t horizontal = 0; horizontal < 2; ++horizontal) {
					const auto *color =
						bgra + (size_t(row + vertical) * width + column + horizontal) * 4;
					red += color[2];
					green += color[1];
					blue += color[0];
				}
			}
			auto *output = destination + pixels + size_t(row / 2) * width + column;
			output[0] = chroma_u((red + 2) / 4, (green + 2) / 4, (blue + 2) / 4);
			output[1] = chroma_v((red + 2) / 4, (green + 2) / 4, (blue + 2) / 4);
		}
	}
}

void copy_image(const uint8_t *source, uint8_t *destination, PixelFormat format, uint32_t width, uint32_t height,
		bool bottom_up)
{
	if (!bottom_up || format == PixelFormat::NV12 || format == PixelFormat::YUY2) {
		memcpy(destination, source, image_size(format, width, height));
		return;
	}
	const size_t stride = image_stride(format, width);
	for (uint32_t row = 0; row < height; ++row) {
		memcpy(destination + size_t(row) * stride, source + size_t(height - row - 1) * stride, stride);
	}
}

}
