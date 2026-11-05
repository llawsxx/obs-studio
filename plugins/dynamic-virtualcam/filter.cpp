#include "filter.hpp"
#include "enumerators.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace dvc {

void free_media(AM_MEDIA_TYPE &value)
{
	CoTaskMemFree(value.pbFormat);
	if (value.pUnk) {
		value.pUnk->Release();
	}
	value = {};
}

HRESULT copy_media(AM_MEDIA_TYPE &destination, const AM_MEDIA_TYPE &source)
{
	destination = source;
	destination.pbFormat = nullptr;
	destination.pUnk = nullptr;
	if (source.cbFormat) {
		destination.pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(source.cbFormat));
		if (!destination.pbFormat) {
			return E_OUTOFMEMORY;
		}
		memcpy(destination.pbFormat, source.pbFormat, source.cbFormat);
	}
	destination.pUnk = source.pUnk;
	if (destination.pUnk) {
		destination.pUnk->AddRef();
	}
	return S_OK;
}

Camera::Camera(const std::wstring &session, REFCLSID id) : class_id(id)
{
	++object_count;
	stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (stop_event && queue.open(session)) {
		const auto *header = queue.header();
		width = header->width;
		height = header->height;
		rate = {header->fps_num, header->fps_den, header->fixed_interval != 0};
		format = PixelFormat(header->pixel_format);
		static std::atomic<uint64_t> serial{0};
		token = now_ns() + ++serial;
		client_index = queue.attach(token);
	}
	if (FAILED(make_media(0, &media))) {
		queue.detach(client_index, token);
		client_index = -1;
	}
}

Camera::~Camera()
{
	Stop();
	queue.detach(client_index, token);
	free_media(media);
	--object_count;
}

HRESULT Camera::QueryInterface(REFIID iid, void **output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = nullptr;
	if (iid == IID_IUnknown || iid == IID_IBaseFilter || iid == IID_IMediaFilter || iid == IID_IPersist) {
		*output = static_cast<IBaseFilter *>(this);
	} else if (iid == IID_IAMFilterMiscFlags) {
		*output = static_cast<IAMFilterMiscFlags *>(this);
	} else if (iid == IID_IPin) {
		*output = static_cast<IPin *>(this);
	} else if (iid == IID_IAMStreamConfig) {
		*output = static_cast<IAMStreamConfig *>(this);
	} else if (iid == IID_IKsPropertySet) {
		*output = static_cast<IKsPropertySet *>(this);
	} else {
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

ULONG Camera::AddRef()
{
	return ++references;
}

ULONG Camera::Release()
{
	const ULONG remaining = --references;
	if (!remaining) {
		delete this;
	}
	return remaining;
}

HRESULT Camera::GetClassID(CLSID *output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = class_id;
	return S_OK;
}

HRESULT Camera::Stop()
{
	std::lock_guard transition(lifecycle);
	if ((worker.joinable() && worker.get_id() == std::this_thread::get_id()) ||
	    (collector.joinable() && collector.get_id() == std::this_thread::get_id())) {
		return VFW_E_WRONG_STATE;
	}
	ComPtr<IPin> target;
	ComPtr<IMemAllocator> buffers;
	{
		std::lock_guard lock(mutex);
		state = State_Stopped;
		SetEvent(stop_event);
		target = connected;
		buffers = allocator;
	}
	if (target) {
		target->BeginFlush();
	}
	if (buffers) {
		buffers->Decommit();
	}
	if (collector.joinable()) {
		collector.join();
	}
	if (worker.joinable()) {
		worker.join();
	}
	received_frames.reset();
	if (target) {
		target->EndFlush();
	}
	if (client_index >= 0) {
		store(queue.header()->clients[client_index].streaming, 0);
	}
	return S_OK;
}

HRESULT Camera::Pause()
{
	std::lock_guard transition(lifecycle);
	std::lock_guard lock(mutex);
	if (state == State_Stopped && allocator) {
		const HRESULT result = allocator->Commit();
		if (FAILED(result)) {
			return result;
		}
	}
	state = State_Paused;
	return S_OK;
}

HRESULT Camera::Run(REFERENCE_TIME start)
{
	std::lock_guard transition(lifecycle);
	std::lock_guard lock(mutex);
	if (!available() || !load(queue.header()->active) ||
	    !process_alive(queue.header()->owner_pid, queue.header()->owner_start)) {
		return VFW_E_NOT_FOUND;
	}
	if (!connected) {
		return VFW_E_NOT_CONNECTED;
	}
	run_start = start;
	if (!worker.joinable()) {
		const HRESULT result = allocator->Commit();
		if (FAILED(result)) {
			return result;
		}
		ResetEvent(stop_event);
		try {
			received_frames = std::make_unique<FrameCache>(rate, queue.header()->target_frames,
								       queue.header()->slots);
			collector = std::thread([this] {
				try {
					collect();
				} catch (...) {
					SetEvent(stop_event);
				}
			});
			worker = std::thread([this] {
				CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				try {
					deliver();
				} catch (...) {
					SetEvent(stop_event);
				}
				CoUninitialize();
			});
		} catch (...) {
			SetEvent(stop_event);
			if (collector.joinable()) {
				collector.join();
			}
			received_frames.reset();
			allocator->Decommit();
			return E_OUTOFMEMORY;
		}
	}
	state = State_Running;
	return S_OK;
}

HRESULT Camera::GetState(DWORD, FILTER_STATE *output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	*output = state;
	return state == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

HRESULT Camera::SetSyncSource(IReferenceClock *value)
{
	std::lock_guard lock(mutex);
	clock = value;
	return S_OK;
}

HRESULT Camera::GetSyncSource(IReferenceClock **output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	return clock.CopyTo(output);
}

HRESULT Camera::EnumPins(IEnumPins **output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = new (std::nothrow) Enumerator<false>(this);
	return *output ? S_OK : E_OUTOFMEMORY;
}

HRESULT Camera::FindPin(LPCWSTR id, IPin **output)
{
	if (!output || !id) {
		return E_POINTER;
	}
	*output = nullptr;
	if (wcscmp(id, L"Video") != 0) {
		return VFW_E_NOT_FOUND;
	}
	*output = static_cast<IPin *>(this);
	AddRef();
	return S_OK;
}

HRESULT Camera::QueryFilterInfo(FILTER_INFO *output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	wcsncpy_s(output->achName, graph_name.c_str(), _TRUNCATE);
	output->pGraph = graph;
	if (graph) {
		graph->AddRef();
	}
	return S_OK;
}

HRESULT Camera::JoinFilterGraph(IFilterGraph *value, LPCWSTR name)
{
	std::lock_guard lock(mutex);
	graph = value;
	graph_name = name ? name : camera_name;
	return S_OK;
}

HRESULT Camera::QueryVendorInfo(LPWSTR *output)
{
	if (output) {
		*output = nullptr;
	}
	return E_NOTIMPL;
}

ULONG Camera::GetMiscFlags()
{
	return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

HRESULT Camera::ReceiveConnection(IPin *, const AM_MEDIA_TYPE *)
{
	return VFW_E_INVALID_DIRECTION;
}

HRESULT Camera::Connect(IPin *pin, const AM_MEDIA_TYPE *value)
{
	if (!pin) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	if (connected) {
		return VFW_E_ALREADY_CONNECTED;
	}
	if (state != State_Stopped) {
		return VFW_E_NOT_STOPPED;
	}
	PIN_DIRECTION direction;
	if (FAILED(pin->QueryDirection(&direction)) || direction != PINDIR_INPUT) {
		return VFW_E_INVALID_DIRECTION;
	}
	ComPtr<IMemInputPin> input;
	HRESULT result = pin->QueryInterface(IID_PPV_ARGS(&input));
	if (FAILED(result)) {
		return result;
	}
	for (int index = -1; index < 1; ++index) {
		AM_MEDIA_TYPE proposal = {};
		if (value && value->pbFormat) {
			if (index != -1) {
				break;
			}
			result = copy_media(proposal, *value);
		} else if (index == -1) {
			result = copy_media(proposal, media);
		} else {
			result = make_media(index, &proposal);
		}
		if (FAILED(result)) {
			return result;
		}
		PixelFormat proposed_format;
		bool flipped;
		result = validate(&proposal, proposed_format, flipped);
		if (SUCCEEDED(result) && value && value->subtype != GUID_NULL && value->subtype != proposal.subtype) {
			result = VFW_E_TYPE_NOT_ACCEPTED;
		}
		if (SUCCEEDED(result)) {
			result = pin->ReceiveConnection(static_cast<IPin *>(this), &proposal);
		}
		if (FAILED(result)) {
			free_media(proposal);
			continue;
		}
		ComPtr<IMemAllocator> buffers;
		result = input->GetAllocator(&buffers);
		if (FAILED(result)) {
			result = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
						  IID_PPV_ARGS(&buffers));
		}
		const LONG bytes = LONG(image_size(proposed_format, width, height));
		ALLOCATOR_PROPERTIES requested = {}, actual = {};
		input->GetAllocatorRequirements(&requested);
		requested.cBuffers = std::max(3L, requested.cBuffers);
		requested.cbBuffer = std::max(bytes, requested.cbBuffer);
		requested.cbAlign = std::max(1L, requested.cbAlign);
		if (SUCCEEDED(result)) {
			result = buffers->SetProperties(&requested, &actual);
		}
		if (SUCCEEDED(result) && (actual.cbBuffer < bytes || actual.cBuffers < 1)) {
			result = VFW_E_SIZENOTSET;
		}
		if (SUCCEEDED(result)) {
			result = input->NotifyAllocator(buffers.Get(), FALSE);
		}
		if (SUCCEEDED(result)) {
			free_media(media);
			media = proposal;
			format = proposed_format;
			bottom_up = flipped;
			connected = pin;
			receiver = input;
			allocator = buffers;
			return S_OK;
		}
		pin->Disconnect();
		free_media(proposal);
	}
	return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT Camera::Disconnect()
{
	std::lock_guard lock(mutex);
	if (state != State_Stopped) {
		return VFW_E_NOT_STOPPED;
	}
	if (!connected) {
		return S_FALSE;
	}
	allocator.Reset();
	receiver.Reset();
	connected.Reset();
	return S_OK;
}

HRESULT Camera::ConnectedTo(IPin **output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	*output = nullptr;
	return connected ? connected.CopyTo(output) : VFW_E_NOT_CONNECTED;
}

HRESULT Camera::ConnectionMediaType(AM_MEDIA_TYPE *output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	return connected ? copy_media(*output, media) : VFW_E_NOT_CONNECTED;
}

HRESULT Camera::QueryPinInfo(PIN_INFO *output)
{
	if (!output) {
		return E_POINTER;
	}
	output->pFilter = static_cast<IBaseFilter *>(this);
	AddRef();
	output->dir = PINDIR_OUTPUT;
	wcscpy_s(output->achName, L"Video");
	return S_OK;
}

HRESULT Camera::QueryDirection(PIN_DIRECTION *output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = PINDIR_OUTPUT;
	return S_OK;
}

HRESULT Camera::QueryId(LPWSTR *output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"Video")));
	if (!*output) {
		return E_OUTOFMEMORY;
	}
	memcpy(*output, L"Video", sizeof(L"Video"));
	return S_OK;
}

HRESULT Camera::EnumMediaTypes(IEnumMediaTypes **output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = new (std::nothrow) Enumerator<true>(this);
	return *output ? S_OK : E_OUTOFMEMORY;
}

HRESULT Camera::QueryInternalConnections(IPin **, ULONG *)
{
	return E_NOTIMPL;
}
HRESULT Camera::EndOfStream()
{
	return E_UNEXPECTED;
}
HRESULT Camera::BeginFlush()
{
	return E_UNEXPECTED;
}
HRESULT Camera::EndFlush()
{
	return E_UNEXPECTED;
}
HRESULT Camera::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double)
{
	return E_UNEXPECTED;
}

HRESULT Camera::Set(REFGUID property, DWORD id, void *, DWORD, void *, DWORD)
{
	if (property != AMPROPSETID_Pin) {
		return E_PROP_SET_UNSUPPORTED;
	}
	if (id != AMPROPERTY_PIN_CATEGORY) {
		return E_PROP_ID_UNSUPPORTED;
	}
	return E_NOTIMPL;
}

HRESULT Camera::Get(REFGUID property, DWORD id, void *, DWORD, void *data, DWORD size, DWORD *returned)
{
	if (returned) {
		*returned = 0;
	}
	if (property != AMPROPSETID_Pin) {
		return E_PROP_SET_UNSUPPORTED;
	}
	if (id != AMPROPERTY_PIN_CATEGORY) {
		return E_PROP_ID_UNSUPPORTED;
	}
	if (returned) {
		*returned = sizeof(GUID);
	}
	if (!data) {
		return returned ? S_OK : E_POINTER;
	}
	if (size < sizeof(GUID)) {
		return E_UNEXPECTED;
	}
	memcpy(data, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
	return S_OK;
}

HRESULT Camera::QuerySupported(REFGUID property, DWORD id, DWORD *support)
{
	if (!support) {
		return E_POINTER;
	}
	*support = 0;
	if (property != AMPROPSETID_Pin) {
		return E_PROP_SET_UNSUPPORTED;
	}
	if (id != AMPROPERTY_PIN_CATEGORY) {
		return E_PROP_ID_UNSUPPORTED;
	}
	*support = KSPROPERTY_SUPPORT_GET;
	return S_OK;
}

}
