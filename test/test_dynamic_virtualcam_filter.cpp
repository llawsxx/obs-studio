#include "filter.hpp"

#include <cstdio>
#include <cstdlib>
#include <numeric>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

namespace dvc {
std::atomic<long> object_count{0};
}

using dvc::ComPtr;

class Sink final : public IPin, public IMemInputPin {
	std::atomic<ULONG> references{1};
	ComPtr<IPin> source;
	ComPtr<IMemAllocator> allocator;
	std::atomic<bool> flushing{false};
	LONG expected;
	dvc::Rate rate;
	REFERENCE_TIME previous = -1;
	REFERENCE_TIME first_timestamp = -1;
	LONGLONG previous_media = -1;
	bool check_media;
	uint64_t previous_pts_index = 0;

public:
	std::atomic<uint64_t> received{0};
	std::atomic<uint64_t> distinct{0};
	std::atomic<uint64_t> maximum_gap{0};
	uint8_t previous_pixel = 0;
	uint64_t first_receive = 0, last_receive = 0;
	size_t row_stride = 0;
	uint32_t rows = 0;
	std::atomic<uint64_t> oriented{0};
	ComPtr<IReferenceClock> timing_clock;
	REFERENCE_TIME graph_start = 0;
	std::atomic<uint64_t> maximum_pts_late{0};
	std::atomic<DWORD> stall_ms{0};
	std::atomic<bool> inside_receive{false};
	std::atomic<uint64_t> stalls{0};
	explicit Sink(LONG size, dvc::Rate rate = {60000, 1001}, bool check_media = false)
		: expected(size),
		  rate(rate),
		  check_media(check_media)
	{
	}
	STDMETHODIMP QueryInterface(REFIID iid, void **output) override
	{
		if (!output) {
			return E_POINTER;
		}
		*output = nullptr;
		if (iid == IID_IUnknown || iid == IID_IPin) {
			*output = static_cast<IPin *>(this);
		} else if (iid == IID_IMemInputPin) {
			*output = static_cast<IMemInputPin *>(this);
		} else {
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
	STDMETHODIMP_(ULONG) Release() override
	{
		const ULONG remaining = --references;
		if (!remaining) {
			delete this;
		}
		return remaining;
	}
	STDMETHODIMP Connect(IPin *, const AM_MEDIA_TYPE *) override { return E_UNEXPECTED; }
	STDMETHODIMP ReceiveConnection(IPin *pin, const AM_MEDIA_TYPE *media) override
	{
		if (!media || LONG(media->lSampleSize) != expected) {
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		source = pin;
		return S_OK;
	}
	STDMETHODIMP Disconnect() override
	{
		source.Reset();
		return S_OK;
	}
	STDMETHODIMP ConnectedTo(IPin **output) override { return source.CopyTo(output); }
	STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *) override { return E_NOTIMPL; }
	STDMETHODIMP QueryPinInfo(PIN_INFO *) override { return E_NOTIMPL; }
	STDMETHODIMP QueryDirection(PIN_DIRECTION *output) override
	{
		*output = PINDIR_INPUT;
		return S_OK;
	}
	STDMETHODIMP QueryId(LPWSTR *) override { return E_NOTIMPL; }
	STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *) override { return S_OK; }
	STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **) override { return E_NOTIMPL; }
	STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override { return E_NOTIMPL; }
	STDMETHODIMP EndOfStream() override { return S_OK; }
	STDMETHODIMP BeginFlush() override
	{
		flushing = true;
		return S_OK;
	}
	STDMETHODIMP EndFlush() override
	{
		flushing = false;
		return S_OK;
	}
	STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
	STDMETHODIMP GetAllocator(IMemAllocator **output) override
	{
		if (!allocator) {
			const HRESULT result = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
								IID_PPV_ARGS(&allocator));
			if (FAILED(result)) {
				return result;
			}
		}
		return allocator.CopyTo(output);
	}
	STDMETHODIMP NotifyAllocator(IMemAllocator *value, BOOL) override
	{
		allocator = value;
		return S_OK;
	}
	STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *) override { return E_NOTIMPL; }
	STDMETHODIMP Receive(IMediaSample *sample) override
	{
		if (flushing) {
			return S_FALSE;
		}
		CHECK(sample->GetActualDataLength() == expected);
		REFERENCE_TIME start, end;
		CHECK(SUCCEEDED(sample->GetTime(&start, &end)));
		CHECK(start > previous && end > start);
		CHECK(end - start >= REFERENCE_TIME(rate.offset(1) / 100));
		CHECK(end - start <= REFERENCE_TIME(rate.offset(1) / 100 + 1));
		previous = start;
		if (check_media) {
			LONGLONG media_start, media_end;
			CHECK(SUCCEEDED(sample->GetMediaTime(&media_start, &media_end)));
			CHECK(media_start == previous_media + 1 && media_end == media_start + 1);
			if (first_timestamp < 0) {
				first_timestamp = start;
			}
			const uint64_t pts_index = rate.index_at((uint64_t(start - first_timestamp) + 1) * 100);
			CHECK(start - first_timestamp == REFERENCE_TIME(rate.offset(pts_index) / 100));
			CHECK(pts_index >= uint64_t(media_start));
			if (media_start && pts_index > previous_pts_index + 1) {
				CHECK(sample->IsDiscontinuity() == S_OK);
			}
			previous_pts_index = pts_index;
			previous_media = media_start;
		}
		BYTE *data = nullptr;
		CHECK(SUCCEEDED(sample->GetPointer(&data)));
		if (rows && (data[0] || oriented.load())) {
			CHECK(data[0] == 0xee && data[size_t(rows - 1) * row_stride] == 0x11);
			++oriented;
		}
		if (data[0] != previous_pixel) {
			++distinct;
		}
		previous_pixel = data[0];
		const uint64_t current = dvc::now_ns();
		if (last_receive) {
			maximum_gap = std::max(maximum_gap.load(), current - last_receive);
		}
		last_receive = current;
		if (!first_receive) {
			first_receive = last_receive;
		}
		if (timing_clock) {
			REFERENCE_TIME current = 0;
			CHECK(SUCCEEDED(timing_clock->GetTime(&current)));
			const uint64_t late =
				current > graph_start + start ? uint64_t(current - graph_start - start) * 100 : 0;
			maximum_pts_late = std::max(maximum_pts_late.load(), late);
		}
		const uint64_t delivered = ++received;
		const DWORD delay = stall_ms.load();
		if (delay && !(delivered % 20) && stalls < 3) {
			++stalls;
			inside_receive = true;
			Sleep(delay);
			inside_receive = false;
		}
		return S_OK;
	}
	STDMETHODIMP ReceiveMultiple(IMediaSample **samples, LONG count, LONG *processed) override
	{
		*processed = 0;
		for (LONG index = 0; index < count; ++index) {
			const HRESULT result = Receive(samples[index]);
			if (result != S_OK) {
				return result;
			}
			++*processed;
		}
		return S_OK;
	}
	STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }
};

static std::wstring new_session()
{
	GUID id;
	CHECK(SUCCEEDED(CoCreateGuid(&id)));
	wchar_t value[40];
	CHECK(StringFromGUID2(id, value, 40));
	return value;
}

static void wait_frames(Sink *sink, uint64_t count)
{
	const uint64_t until = dvc::now_ns() + 3000000000;
	while (sink->received < count && dvc::now_ns() < until) {
		Sleep(1);
	}
	CHECK(sink->received >= count);
}

static void test_pin_properties(IPin *pin)
{
	ComPtr<IKsPropertySet> properties;
	CHECK(SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&properties))));
	DWORD support = 0;
	CHECK(properties->QuerySupported(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, &support) == S_OK);
	CHECK(support == KSPROPERTY_SUPPORT_GET);
	DWORD returned = 0;
	CHECK(properties->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, nullptr, 0, &returned) == S_OK);
	CHECK(returned == sizeof(GUID));
	GUID category = GUID_NULL;
	CHECK(properties->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category),
			      &returned) == S_OK);
	CHECK(category == PIN_CATEGORY_CAPTURE && returned == sizeof(GUID));
	CHECK(properties->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category) - 1,
			      &returned) == E_UNEXPECTED);
	CHECK(properties->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, nullptr, 0, nullptr) == E_POINTER);
	CHECK(properties->QuerySupported(GUID_NULL, AMPROPERTY_PIN_CATEGORY, &support) == E_PROP_SET_UNSUPPORTED);
	CHECK(support == 0);
	CHECK(properties->QuerySupported(AMPROPSETID_Pin, MAXDWORD, &support) == E_PROP_ID_UNSUPPORTED);
	CHECK(properties->Set(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category)) ==
	      E_NOTIMPL);
	CHECK(properties->Set(GUID_NULL, AMPROPERTY_PIN_CATEGORY, nullptr, 0, nullptr, 0) == E_PROP_SET_UNSUPPORTED);
	CHECK(properties->Set(AMPROPSETID_Pin, MAXDWORD, nullptr, 0, nullptr, 0) == E_PROP_ID_UNSUPPORTED);
	returned = MAXDWORD;
	CHECK(properties->Get(GUID_NULL, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category), &returned) ==
	      E_PROP_SET_UNSUPPORTED);
	CHECK(returned == 0);
	ComPtr<IUnknown> pin_identity, property_identity;
	CHECK(SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&pin_identity))));
	CHECK(SUCCEEDED(properties->QueryInterface(IID_PPV_ARGS(&property_identity))));
	CHECK(pin_identity.Get() == property_identity.Get());
}

static void test_camera_size(uint32_t width, uint32_t height)
{
	CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
	std::vector<uint8_t> source(size_t(width) * height * 4, 128), destination(source.size());
	const uint64_t conversion_start = dvc::now_ns();
	dvc::convert(source.data(), destination.data(), dvc::PixelFormat::NV12, width, height, false);
	printf("%ux%u NV12 conversion: %.3f ms\n", width, height, double(dvc::now_ns() - conversion_start) / 1000000.0);
	const dvc::PixelFormat formats[] = {dvc::PixelFormat::NV12, dvc::PixelFormat::YUY2, dvc::PixelFormat::RGB24,
					    dvc::PixelFormat::RGB32, dvc::PixelFormat::ARGB32};
	for (int index = 0; index < 5; ++index) {
		const std::wstring session = new_session();
		dvc::Queue writer;
		const auto format = formats[index];
		CHECK(writer.create(session, width, height, 5, {60000, 1001}, format));
		std::atomic<bool> running{true};
		std::atomic<bool> publishing{true};
		const bool rgb = format != dvc::PixelFormat::NV12 && format != dvc::PixelFormat::YUY2;
		std::thread producer([&] {
			std::vector<uint8_t> pixels(dvc::image_size(format, width, height));
			uint8_t counter = 0;
			while (running) {
				memset(pixels.data(), counter++, pixels.size());
				if (rgb) {
					pixels[0] = 0x11;
					pixels[size_t(height - 1) * dvc::image_stride(format, width)] = 0xee;
				}
				if (publishing) {
					writer.write(pixels.data(), dvc::image_stride(format, width), dvc::now_ns());
				}
				Sleep(5);
			}
		});
		ComPtr<dvc::Camera> camera;
		camera.Attach(new dvc::Camera(session));
		CHECK(camera->available());
		ComPtr<IMemInputPin> wrong_interface;
		CHECK(camera->QueryInterface(IID_PPV_ARGS(&wrong_interface)) == E_NOINTERFACE);
		ComPtr<IPin> pin;
		CHECK(SUCCEEDED(camera->QueryInterface(IID_PPV_ARGS(&pin))));
		ComPtr<IEnumPins> pins;
		CHECK(SUCCEEDED(camera->EnumPins(&pins)));
		ComPtr<IPin> enumerated;
		CHECK(pins->Next(1, &enumerated, nullptr) == S_OK);
		CHECK(enumerated.Get() == pin.Get());
		test_pin_properties(enumerated.Get());
		int count = 0, size = 0;
		CHECK(SUCCEEDED(camera->GetNumberOfCapabilities(&count, &size)));
		CHECK(count == 1 && size == sizeof(VIDEO_STREAM_CONFIG_CAPS));
		VIDEO_STREAM_CONFIG_CAPS caps;
		AM_MEDIA_TYPE *media = nullptr;
		CHECK(SUCCEEDED(camera->GetStreamCaps(0, &media, reinterpret_cast<BYTE *>(&caps))));
		auto *video_info = reinterpret_cast<VIDEOINFOHEADER *>(media->pbFormat);
		CHECK(video_info->bmiHeader.biHeight == LONG(height));
		video_info->bmiHeader.biHeight = -LONG(height);
		CHECK(camera->QueryAccept(media) == S_FALSE);
		CHECK(camera->SetFormat(media) == VFW_E_TYPE_NOT_ACCEPTED);
		video_info->bmiHeader.biHeight = LONG(height);
		CHECK(camera->QueryAccept(media) == S_OK);
		const ULONG length = media->cbFormat;
		media->cbFormat = 1;
		CHECK(camera->QueryAccept(media) == S_FALSE);
		media->cbFormat = length;
		CHECK(SUCCEEDED(camera->SetFormat(media)));
		ComPtr<Sink> sink;
		sink.Attach(new Sink(LONG(media->lSampleSize)));
		if (rgb) {
			sink->row_stride = dvc::image_stride(format, width);
			sink->rows = height;
		}
		CHECK(SUCCEEDED(camera->Connect(sink.Get(), media)));
		CHECK(camera->SetFormat(media) == VFW_E_NOT_STOPPED);
		ComPtr<IReferenceClock> clock;
		CHECK(SUCCEEDED(
			CoCreateInstance(CLSID_SystemClock, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&clock))));
		CHECK(SUCCEEDED(camera->SetSyncSource(clock.Get())));
		CHECK(SUCCEEDED(camera->Pause()));
		FILTER_STATE state;
		CHECK(camera->GetState(0, &state) == VFW_S_CANT_CUE && state == State_Paused);
		REFERENCE_TIME now;
		clock->GetTime(&now);
		CHECK(SUCCEEDED(camera->Run(now)));
		const uint64_t target_frames = width > 2 ? 60 : 12;
		wait_frames(sink.Get(), target_frames / 2);
		if (rgb) {
			publishing = false;
		}
		dvc::store(writer.header()->phase, 6000000);
		wait_frames(sink.Get(), target_frames);
		CHECK(SUCCEEDED(camera->Stop()));
		printf("DirectShow %ux%u format %d: %llu delivered, %llu distinct images, %.2f FPS\n", width, height,
		       index, static_cast<unsigned long long>(sink->received.load()),
		       static_cast<unsigned long long>(sink->distinct.load()),
		       double(sink->received - 1) * 1000000000.0 / double(sink->last_receive - sink->first_receive));
		CHECK(rgb ? sink->oriented > 3 && dvc::load(writer.header()->repeated) > 0 : sink->distinct > 3);
		CHECK(SUCCEEDED(camera->Disconnect()));
		CHECK(SUCCEEDED(sink->Disconnect()));
		dvc::free_media(*media);
		CoTaskMemFree(media);
		running = false;
		producer.join();
		pins.Reset();
		enumerated.Reset();
		pin.Reset();
		camera.Reset();
		CHECK(dvc::object_count == 0);
		for (const auto &client : writer.header()->clients) {
			CHECK(client.pid == 0);
		}
		dvc::store(writer.header()->active, 0);
		ComPtr<dvc::Camera> unavailable;
		unavailable.Attach(new dvc::Camera(session));
		CHECK(!unavailable->available());
	}
	CoUninitialize();
}

void test_camera()
{
	test_camera_size(2, 2);
	test_camera_size(1920, 1080);
}

void test_camera_cadence()
{
	CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
	const dvc::Rate rate{60, 1};
	const uint32_t width = 1080, height = 1920;
	for (const uint64_t phase : {8000000ULL, 15000000ULL}) {
		const std::wstring session = new_session();
		dvc::Queue writer;
		CHECK(writer.create(session, width, height, 3, rate, dvc::PixelFormat::NV12));
		dvc::store(writer.header()->phase, phase);
		std::atomic<bool> running{true};
		std::thread producer([&] {
			std::vector<uint8_t> pixels(dvc::image_size(dvc::PixelFormat::NV12, width, height));
			dvc::Handle timer(CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS));
			if (!timer) {
				timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
			}
			CHECK(timer);
			const uint64_t epoch = dvc::now_ns() + 50000000;
			for (uint64_t index = 0; running; ++index) {
				memset(pixels.data(), uint8_t(index * 8), pixels.size());
				const int64_t jitter = index ? (int64_t(index % 3) - 1) * 2000000 : 0;
				const uint64_t target = uint64_t(int64_t(epoch + rate.offset(index)) + jitter);
				while (running) {
					const uint64_t now = dvc::now_ns();
					if (now >= target) {
						break;
					}
					LARGE_INTEGER due;
					due.QuadPart = -LONGLONG((target - now + 99) / 100);
					CHECK(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE));
					CHECK(WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0);
				}
				if (running) {
					writer.write(pixels.data(), width, 500000000000ULL + rate.offset(index));
				}
			}
		});
		ComPtr<dvc::Camera> camera;
		camera.Attach(new dvc::Camera(session));
		AM_MEDIA_TYPE *media = nullptr;
		VIDEO_STREAM_CONFIG_CAPS caps;
		CHECK(SUCCEEDED(camera->GetStreamCaps(0, &media, reinterpret_cast<BYTE *>(&caps))));
		ComPtr<Sink> sink;
		sink.Attach(new Sink(LONG(media->lSampleSize), rate));
		CHECK(SUCCEEDED(camera->Connect(sink.Get(), media)));
		CHECK(SUCCEEDED(camera->Run(0)));
		wait_frames(sink.Get(), 120);
		CHECK(SUCCEEDED(camera->Stop()));
		running = false;
		producer.join();
		const uint64_t delivered = dvc::load(writer.header()->delivered);
		const uint64_t repeats = dvc::load(writer.header()->repeated);
		printf("60 FPS producer, +/-2ms jitter, phase %.1fms: %llu/%llu fresh, %llu repeats, %.2f fresh FPS\n",
		       double(phase) / 1000000.0, static_cast<unsigned long long>(delivered - repeats),
		       static_cast<unsigned long long>(delivered), static_cast<unsigned long long>(repeats),
		       double(delivered - repeats) / delivered * double(delivered - 1) * 1000000000.0 /
			       double(sink->last_receive - sink->first_receive));
		CHECK(SUCCEEDED(camera->Disconnect()));
		CHECK(SUCCEEDED(sink->Disconnect()));
		dvc::free_media(*media);
		CoTaskMemFree(media);
	}
	CHECK(dvc::object_count == 0);
	CoUninitialize();
}

void test_camera_parallel_receivers()
{
	CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
	for (const dvc::Rate rate :
	     {dvc::Rate{60, 1}, dvc::Rate{60000, 1001}, dvc::Rate{60, 1, true}, dvc::Rate{60000, 1001, true}}) {
		printf("test_camera_parallel_receivers fixed_interval=%d\n", rate.fixed_interval);
		for (const size_t clients : {size_t(1), size_t(2)}) {
			const std::wstring session = new_session();
			const uint32_t width = 1080, height = 1920;
			dvc::Queue writer;
			CHECK(writer.create(session, width, height, 4, rate, dvc::PixelFormat::NV12));
			std::atomic<bool> running{true};
			std::thread producer([&] {
				std::vector<uint8_t> pixels(dvc::image_size(dvc::PixelFormat::NV12, width, height));
				dvc::Handle timer(CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS));
				CHECK(timer);
				const uint64_t epoch = dvc::now_ns() + 50000000;
				for (uint64_t index = 0; running; ++index) {
					const uint64_t target = epoch + rate.offset(index);
					while (running && dvc::now_ns() < target) {
						const uint64_t current = dvc::now_ns();
						if (current >= target) {
							break;
						}
						LARGE_INTEGER due;
						due.QuadPart = -LONGLONG((target - current + 99) / 100);
						CHECK(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE));
						CHECK(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0);
					}
					if (running) {
						memset(pixels.data(), uint8_t(index + 1), pixels.size());
						writer.write(pixels.data(), width,
							     500000000000ULL + rate.offset(index));
					}
				}
			});
			ComPtr<IReferenceClock> clock;
			CHECK(SUCCEEDED(CoCreateInstance(CLSID_SystemClock, nullptr, CLSCTX_INPROC_SERVER,
							 IID_PPV_ARGS(&clock))));
			REFERENCE_TIME graph_start = 0;
			CHECK(SUCCEEDED(clock->GetTime(&graph_start)));
			std::array<ComPtr<dvc::Camera>, 2> cameras;
			std::array<ComPtr<Sink>, 2> sinks;
			for (size_t index = 0; index < clients; ++index) {
				cameras[index].Attach(new dvc::Camera(session));
				AM_MEDIA_TYPE *media = nullptr;
				VIDEO_STREAM_CONFIG_CAPS caps;
				CHECK(SUCCEEDED(
					cameras[index]->GetStreamCaps(0, &media, reinterpret_cast<BYTE *>(&caps))));
				sinks[index].Attach(new Sink(LONG(media->lSampleSize), rate, true));
				sinks[index]->timing_clock = clock;
				sinks[index]->graph_start = graph_start;
				CHECK(SUCCEEDED(cameras[index]->Connect(sinks[index].Get(), media)));
				dvc::free_media(*media);
				CoTaskMemFree(media);
				CHECK(SUCCEEDED(cameras[index]->SetSyncSource(clock.Get())));
				CHECK(SUCCEEDED(cameras[index]->Run(graph_start)));
			}
			for (size_t index = 0; index < clients; ++index) {
				wait_frames(sinks[index].Get(), 120);
				printf("%u/%u FPS, %zu clients, receiver %zu: %llu/%llu distinct before stalls\n",
				       rate.numerator, rate.denominator, clients, index,
				       static_cast<unsigned long long>(sinks[index]->distinct.load()),
				       static_cast<unsigned long long>(sinks[index]->received.load()));
				CHECK(sinks[index]->distinct * 100 >= sinks[index]->received * 93);
			}
			Sink *slow = sinks[clients - 1].Get();
			slow->stall_ms = 200;
			const uint64_t stall_deadline = dvc::now_ns() + 2000000000;
			while (!slow->inside_receive && dvc::now_ns() < stall_deadline) {
				Sleep(1);
			}
			CHECK(slow->inside_receive);
			auto &slow_client = writer.header()->clients[clients - 1];
			const uint64_t captured = dvc::load(slow_client.captured);
			Sleep(80);
			CHECK(slow->inside_receive && dvc::load(slow_client.captured) >= captured + 2);
			CHECK(dvc::load(slow_client.buffered) <= writer.header()->slots);
			wait_frames(slow, slow->received + 85);
			for (size_t index = 0; index < clients; ++index) {
				CHECK(SUCCEEDED(cameras[index]->Stop()));
			}
			running = false;
			producer.join();
			CHECK(slow->stalls == 3);
			uint64_t skipped = 0, maximum_receive = 0;
			for (const auto &client : writer.header()->clients) {
				if (!client.pid) {
					continue;
				}
				skipped += dvc::load(client.pts_skipped);
				maximum_receive = std::max(maximum_receive, dvc::load(client.max_receive_ns));
				CHECK(!dvc::load(client.read_timeouts));
			}
			CHECK(skipped >= 25 && maximum_receive >= 190000000);
			for (size_t index = 0; index < clients; ++index) {
				printf("Receiver %zu after three 200ms stalls: max PTS lateness %.3fms, %llu timestamp slots skipped\n",
				       index, double(sinks[index]->maximum_pts_late) / 1000000.0,
				       static_cast<unsigned long long>(skipped));
				CHECK(sinks[index]->maximum_pts_late < rate.offset(2) + 5000000);
				if (index + 1 < clients) {
					CHECK(sinks[index]->distinct * 100 >= sinks[index]->received * 93);
				}
				CHECK(SUCCEEDED(cameras[index]->Disconnect()));
				CHECK(SUCCEEDED(sinks[index]->Disconnect()));
			}
		}
	}
	CHECK(dvc::object_count == 0);
	CoUninitialize();
}

void test_camera_phase_updates()
{
	CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
	for (const dvc::Rate rate :
	     {dvc::Rate{60, 1}, dvc::Rate{60000, 1001}, dvc::Rate{60, 1, true}, dvc::Rate{60000, 1001, true}}) {
		printf("test_camera_phase_updates fixed_interval=%d\n", rate.fixed_interval);
		const std::wstring session = new_session();
		dvc::Queue writer;
		CHECK(writer.create(session, 16, 16, 3, rate, dvc::PixelFormat::NV12));
		std::vector<uint8_t> pixels(dvc::image_size(dvc::PixelFormat::NV12, 16, 16), 16);
		CHECK(writer.write(pixels.data(), 16, 0));
		CHECK(writer.write(pixels.data(), 16, rate.offset(1)));
		ComPtr<dvc::Camera> camera;
		camera.Attach(new dvc::Camera(session));
		AM_MEDIA_TYPE *media = nullptr;
		VIDEO_STREAM_CONFIG_CAPS caps;
		CHECK(SUCCEEDED(camera->GetStreamCaps(0, &media, reinterpret_cast<BYTE *>(&caps))));
		ComPtr<Sink> sink;
		sink.Attach(new Sink(LONG(media->lSampleSize), rate, true));
		CHECK(SUCCEEDED(camera->Connect(sink.Get(), media)));
		ComPtr<IReferenceClock> clock;
		CHECK(SUCCEEDED(
			CoCreateInstance(CLSID_SystemClock, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&clock))));
		CHECK(SUCCEEDED(camera->SetSyncSource(clock.Get())));
		REFERENCE_TIME run_start;
		CHECK(SUCCEEDED(clock->GetTime(&run_start)));
		CHECK(SUCCEEDED(camera->Run(run_start)));
		wait_frames(sink.Get(), 5);
		const uint64_t before = sink->received, before_distinct = sink->distinct;
		dvc::Handle timer(CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS));
		if (!timer) {
			timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
		}
		CHECK(timer);
		const uint64_t start = dvc::now_ns(), duration = 1200000000, period = rate.offset(1);
		uint64_t source_index = 2;
		for (uint64_t tick = 1; dvc::now_ns() - start < duration; ++tick) {
			const uint64_t phase = dvc::mul_div(tick, period - 1, 400) % period;
			dvc::store(writer.header()->phase, tick < 600 ? phase : period - 1 - phase);
			if (dvc::now_ns() >= start + rate.offset(source_index)) {
				++pixels[0];
				writer.write(pixels.data(), 16, rate.offset(source_index));
				++source_index;
			}
			const uint64_t now = dvc::now_ns(), target = start + tick * 1000000;
			if (now < target) {
				LARGE_INTEGER due;
				due.QuadPart = -LONGLONG((target - now + 99) / 100);
				CHECK(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE));
				CHECK(WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0);
			}
		}
		const uint64_t delivered = sink->received - before, distinct = sink->distinct - before_distinct;
		printf("Live phase sweep %u/%u: %llu delivered, %llu distinct during adjustment\n", rate.numerator,
		       rate.denominator, static_cast<unsigned long long>(delivered),
		       static_cast<unsigned long long>(distinct));
		CHECK(delivered >= 30 && delivered <= 80 && distinct >= 20);
		dvc::store(writer.header()->phase, 0);
		wait_frames(sink.Get(), sink->received + 5);
		dvc::store(writer.header()->phase, period - 1);
		wait_frames(sink.Get(), sink->received + 5);
		CHECK(SUCCEEDED(camera->Stop()));
		CHECK(sink->maximum_gap < 250000000);
		CHECK(SUCCEEDED(camera->Disconnect()));
		CHECK(SUCCEEDED(sink->Disconnect()));
		dvc::free_media(*media);
		CoTaskMemFree(media);
	}
	CHECK(dvc::object_count == 0);
	CoUninitialize();
}
