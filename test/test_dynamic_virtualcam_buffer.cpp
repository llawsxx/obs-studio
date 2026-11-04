#include "ipc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

static std::wstring buffer_session()
{
	GUID id;
	wchar_t text[40];
	CHECK(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, text, 40));
	return text;
}

class BufferFixture {
public:
	std::wstring session = buffer_session();
	dvc::Queue writer;
	dvc::Queue reader;
	dvc::FrameBuffer state;
	std::vector<uint8_t> image;
	dvc::Rate rate;
	explicit BufferFixture(uint32_t target = 2, uint32_t maximum = 5, dvc::Rate rate = {60, 1}) : rate(rate)
	{
		CHECK(writer.create(session, 2, 2, maximum, rate, dvc::PixelFormat::ARGB32, target));
		CHECK(reader.open(session));
	}
	void publish(uint64_t timestamp, uint8_t value)
	{
		const uint8_t pixels[16] = {value};
		CHECK(writer.write(pixels, 8, timestamp));
	}
	bool select(uint64_t timestamp) { return reader.select(state, timestamp, image); }
};

static void test_buffer_start_and_underrun()
{
	BufferFixture fixture;
	const uint64_t origin = 500000000000000000ULL;
	CHECK(!fixture.select(0));
	fixture.publish(origin, 1);
	CHECK(!fixture.select(0));
	fixture.publish(origin + fixture.rate.offset(1), 2);
	CHECK(fixture.select(0) && fixture.image[0] == 1 && fixture.state.buffered == 1);
	CHECK(fixture.select(fixture.rate.offset(1)) && fixture.image[0] == 2);
	CHECK(!fixture.state.playing && fixture.state.buffered == 0);
	fixture.publish(origin + fixture.rate.offset(2), 3);
	CHECK(!fixture.select(fixture.rate.offset(2)));
	fixture.publish(origin + fixture.rate.offset(3), 4);
	CHECK(fixture.select(fixture.rate.offset(3)) && fixture.image[0] == 3);
	CHECK(fixture.state.rebuffers == 1);
	CHECK(!fixture.select(fixture.rate.offset(3)));
	CHECK(fixture.select(fixture.rate.offset(4)) && fixture.image[0] == 4);
	CHECK(!fixture.select(fixture.rate.offset(4)));
}

static void test_buffer_overflow()
{
	BufferFixture fixture;
	for (uint64_t index = 0; index < 2; ++index) {
		fixture.publish(17 + fixture.rate.offset(index), uint8_t(index + 1));
	}
	CHECK(fixture.select(0) && fixture.image[0] == 1);
	for (uint64_t index = 2; index < 7; ++index) {
		fixture.publish(17 + fixture.rate.offset(index), uint8_t(index + 1));
	}
	CHECK(fixture.select(fixture.rate.offset(100)) && fixture.image[0] == 6);
	CHECK(fixture.state.discarded == 4 && fixture.state.rebuffers == 1 && fixture.state.buffered == 1);
	CHECK(fixture.select(fixture.rate.offset(101)) && fixture.image[0] == 7);
	CHECK(!fixture.select(fixture.rate.offset(101)));
	BufferFixture new_receiver;
	for (uint64_t index = 0; index < 100; ++index) {
		new_receiver.publish(17 + new_receiver.rate.offset(index), uint8_t(index + 1));
	}
	CHECK(new_receiver.select(0) && new_receiver.image[0] == 99);
	CHECK(new_receiver.state.discarded == 98);
}

static void test_buffer_window()
{
	for (const dvc::Rate rate : {dvc::Rate{60, 1}, dvc::Rate{60000, 1001}}) {
		const uint64_t origin = 9000000000000000000ULL;
		const int64_t tolerance = int64_t(rate.window());
		for (const int64_t difference : {-tolerance - 1, -tolerance, int64_t(0), tolerance, tolerance + 1}) {
			dvc::FrameStamp frames[] = {{1, origin, 1},
						    {2, uint64_t(int64_t(origin + rate.offset(1)) + difference), 1},
						    {3, origin + rate.offset(4), 1}};
			dvc::FrameBuffer buffer;
			CHECK(buffer.select(frames, 3, 3, 1, 1, 0, rate, 2, 5).sequence == 1);
			buffer.consume(1, 3);
			const auto selected = buffer.select(frames, 3, 3, 1, 1, rate.offset(1), rate, 2, 5);
			if (difference >= -tolerance && difference <= tolerance) {
				CHECK(selected.sequence == 2);
			} else {
				CHECK(!selected.sequence);
				CHECK(buffer.consumed == (difference < 0 ? 2 : 1));
			}
		}
		dvc::FrameStamp frames[] = {{1, origin, 1},
					    {2, origin + rate.offset(1) - rate.window(), 1},
					    {3, origin + rate.offset(1) + rate.window(), 1}};
		dvc::FrameBuffer buffer;
		CHECK(buffer.select(frames, 3, 3, 1, 1, 0, rate, 2, 5).sequence == 1);
		buffer.consume(1, 3);
		CHECK(buffer.select(frames, 3, 3, 1, 1, rate.offset(1), rate, 2, 5).sequence == 2);
		buffer.consume(2, 3);
		CHECK(buffer.select(frames, 3, 3, 1, 1, rate.offset(1), rate, 2, 5).sequence == 3);
	}
}

static void test_buffer_discontinuity()
{
	for (const uint64_t restart : {0ULL, 9000000000000000000ULL}) {
		BufferFixture fixture;
		fixture.publish(5000000000, 1);
		fixture.publish(5000000000 + fixture.rate.offset(1), 2);
		CHECK(fixture.select(0));
		fixture.publish(restart, 3);
		CHECK(!fixture.select(fixture.rate.offset(1)));
		fixture.publish(restart + fixture.rate.offset(1), 4);
		CHECK(fixture.select(fixture.rate.offset(2)) && fixture.image[0] == 3);
		CHECK(fixture.state.rebuffers == 1);
		CHECK(fixture.select(fixture.rate.offset(3)) && fixture.image[0] == 4);
	}
}

static void test_buffer_fractional_clock()
{
	BufferFixture fixture(2, 5, {60000, 1001});
	fixture.publish(17, 0);
	fixture.publish(17 + fixture.rate.offset(1), 1);
	dvc::Queue second;
	dvc::FrameBuffer other;
	std::vector<uint8_t> image;
	CHECK(second.open(fixture.session));
	for (uint64_t index = 0; index <= 60000; ++index) {
		const uint64_t timestamp = fixture.rate.offset(index);
		CHECK(fixture.select(timestamp));
		dvc::store(fixture.writer.header()->phase, index % fixture.rate.offset(1));
		dvc::store(fixture.writer.header()->wake_epoch, index * 1000000000);
		CHECK(second.select(other, timestamp, image));
		CHECK(fixture.image == image && fixture.image[0] == uint8_t(index));
		CHECK(other.consumed == fixture.state.consumed && !other.rebuffers && !other.discarded);
		CHECK(!fixture.state.rebuffers && !fixture.state.discarded);
		fixture.publish(17 + fixture.rate.offset(index + 2), uint8_t(index + 2));
	}
	CHECK(fixture.rate.offset(60000) == 1001000000000ULL);
}

static void test_buffer_independent_receivers()
{
	BufferFixture fixture;
	dvc::Queue second;
	dvc::FrameBuffer other;
	std::vector<uint8_t> image;
	CHECK(second.open(fixture.session));
	fixture.publish(17, 1);
	fixture.publish(17 + fixture.rate.offset(1), 2);
	CHECK(fixture.select(0));
	CHECK(second.select(other, 4000000000000000000ULL, image) && image[0] == 1);
	for (uint64_t index = 1; index <= 10; ++index) {
		fixture.publish(17 + fixture.rate.offset(index + 1), uint8_t(index + 2));
		CHECK(fixture.select(fixture.rate.offset(index)) && fixture.image[0] == uint8_t(index + 1));
	}
	CHECK(!fixture.state.rebuffers && !fixture.state.discarded);
	CHECK(second.select(other, 4000000000000000000ULL + fixture.rate.offset(1), image));
	CHECK(image[0] == 11 && other.rebuffers == 1 && other.discarded == 9);
}

static void test_buffer_mismatched_rates()
{
	BufferFixture slower(2, 5, {60, 1});
	const dvc::Rate fast_source{120, 1};
	slower.publish(17, 1);
	slower.publish(17 + fast_source.offset(1), 2);
	for (uint64_t index = 0; index <= 1000; ++index) {
		if (index) {
			slower.publish(17 + fast_source.offset(index * 2), uint8_t(index * 2 + 1));
			slower.publish(17 + fast_source.offset(index * 2 + 1), uint8_t(index * 2 + 2));
		}
		CHECK(slower.select(slower.rate.offset(index)));
		CHECK(slower.state.consumed == (index ? index * 2 : 1));
		CHECK(!slower.state.rebuffers);
	}
	BufferFixture faster(2, 5, {120, 1});
	const dvc::Rate slow_source{60, 1};
	faster.publish(17, 1);
	faster.publish(17 + slow_source.offset(1), 2);
	for (uint64_t index = 0; index <= 1000; ++index) {
		if (index && !(index & 1)) {
			faster.publish(17 + slow_source.offset(index / 2 + 1), uint8_t(index / 2 + 2));
		}
		CHECK(faster.select(faster.rate.offset(index)) == !(index & 1));
		CHECK(faster.state.consumed == index / 2 + 1);
		CHECK(!faster.state.rebuffers);
	}
}

static void test_buffer_busy_slot()
{
	BufferFixture fixture;
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	dvc::Handle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	dvc::Handle slot(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
				    dvc::object_name(fixture.session, L"slot0").c_str()));
	CHECK(ready && release && slot);
	std::thread holder([&] {
		CHECK(WaitForSingleObject(slot, 5000) == WAIT_OBJECT_0);
		SetEvent(ready);
		CHECK(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0);
		ReleaseMutex(slot);
	});
	CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
	const uint8_t pixels[16] = {1};
	CHECK(!fixture.writer.write(pixels, 8, 17));
	CHECK(!dvc::load(fixture.writer.header()->published));
	SetEvent(release);
	holder.join();
	fixture.publish(17, 1);
	fixture.publish(17 + fixture.rate.offset(1), 2);
	CHECK(fixture.select(0) && fixture.image[0] == 1 && fixture.state.consumed == 1);
	CHECK(!fixture.state.discarded && dvc::load(fixture.writer.header()->dropped) == 1);
}

void test_frame_buffer()
{
	if (sizeof(size_t) == 4) {
		dvc::Queue too_large;
		CHECK(!too_large.create(buffer_session(), 8192, 8192, 30, {60, 1}));
	}
	test_buffer_start_and_underrun();
	test_buffer_overflow();
	test_buffer_window();
	test_buffer_discontinuity();
	test_buffer_fractional_clock();
	test_buffer_independent_receivers();
	test_buffer_mismatched_rates();
	test_buffer_busy_slot();
	for (const uint32_t maximum : {1U, 5U, 30U}) {
		BufferFixture fixture(1, maximum);
		fixture.publish(0, 1);
		CHECK(fixture.select(0) && fixture.image[0] == 1);
	}
	puts("Frame-count buffering, +/-0.6T, overflow/underrun, OBS discontinuities, independent clocks and receivers passed.");
}

#ifdef DVC_BUFFER_TEST_MAIN
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	test_frame_buffer();
	return 0;
}
#endif
