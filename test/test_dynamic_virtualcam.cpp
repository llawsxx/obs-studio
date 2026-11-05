#include "ipc.hpp"
#include "pixels.hpp"
#include "test-card.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

void test_camera();
void test_camera_cadence();
void test_camera_phase_updates();
void test_camera_parallel_receivers();
void test_frame_buffer();
int frame_buffer_reader_helper(const char *session);

static void test_timing()
{
	const dvc::Rate fractional{60000, 1001};
	CHECK(fractional.valid());
	CHECK(fractional.offset(60000) == 1001000000000ULL);
	CHECK(fractional.offset(1) == 16683333);
	CHECK(fractional.offset(3) == 50050000);
	CHECK(dvc::mul_div(999999999999999ULL, 999983, 999979000000000ULL) == 1000004);
	CHECK((dvc::Rate{999983, 999979}.offset(987654321)) == 987650370315554364ULL);
	CHECK((dvc::Rate{999983, 999979}.after(1234567890123456)) == 1234573);
	CHECK((dvc::Rate{60, 1}.window()) == 10000000);
	const dvc::Rate fixed60{60, 1, true}, fixed_fractional{60000, 1001, true};
	CHECK(fixed60.offset(1) == 16666666);
	CHECK(fixed60.offset(3) == 49999998);
	CHECK(fixed60.offset(60) == 999999960);
	CHECK(fixed60.offset(3600000) == 59999997600000ULL);
	CHECK(fixed_fractional.offset(3) == 50049999);
	CHECK(fixed_fractional.offset(60000) == 1000999980000ULL);
	CHECK(fixed60.window() == 9999999);
	CHECK(!(dvc::Rate{0, 1}.valid()));
	CHECK(!(dvc::Rate{1001, 1}.valid()));
	CHECK(!(dvc::Rate{1, 2}.valid()));
	for (const dvc::Rate rate : {dvc::Rate{60, 1}, fractional, fixed60, fixed_fractional, dvc::Rate{999983, 999979},
				     dvc::Rate{1000000, 1000}}) {
		CHECK(rate.valid());
		for (uint64_t index = 0; index < 100000; ++index) {
			const uint64_t deadline = rate.offset(index);
			CHECK(rate.after(deadline) == index + 1);
			if (deadline) {
				CHECK(rate.after(deadline - 1) == index);
			}
		}
	}
	const dvc::Rate rate{60, 1};
	CHECK(dvc::eligible(2, 1, 100, 100 + rate.window(), rate));
	CHECK(!dvc::eligible(2, 1, 100, 101 + rate.window(), rate));
	CHECK(!dvc::eligible(2, 2, 100, 100, rate));
	CHECK(dvc::eligible(2, 1, 100 + rate.window(), 100, rate));
	CHECK(!dvc::eligible(2, 1, 101 + rate.window(), 100, rate));
	const uint64_t epoch = 1000000000;
	for (uint64_t index = 0; index < 60000; ++index) {
		CHECK(epoch + 6000000 + fractional.offset(index) - (epoch + 3000000 + fractional.offset(index)) ==
		      3000000);
	}
}

static void test_pixels()
{
	const uint8_t black[] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
	uint8_t result[32] = {};
	dvc::convert(black, result, dvc::PixelFormat::NV12, 2, 2, false);
	CHECK(result[0] == 16 && result[3] == 16 && result[4] == 128 && result[5] == 128);
	dvc::convert(black, result, dvc::PixelFormat::YUY2, 2, 2, false);
	CHECK(result[0] == 16 && result[1] == 128 && result[2] == 16 && result[3] == 128);
	const uint8_t colors[] = {1, 2, 3, 17, 4, 5, 6, 18, 7, 8, 9, 19, 10, 11, 12, 20};
	dvc::convert(colors, result, dvc::PixelFormat::RGB24, 2, 2, true);
	CHECK(dvc::image_size(dvc::PixelFormat::RGB24, 2, 2) == 16);
	CHECK(result[0] == 7 && result[3] == 10 && result[6] == 0 && result[7] == 0);
	CHECK(result[8] == 1 && result[11] == 4 && result[14] == 0 && result[15] == 0);
	dvc::convert(colors, result, dvc::PixelFormat::ARGB32, 2, 2, false);
	CHECK(result[0] == 1 && result[2] == 3 && result[3] == 255 && result[15] == 255);
	dvc::convert(colors, result, dvc::PixelFormat::RGB32, 2, 2, false);
	CHECK(result[0] == 1 && result[3] == 0 && result[15] == 0);
}

static std::wstring unique_session()
{
	GUID id;
	CHECK(SUCCEEDED(CoCreateGuid(&id)));
	wchar_t value[40];
	CHECK(StringFromGUID2(id, value, 40));
	return value;
}

static void test_queue()
{
	for (uint32_t slots = 3; slots <= 5; ++slots) {
		const std::wstring session = unique_session();
		dvc::Queue writer, first, second;
		const dvc::Rate rate{60, 1};
		CHECK(writer.create(session, 2, 2, slots, rate));
		CHECK(first.open(session));
		CHECK(second.open(session));
		CHECK(writer.header()->target_frames == 2);
		std::vector<uint8_t> pixels(16, 1), received;
		const uint64_t now = 1000000000;
		CHECK(writer.write(pixels.data(), 8, now - 9000000));
		CHECK(dvc::load(writer.header()->frames[0].timestamp) == now - 9000000);
		pixels[0] = 2;
		CHECK(writer.write(pixels.data(), 8, now - 4000000));
		uint64_t consumed = 0, skipped = 0, other_consumed = 0;
		CHECK(first.read(consumed, now, received, skipped));
		CHECK(consumed == 1 && received[0] == 1);
		CHECK(second.read(other_consumed, now, received, skipped));
		CHECK(other_consumed == 1);
		CHECK(first.read(consumed, now, received, skipped));
		CHECK(consumed == 2 && received[0] == 2);
		CHECK(!first.read(consumed, now, received, skipped));
		CHECK(!second.read(other_consumed, now + rate.window(), received, skipped));
		for (uint32_t index = 0; index < slots + 4; ++index) {
			pixels[0] = uint8_t(index);
			CHECK(writer.write(pixels.data(), 8, now + index));
		}
		CHECK(first.read(consumed, now + slots + 4, received, skipped));
		CHECK(received[0] == 4 && skipped == 4);
		const int client = first.attach(1234);
		CHECK(client >= 0);
		CHECK(writer.header()->clients[client].pid == GetCurrentProcessId());
		first.detach(client, 999);
		CHECK(writer.header()->clients[client].pid != 0);
		first.detach(client, 1234);
		CHECK(writer.header()->clients[client].pid == 0);
		dvc::store(writer.header()->active, 0);
		CHECK(!first.read(consumed, now + slots + 4, received, skipped));
		CHECK(!writer.write(pixels.data(), 8, now));
	}
}

static void test_queue_formats()
{
	const uint32_t width = 2, height = 2;
	const uint8_t bgra[] = {1, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
	const uint64_t target = 1000000000;
	for (const auto format : {dvc::PixelFormat::NV12, dvc::PixelFormat::YUY2, dvc::PixelFormat::RGB24,
				  dvc::PixelFormat::RGB32, dvc::PixelFormat::ARGB32}) {
		const std::wstring session = unique_session();
		dvc::Queue writer, reader;
		CHECK(writer.create(session, width, height, 3, {60, 1}, format));
		CHECK(reader.open(session));
		CHECK(reader.header()->pixel_format == uint32_t(format));
		std::vector<uint8_t> expected(dvc::image_size(format, width, height));
		dvc::convert(bgra, expected.data(), format, width, height, false);
		const size_t stride = dvc::image_stride(format, width);
		const size_t row_bytes = format == dvc::PixelFormat::RGB24 ? width * 3 : stride;
		const uint32_t strides[] = {32, 16};
		std::vector<uint8_t> first(strides[0] * height, 0xDD), second(strides[1], 0xEE);
		for (uint32_t row = 0; row < height; ++row) {
			memcpy(first.data() + row * strides[0], expected.data() + row * stride, row_bytes);
		}
		const uint8_t *planes[] = {first.data(), nullptr};
		if (format == dvc::PixelFormat::NV12) {
			CHECK(!writer.write_planes(planes, strides, target));
			memcpy(second.data(), expected.data() + width * height, width);
			planes[1] = second.data();
		}
		CHECK(writer.write_planes(planes, strides, target - 24000000));
		uint64_t consumed = 0, skipped = 0;
		std::vector<uint8_t> actual;
		CHECK(!reader.read(consumed, target, actual, skipped));
		CHECK(writer.write_planes(planes, strides, target - 2000000));
		uint64_t timestamp = 0;
		CHECK(reader.read(consumed, target, actual, skipped, &timestamp));
		CHECK(timestamp == target - 2000000);
		CHECK(actual == expected);
		CHECK(dvc::load(writer.header()->published) == 2);
		CHECK(!reader.read(consumed, target + 1000, actual, skipped));
	}
}

static void save_card(const std::string &path, const uint8_t *image, uint32_t width, uint32_t height)
{
	FILE *file = fopen(path.c_str(), "wb");
	CHECK(file);
	BITMAPFILEHEADER header = {};
	header.bfType = 0x4d42;
	header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
	header.bfSize = header.bfOffBits + width * height * 4;
	BITMAPINFOHEADER info = {};
	info.biSize = sizeof(info);
	info.biWidth = LONG(width);
	info.biHeight = -LONG(height);
	info.biPlanes = 1;
	info.biBitCount = 32;
	CHECK(fwrite(&header, sizeof(header), 1, file) == 1);
	CHECK(fwrite(&info, sizeof(info), 1, file) == 1);
	CHECK(fwrite(image, size_t(width) * height * 4, 1, file) == 1);
	CHECK(fclose(file) == 0);
}

static void test_card(const char *image_prefix)
{
	const uint32_t width = 1280, height = 720;
	dvc::TestCard card(width, height);
	CHECK(card.valid());
	dvc::CardFrame frame{12345, 1000000123456ULL, 999980123456ULL, {60000, 1001}, {60, 1}};
	CHECK(card.draw(dvc::TestCardMode::Off, frame) == nullptr);
	const uint8_t *first = card.draw(dvc::TestCardMode::Motion, frame);
	CHECK(first);
	CHECK(first[0] == 235 && first[1] == 235 && first[2] == 235);
	std::vector<uint8_t> previous(first, first + size_t(width) * height * 4);
	++frame.sequence;
	const uint8_t *second = card.draw(dvc::TestCardMode::Motion, frame);
	CHECK(memcmp(previous.data(), second, previous.size()) != 0);
	if (image_prefix) {
		save_card(std::string(image_prefix) + "-motion.bmp", second, width, height);
	}
	const std::wstring session = unique_session();
	dvc::Queue writer, reader;
	CHECK(writer.create(session, width, height, 3, frame.output_rate));
	CHECK(reader.open(session));
	CHECK(writer.write(second, card.stride(), frame.source_timestamp));
	uint64_t consumed = 0, skipped = 0;
	std::vector<uint8_t> received;
	CHECK(reader.read(consumed, frame.source_timestamp + 1000000, received, skipped));
	CHECK(memcmp(received.data(), second, received.size()) == 0);
	CHECK(!reader.read(consumed, frame.source_timestamp + 2000000, received, skipped));
	dvc::TestCard other_card(width, height);
	auto other_frame = frame;
	++other_frame.sequence;
	const uint8_t *motion = other_card.draw(dvc::TestCardMode::Motion, other_frame);
	CHECK(motion && memcmp(motion, second, received.size()) != 0);
	dvc::Queue other_writer, other_reader;
	const std::wstring other_session = unique_session();
	CHECK(other_writer.create(other_session, width, height, 5, {60000, 1001}));
	CHECK(other_reader.open(other_session));
	dvc::store(writer.header()->phase, 3000000);
	dvc::store(other_writer.header()->phase, 6000000);
	CHECK(other_writer.write(motion, other_card.stride(), frame.source_timestamp));
	uint64_t other_consumed = 0, other_skipped = 0;
	std::vector<uint8_t> other_received;
	CHECK(other_reader.read(other_consumed, frame.source_timestamp + 1000000, other_received, other_skipped));
	CHECK(memcmp(other_received.data(), motion, other_received.size()) == 0);
	++frame.sequence;
	CHECK(card.draw(dvc::TestCardMode::Motion, frame));
	dvc::store(writer.header()->phase, 1000000);
	CHECK(dvc::load(other_writer.header()->phase) == 6000000);
	CHECK(memcmp(other_received.data(), motion, other_received.size()) == 0);
	dvc::TestCard too_small(16, 16), invalid(0, 720);
	CHECK(too_small.draw(dvc::TestCardMode::Motion, frame));
	CHECK(!invalid.valid());
}

int main(int count, char **arguments)
{
	if (count == 3 && strcmp(arguments[1], "--reader-crash-helper") == 0) {
		return frame_buffer_reader_helper(arguments[2]);
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	if (count == 2 && strcmp(arguments[1], "--buffer-only") == 0) {
		test_frame_buffer();
		return 0;
	}
	if (count == 2 && strcmp(arguments[1], "--parallel-receivers-only") == 0) {
		test_camera_parallel_receivers();
		return 0;
	}
	if (count == 2 && strcmp(arguments[1], "--phase-updates-only") == 0) {
		test_camera_phase_updates();
		return 0;
	}
	if (count == 2 && strcmp(arguments[1], "--cadence-only") == 0) {
		test_camera_cadence();
		return 0;
	}
	if (count != 1 && !(count == 3 && strcmp(arguments[1], "--test-card-images") == 0)) {
		return 2;
	}
	test_timing();
	test_frame_buffer();
	test_pixels();
	test_queue();
	test_queue_formats();
	test_camera();
	test_camera_cadence();
	test_camera_parallel_receivers();
	test_camera_phase_updates();
	test_card(count == 3 && strcmp(arguments[1], "--test-card-images") == 0 ? arguments[2] : nullptr);
	puts("dynamic virtual camera timing, selection, conversion, IPC and test card tests passed");
	return 0;
}
