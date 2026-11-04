#include "filter.hpp"

#include <algorithm>
#include <stdexcept>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace dvc {

class DeadlineWaiter {
	Handle timer;

public:
	DeadlineWaiter()
		: timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
					       TIMER_ALL_ACCESS))
	{
		if (!timer) {
			timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
		}
		if (!timer) {
			throw std::runtime_error("Could not create virtual camera timer");
		}
	}
	bool wait(HANDLE stop, uint64_t target)
	{
		for (;;) {
			const uint64_t current = now_ns();
			if (current >= target) {
				return WaitForSingleObject(stop, 0) == WAIT_TIMEOUT;
			}
			LARGE_INTEGER due;
			due.QuadPart = -LONGLONG(std::max<uint64_t>(1, (target - current + 99) / 100));
			if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
				return false;
			}
			HANDLE events[] = {stop, timer};
			if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
				return false;
			}
		}
	}
};

void Camera::deliver()
{
	auto *header = queue.header();
	auto &client = header->clients[client_index];
	store(client.pixel_format, uint64_t(format));
	store(client.last_result, S_OK);
	store(client.streaming, 1);
	DeadlineWaiter timer;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	std::vector<uint8_t> black(size_t(width) * height * 4, 0);
	std::vector<uint8_t> converted(image_size(format, width, height));
	std::vector<uint8_t> candidate(converted.size());
	FrameBuffer buffer;
	uint64_t last_phase = UINT64_MAX;
	uint64_t deadline = 0, wake_index = 0;
	uint64_t allocation_ns = std::min<uint64_t>(1000000, rate.offset(1) / 2);
	uint64_t sample_index = 0, last_rebuffers = 0;
	REFERENCE_TIME stream_origin = 0;
	bool started = false;
	convert(black.data(), converted.data(), format, width, height, false);
	black.clear();
	black.shrink_to_fit();
	while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT && load(header->active) &&
	       process_alive(header->owner_pid, header->owner_start)) {
		const uint64_t phase = std::min(load(header->phase), rate.offset(1) - 1);
		const uint64_t anchor = load(header->wake_epoch) + phase;
		if (last_phase != phase) {
			const uint64_t now = now_ns();
			wake_index = std::max(wake_index, now < anchor ? uint64_t(0) : rate.after(now - anchor));
			deadline = anchor + rate.offset(wake_index);
			last_phase = phase;
		}
		auto advance_wait = [&] {
			const uint64_t after = now_ns();
			wake_index = std::max(wake_index + 1, rate.after(after > anchor ? after - anchor : 0));
			deadline = anchor + rate.offset(wake_index);
		};
		if (!timer.wait(stop_event, deadline > allocation_ns ? deadline - allocation_ns : 0)) {
			break;
		}
		const uint64_t prepare_start = now_ns();
		ComPtr<IMediaSample> sample;
		HRESULT result = allocator->GetBuffer(&sample, nullptr, nullptr, 0);
		store(client.last_result, uint32_t(result));
		if (FAILED(result) || WaitForSingleObject(stop_event, 0) != WAIT_TIMEOUT) {
			break;
		}
		BYTE *destination = nullptr;
		result = sample->GetPointer(&destination);
		store(client.last_result, uint32_t(result));
		if (FAILED(result) || sample->GetSize() < LONG(converted.size())) {
			if (SUCCEEDED(result)) {
				store(client.last_result, uint32_t(VFW_E_BUFFER_OVERFLOW));
			}
			break;
		}
		const uint64_t allocation_time = now_ns() - prepare_start;
		allocation_ns = std::min(rate.offset(1) - 1, std::max(allocation_time + 500000, allocation_ns * 7 / 8));
		if (!timer.wait(stop_event, deadline) || !load(header->active)) {
			break;
		}
		const uint64_t logical_timestamp = rate.offset(sample_index);
		const uint64_t select_start = now_ns();
		const bool fresh = queue.select(buffer, logical_timestamp, candidate);
		store(client.buffered, buffer.buffered);
		store(client.rebuffers, buffer.rebuffers);
		store(client.expired, buffer.discarded);
		store(client.skipped, buffer.discarded);
		if (!fresh && !started) {
			advance_wait();
			continue;
		}
		copy_image(fresh ? candidate.data() : converted.data(), destination, format, width, height, bottom_up);
		store(client.prepare_ns, now_ns() - select_start);
		sample->SetActualDataLength(LONG(converted.size()));
		if (!started) {
			ComPtr<IReferenceClock> reference;
			REFERENCE_TIME reference_now = 0;
			GetSyncSource(&reference);
			if (reference && SUCCEEDED(reference->GetTime(&reference_now))) {
				stream_origin = std::max<REFERENCE_TIME>(0, reference_now - run_start.load());
			}
		}
		REFERENCE_TIME timestamp = stream_origin + REFERENCE_TIME(logical_timestamp / 100);
		REFERENCE_TIME end = stream_origin + REFERENCE_TIME(rate.offset(sample_index + 1) / 100);
		sample->SetTime(&timestamp, &end);
		LONGLONG frame_index = LONGLONG(sample_index), frame_end = frame_index + 1;
		sample->SetMediaTime(&frame_index, &frame_end);
		sample->SetSyncPoint(TRUE);
		sample->SetPreroll(FALSE);
		sample->SetDiscontinuity(!started || buffer.rebuffers != last_rebuffers);
		const uint64_t delivery_time = now_ns();
		store(client.late_ns, delivery_time > deadline ? delivery_time - deadline : 0);
		store(client.heartbeat, delivery_time);
		const uint64_t receive_start = now_ns();
		result = receiver->Receive(sample.Get());
		store(client.receive_ns, now_ns() - receive_start);
		store(client.last_result, uint32_t(result));
		if (result != S_OK) {
			break;
		}
		if (fresh) {
			converted.swap(candidate);
		}
		started = true;
		last_rebuffers = buffer.rebuffers;
		++sample_index;
		InterlockedIncrement64(&client.delivered);
		InterlockedIncrement64(&header->delivered);
		if (!fresh) {
			InterlockedIncrement64(&client.repeated);
			InterlockedIncrement64(&header->repeated);
		}
		advance_wait();
	}
	store(client.streaming, 0);
}

}
