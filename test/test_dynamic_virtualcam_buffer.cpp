#include "ipc.hpp"
#include "frame-cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>

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
	for (const dvc::Rate rate :
	     {dvc::Rate{60, 1}, dvc::Rate{60000, 1001}, dvc::Rate{60, 1, true}, dvc::Rate{60000, 1001, true}}) {
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
	for (const dvc::Rate rate : {dvc::Rate{60000, 1001}, dvc::Rate{60000, 1001, true}}) {
		BufferFixture fixture(2, 5, rate);
		CHECK(fixture.writer.header()->fixed_interval == uint32_t(rate.fixed_interval));
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
		CHECK(fixture.rate.offset(60000) == (rate.fixed_interval ? 1000999980000ULL : 1001000000000ULL));
	}
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

static void test_buffer_contended_reader()
{
	BufferFixture fixture;
	fixture.publish(17, 1);
	fixture.publish(17 + fixture.rate.offset(1), 2);
	dvc::Handle slot(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
				    dvc::object_name(fixture.session, L"slot0").c_str()));
	dvc::Handle started(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	CHECK(slot && started && WaitForSingleObject(slot, 5000) == WAIT_OBJECT_0);
	bool selected = false;
	std::thread reader([&] {
		SetEvent(started);
		selected = fixture.select(0);
	});
	CHECK(WaitForSingleObject(started, 5000) == WAIT_OBJECT_0);
	dvc::Handle timer(CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS));
	CHECK(timer);
	LARGE_INTEGER due;
	due.QuadPart = -10000;
	CHECK(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE));
	CHECK(WaitForSingleObject(timer, 5000) == WAIT_OBJECT_0);
	ReleaseMutex(slot);
	reader.join();
	CHECK(selected && fixture.state.consumed == 1 && fixture.image[0] == 1);
	CHECK(!fixture.state.discarded);
}

static void test_buffer_shared_readers()
{
	BufferFixture fixture;
	for (uint64_t index = 0; index < 5; ++index) {
		fixture.publish(17 + fixture.rate.offset(index), uint8_t(index + 1));
	}
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	dvc::Handle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	dvc::Handle lease(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
				     dvc::object_name(fixture.session, L"reader0").c_str()));
	dvc::Handle slot(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
				    dvc::object_name(fixture.session, L"slot0").c_str()));
	CHECK(ready && release && lease && slot);
	std::thread first([&] {
		CHECK(WaitForSingleObject(lease, 5000) == WAIT_OBJECT_0);
		CHECK(WaitForSingleObject(slot, 5000) == WAIT_OBJECT_0);
		dvc::store(fixture.writer.header()->reader_slots[0], 1);
		ReleaseMutex(slot);
		SetEvent(ready);
		CHECK(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0);
		dvc::store(fixture.writer.header()->reader_slots[0], 0);
		ReleaseMutex(lease);
	});
	CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
	CHECK(fixture.select(0) && fixture.image[0] == 1);
	CHECK(dvc::load(fixture.writer.header()->reader_slots[0]) == 1);
	const uint8_t pixels[16] = {6};
	CHECK(!fixture.writer.write(pixels, 8, 17 + fixture.rate.offset(5)));
	CHECK(dvc::load(fixture.writer.header()->published) == 5);
	SetEvent(release);
	first.join();
	fixture.publish(17 + fixture.rate.offset(5), 6);
	CHECK(dvc::load(fixture.writer.header()->published) == 6);
}

static void test_buffer_parallel_readers()
{
	const std::wstring session = buffer_session();
	const dvc::Rate rate{60000, 1001};
	const uint32_t width = 1920, height = 1080;
	dvc::Queue writer;
	CHECK(writer.create(session, width, height, 5, rate, dvc::PixelFormat::NV12));
	std::vector<uint8_t> pixels(dvc::image_size(dvc::PixelFormat::NV12, width, height), 1);
	CHECK(writer.write(pixels.data(), width, 17));
	memset(pixels.data(), 2, pixels.size());
	CHECK(writer.write(pixels.data(), width, 17 + rate.offset(1)));
	std::mutex mutex;
	std::condition_variable condition;
	uint64_t round = 0;
	size_t completed = 0;
	bool running = true;
	auto consume = [&] {
		dvc::Queue reader;
		CHECK(reader.open(session));
		dvc::FrameBuffer buffer;
		std::vector<uint8_t> image;
		uint64_t previous = 0;
		for (;;) {
			std::unique_lock lock(mutex);
			condition.wait(lock, [&] { return !running || round > previous; });
			if (!running) {
				break;
			}
			previous = round;
			lock.unlock();
			CHECK(reader.select(buffer, rate.offset(previous - 1), image));
			CHECK(image.front() == uint8_t(previous) && image.back() == uint8_t(previous));
			CHECK(!buffer.discarded && !buffer.rebuffers && !reader.read_timeouts());
			lock.lock();
			++completed;
			condition.notify_all();
		}
	};
	std::thread first(consume), second(consume);
	for (uint64_t index = 1; index <= 300; ++index) {
		if (index > 1) {
			memset(pixels.data(), uint8_t(index + 1), pixels.size());
			CHECK(writer.write(pixels.data(), width, 17 + rate.offset(index)));
		}
		std::unique_lock lock(mutex);
		completed = 0;
		round = index;
		condition.notify_all();
		CHECK(condition.wait_for(lock, std::chrono::seconds(5), [&] { return completed == 2; }));
	}
	{
		std::lock_guard lock(mutex);
		running = false;
		condition.notify_all();
	}
	first.join();
	second.join();
	CHECK(!dvc::load(writer.header()->dropped));
	puts("Two parallel 1080p readers: 300/300 fresh frames each, no torn frames or writer drops");
}

int frame_buffer_reader_helper(const char *session_text)
{
	const std::string text(session_text);
	const std::wstring session(text.begin(), text.end());
	dvc::Queue reader;
	CHECK(reader.open(session));
	dvc::Handle lease(
		OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, dvc::object_name(session, L"reader0").c_str()));
	dvc::Handle slot(
		OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, dvc::object_name(session, L"slot0").c_str()));
	dvc::Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, dvc::object_name(session, L"test-ready").c_str()));
	dvc::Handle finish(OpenEventW(SYNCHRONIZE, FALSE, dvc::object_name(session, L"test-finish").c_str()));
	CHECK(lease && slot && ready && finish);
	CHECK(WaitForSingleObject(lease, 5000) == WAIT_OBJECT_0);
	CHECK(WaitForSingleObject(slot, 5000) == WAIT_OBJECT_0);
	dvc::store(reader.header()->reader_slots[0], 1);
	ReleaseMutex(slot);
	SetEvent(ready);
	const DWORD result = WaitForSingleObject(finish, 5000);
	ExitProcess(result == WAIT_OBJECT_0 ? 0 : 1);
}

static void test_buffer_reader_exit()
{
	BufferFixture fixture;
	for (uint64_t index = 0; index < 5; ++index) {
		fixture.publish(17 + fixture.rate.offset(index), uint8_t(index + 1));
	}
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(fixture.session, L"test-ready").c_str()));
	dvc::Handle finish(
		CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(fixture.session, L"test-finish").c_str()));
	wchar_t executable[MAX_PATH];
	CHECK(GetModuleFileNameW(nullptr, executable, MAX_PATH));
	std::wstring command = L"\"" + std::wstring(executable) + L"\" --reader-crash-helper " + fixture.session;
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	CHECK(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
			     &startup, &process));
	dvc::Handle child(process.hProcess), thread(process.hThread);
	CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
	const uint8_t pixels[16] = {6};
	CHECK(!fixture.writer.write(pixels, 8, 17 + fixture.rate.offset(5)));
	SetEvent(finish);
	CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
	DWORD exit_code = 1;
	CHECK(GetExitCodeProcess(child, &exit_code) && !exit_code);
	CHECK(dvc::load(fixture.writer.header()->reader_slots[0]) == 1);
	fixture.publish(17 + fixture.rate.offset(5), 6);
	CHECK(!dvc::load(fixture.writer.header()->reader_slots[0]));
	CHECK(fixture.select(0) && fixture.image[0] == 5);
	puts("Reader process exit: abandoned read lease reclaimed, publishing and reading resumed");
}

static void test_frame_cache(dvc::Rate rate)
{
	dvc::FrameCache cache(rate, 2, 5);
	std::vector<uint8_t> input, output;
	auto push = [&](uint64_t sequence, uint64_t generation = 1, uint64_t start = 1) {
		input.assign(16, uint8_t(sequence));
		cache.push({sequence, 500000000000ULL + rate.offset(sequence - 1), generation}, start, input);
		CHECK(cache.statistics().buffered <= 5);
	};
	push(1);
	CHECK(!cache.select(0, output));
	push(2);
	CHECK(cache.select(0, output) && output[0] == 1);
	CHECK(!cache.select(0, output));
	CHECK(cache.select(rate.offset(1), output) && output[0] == 2);
	push(3);
	CHECK(!cache.select(rate.offset(2), output));
	push(4);
	CHECK(cache.select(rate.offset(3), output) && output[0] == 3);
	CHECK(cache.statistics().rebuffers == 1);
	for (uint64_t sequence = 5; sequence <= 9; ++sequence) {
		push(sequence);
	}
	CHECK(cache.statistics().buffered == 2);
	CHECK(cache.select(rate.offset(100), output) && output[0] == 8);
	CHECK(cache.statistics().rebuffers == 2);
	CHECK(cache.select(rate.offset(101), output) && output[0] == 9);
	push(10, 2, 10);
	CHECK(!cache.select(rate.offset(102), output));
	push(11, 2, 10);
	CHECK(cache.select(rate.offset(102), output) && output[0] == 10);
	push(11, 2, 10);
	CHECK(cache.statistics().buffered == 1);
	CHECK(cache.select(rate.offset(103), output) && output[0] == 11);
	push(15, 2, 10);
	CHECK(!cache.select(rate.offset(104), output));
	push(16, 2, 10);
	CHECK(cache.select(rate.offset(104), output) && output[0] == 15);
	puts("Private cache: priming, one-shot selection, bounded overflow, underrun, generation and gaps passed.");
}

static void test_frame_notifications()
{
	BufferFixture fixture;
	dvc::Queue second;
	CHECK(second.open(fixture.session));
	const int first_client = fixture.reader.attach(123);
	const int second_client = second.attach(456);
	CHECK(first_client >= 0 && second_client >= 0 && first_client != second_client);
	uint64_t cursor = 0, start = 0;
	dvc::FrameStamp stamp;
	fixture.publish(500000000000ULL, 1);
	fixture.publish(500000000000ULL + fixture.rate.offset(1), 2);
	CHECK(WaitForSingleObject(fixture.reader.frame_event(first_client), 0) == WAIT_OBJECT_0);
	CHECK(WaitForSingleObject(second.frame_event(second_client), 0) == WAIT_OBJECT_0);
	CHECK(fixture.reader.next(cursor, stamp, start, fixture.image) && cursor == 1 && fixture.image[0] == 1);
	CHECK(stamp.timestamp == 500000000000ULL && start == 1);
	CHECK(fixture.reader.next(cursor, stamp, start, fixture.image) && cursor == 2 && fixture.image[0] == 2);
	CHECK(!fixture.reader.next(cursor, stamp, start, fixture.image));
	uint64_t second_cursor = 0;
	CHECK(second.next(second_cursor, stamp, start, fixture.image) && second_cursor == 1);
	fixture.reader.detach(first_client, 123);
	second.detach(second_client, 456);
	puts("Independent client notifications and immediate ordered ring draining passed.");
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
	test_buffer_contended_reader();
	test_buffer_shared_readers();
	test_buffer_parallel_readers();
	test_buffer_reader_exit();
	test_frame_cache({60000, 1001});
	test_frame_cache({60, 1, true});
	test_frame_cache({60000, 1001, true});
	test_frame_notifications();
	for (const uint32_t maximum : {1U, 5U, 30U}) {
		BufferFixture fixture(1, maximum);
		fixture.publish(0, 1);
		CHECK(fixture.select(0) && fixture.image[0] == 1);
	}
	puts("Frame-count buffering, +/-0.6T, overflow/underrun, OBS discontinuities, independent clocks and receivers passed.");
}

#ifdef DVC_BUFFER_TEST_MAIN
int main(int count, char **arguments)
{
	if (count == 3 && strcmp(arguments[1], "--reader-crash-helper") == 0) {
		return frame_buffer_reader_helper(arguments[2]);
	}
	if (count != 1) {
		return 2;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	test_frame_buffer();
	return 0;
}
#endif
