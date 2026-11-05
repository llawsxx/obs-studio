#pragma once

#include "ipc.hpp"
#include "pixels.hpp"
#include "frame-cache.hpp"
#include <memory>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace dvc {

using Microsoft::WRL::ComPtr;
extern std::atomic<long> object_count;

void free_media(AM_MEDIA_TYPE &media);
HRESULT copy_media(AM_MEDIA_TYPE &destination, const AM_MEDIA_TYPE &source);

class Camera final : public IBaseFilter,
		     public IAMFilterMiscFlags,
		     public IPin,
		     public IAMStreamConfig,
		     public IKsPropertySet {
	std::atomic<ULONG> references{1};
	std::recursive_mutex mutex;
	std::mutex lifecycle;
	FILTER_STATE state = State_Stopped;
	IFilterGraph *graph = nullptr;
	std::wstring graph_name = camera_name;
	ComPtr<IReferenceClock> clock;
	ComPtr<IPin> connected;
	ComPtr<IMemInputPin> receiver;
	ComPtr<IMemAllocator> allocator;
	AM_MEDIA_TYPE media = {};
	Queue queue;
	int client_index = -1;
	uint64_t token = 0;
	Handle stop_event;
	std::thread worker;
	std::thread collector;
	std::unique_ptr<FrameCache> received_frames;
	std::atomic<REFERENCE_TIME> run_start{0};
	uint32_t width = 1920, height = 1080;
	Rate rate;
	CLSID class_id = camera_clsid;
	PixelFormat format = PixelFormat::NV12;
	bool bottom_up = false;

	HRESULT make_media(int index, AM_MEDIA_TYPE *output) const;
	HRESULT validate(const AM_MEDIA_TYPE *value, PixelFormat &pixel_format, bool &flipped) const;
	void deliver();
	void collect();
	void update_cache_statistics();

public:
	explicit Camera(const std::wstring &session = active_session(), REFCLSID id = camera_clsid);
	~Camera();
	bool available() const { return client_index >= 0; }
	HRESULT media_at(int index, AM_MEDIA_TYPE *output) const;
	STDMETHODIMP QueryInterface(REFIID iid, void **output) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	STDMETHODIMP GetClassID(CLSID *output) override;
	STDMETHODIMP Stop() override;
	STDMETHODIMP Pause() override;
	STDMETHODIMP Run(REFERENCE_TIME start) override;
	STDMETHODIMP GetState(DWORD, FILTER_STATE *output) override;
	STDMETHODIMP SetSyncSource(IReferenceClock *value) override;
	STDMETHODIMP GetSyncSource(IReferenceClock **output) override;
	STDMETHODIMP EnumPins(IEnumPins **output) override;
	STDMETHODIMP FindPin(LPCWSTR id, IPin **output) override;
	STDMETHODIMP QueryFilterInfo(FILTER_INFO *output) override;
	STDMETHODIMP JoinFilterGraph(IFilterGraph *value, LPCWSTR name) override;
	STDMETHODIMP QueryVendorInfo(LPWSTR *output) override;
	STDMETHODIMP_(ULONG) GetMiscFlags() override;
	STDMETHODIMP Connect(IPin *pin, const AM_MEDIA_TYPE *value) override;
	STDMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override;
	STDMETHODIMP Disconnect() override;
	STDMETHODIMP ConnectedTo(IPin **output) override;
	STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *output) override;
	STDMETHODIMP QueryPinInfo(PIN_INFO *output) override;
	STDMETHODIMP QueryDirection(PIN_DIRECTION *output) override;
	STDMETHODIMP QueryId(LPWSTR *output) override;
	STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *value) override;
	STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **output) override;
	STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override;
	STDMETHODIMP EndOfStream() override;
	STDMETHODIMP BeginFlush() override;
	STDMETHODIMP EndFlush() override;
	STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override;
	STDMETHODIMP SetFormat(AM_MEDIA_TYPE *value) override;
	STDMETHODIMP GetFormat(AM_MEDIA_TYPE **output) override;
	STDMETHODIMP GetNumberOfCapabilities(int *count, int *size) override;
	STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE **output, BYTE *caps) override;
	STDMETHODIMP Set(REFGUID, DWORD, void *, DWORD, void *, DWORD) override;
	STDMETHODIMP Get(REFGUID property, DWORD id, void *, DWORD, void *data, DWORD size, DWORD *returned) override;
	STDMETHODIMP QuerySupported(REFGUID property, DWORD id, DWORD *support) override;
};

}
