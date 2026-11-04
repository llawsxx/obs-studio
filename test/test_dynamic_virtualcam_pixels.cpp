#include "pixels.hpp"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static constexpr dvc::PixelFormat formats[] = {dvc::PixelFormat::RGB24, dvc::PixelFormat::RGB32,
					       dvc::PixelFormat::ARGB32, dvc::PixelFormat::YUY2,
					       dvc::PixelFormat::NV12};

static void fill_pixels(uint8_t *pixels, size_t bytes, unsigned pattern)
{
	uint32_t random = 0x12345678;
	for (size_t index = 0; index < bytes; ++index) {
		random = random * 1664525 + 1013904223;
		pixels[index] = pattern == 0   ? uint8_t(random >> 24)
				: pattern == 1 ? uint8_t(index & 1 ? 255 : 0)
					       : uint8_t(index);
	}
}

static void compare_conversion(uint32_t width, uint32_t height, unsigned pattern, size_t alignment)
{
	std::vector<uint8_t> input(size_t(width) * height * 4 + alignment + 32, 0xa5);
	auto *source = input.data() + alignment;
	fill_pixels(source, size_t(width) * height * 4, pattern);
	const auto original = input;
	for (const auto format : formats) {
		if ((width & 1 || height & 1) &&
		    (format == dvc::PixelFormat::YUY2 || format == dvc::PixelFormat::NV12)) {
			continue;
		}
		const size_t bytes = dvc::image_size(format, width, height);
		for (const bool bottom_up : {false, true}) {
			std::vector<uint8_t> expected(bytes + alignment + 32, 0xa5), actual(expected);
			dvc::convert_scalar(source, expected.data() + alignment, format, width, height, bottom_up);
			dvc::convert(source, actual.data() + alignment, format, width, height, bottom_up);
			CHECK(actual == expected);
			CHECK(input == original);
			std::vector<uint8_t> top_down(bytes), copied(bytes + alignment + 32, 0xa5), flipped(copied);
			dvc::convert_scalar(source, top_down.data(), format, width, height, false);
			dvc::copy_image(top_down.data(), copied.data() + alignment, format, width, height, bottom_up);
			CHECK(copied == expected);
			if (bottom_up && format != dvc::PixelFormat::NV12 && format != dvc::PixelFormat::YUY2) {
				dvc::copy_image(copied.data() + alignment, flipped.data() + alignment, format, width,
						height, true);
				CHECK(memcmp(flipped.data() + alignment, top_down.data(), bytes) == 0);
			}
		}
	}
}

class GuardedBuffer {
	uint8_t *allocation;
	size_t committed;

public:
	uint8_t *data;
	explicit GuardedBuffer(size_t bytes)
	{
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		committed = (bytes + info.dwPageSize - 1) / info.dwPageSize * info.dwPageSize;
		allocation = static_cast<uint8_t *>(
			VirtualAlloc(nullptr, committed + info.dwPageSize, MEM_RESERVE, PAGE_NOACCESS));
		CHECK(allocation);
		CHECK(VirtualAlloc(allocation, committed, MEM_COMMIT, PAGE_READWRITE));
		data = allocation + committed - bytes;
	}
	~GuardedBuffer() { CHECK(VirtualFree(allocation, 0, MEM_RELEASE)); }
};

static void test_guard_pages()
{
	for (const uint32_t width : {1U, 2U, 3U, 4U, 6U, 8U, 10U, 18U}) {
		GuardedBuffer input(size_t(width) * 2 * 4);
		fill_pixels(input.data, size_t(width) * 2 * 4, 0);
		for (const auto format : formats) {
			if ((width & 1) && (format == dvc::PixelFormat::YUY2 || format == dvc::PixelFormat::NV12)) {
				continue;
			}
			const size_t bytes = dvc::image_size(format, width, 2);
			GuardedBuffer output(bytes);
			std::vector<uint8_t> expected(bytes);
			for (const bool bottom_up : {false, true}) {
				dvc::convert_scalar(input.data, expected.data(), format, width, 2, bottom_up);
				dvc::convert(input.data, output.data, format, width, 2, bottom_up);
				CHECK(memcmp(expected.data(), output.data, bytes) == 0);
			}
		}
	}
}

static double time_conversion(const std::vector<uint8_t> &input, std::vector<uint8_t> &output, dvc::PixelFormat format,
			      uint32_t width, uint32_t height, bool scalar)
{
	const auto function = scalar ? dvc::convert_scalar : dvc::convert;
	function(input.data(), output.data(), format, width, height, true);
	const int iterations = width > 1920 ? 8 : 20;
	const auto start = std::chrono::steady_clock::now();
	for (int iteration = 0; iteration < iterations; ++iteration) {
		function(input.data(), output.data(), format, width, height, true);
	}
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / iterations;
}

static void benchmark()
{
	for (const uint32_t width : {1920U, 3840U}) {
		const uint32_t height = width * 9 / 16;
		std::vector<uint8_t> input(size_t(width) * height * 4);
		fill_pixels(input.data(), input.size(), 0);
		for (const auto format : formats) {
			std::vector<uint8_t> output(dvc::image_size(format, width, height));
			const double scalar = time_conversion(input, output, format, width, height, true);
			const double simd = time_conversion(input, output, format, width, height, false);
			printf("%ux%u format=%u scalar=%.3f ms SIMD=%.3f ms speedup=%.2fx\n", width, height,
			       unsigned(format), scalar, simd, scalar / simd);
		}
	}
}

int main(int count, char **arguments)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	for (const uint32_t width : {1U, 2U, 3U, 4U, 6U, 8U, 10U, 16U, 18U, 30U, 32U, 66U}) {
		for (const uint32_t height : {2U, 4U, 6U}) {
			for (unsigned pattern = 0; pattern < 3; ++pattern) {
				for (const size_t alignment : {0U, 1U, 3U, 15U}) {
					compare_conversion(width, height, pattern, alignment);
				}
			}
		}
	}
	compare_conversion(1920, 1080, 0, 1);
	compare_conversion(3840, 2160, 0, 3);
	test_guard_pages();
	puts("SIMD/scalar byte equality, row order, padding, unaligned buffers and guard pages passed.");
	if (count == 2 && strcmp(arguments[1], "--benchmark") == 0) {
		benchmark();
	}
	return 0;
}
