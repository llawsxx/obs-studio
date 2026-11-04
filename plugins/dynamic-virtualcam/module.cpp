#include "filter.hpp"

#include <new>

namespace dvc {
std::atomic<long> object_count{0};
static std::atomic<long> server_locks{0};

class Factory final : public IClassFactory {
	std::atomic<ULONG> references{1};
	CLSID class_id;
	std::wstring device_id;

public:
	explicit Factory(REFCLSID id) : class_id(id)
	{
		wchar_t text[40];
		StringFromGUID2(id, text, 40);
		device_id = text;
		++object_count;
	}
	~Factory() { --object_count; }
	STDMETHODIMP QueryInterface(REFIID iid, void **output) override
	{
		if (!output) {
			return E_POINTER;
		}
		*output = nullptr;
		if (iid != IID_IUnknown && iid != IID_IClassFactory) {
			return E_NOINTERFACE;
		}
		*output = static_cast<IClassFactory *>(this);
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
	STDMETHODIMP CreateInstance(IUnknown *outer, REFIID iid, void **output) override
	{
		if (!output) {
			return E_POINTER;
		}
		*output = nullptr;
		if (outer) {
			return CLASS_E_NOAGGREGATION;
		}
		try {
			auto *camera = new Camera(active_session(device_id), class_id);
			const HRESULT result = camera->available() ? camera->QueryInterface(iid, output)
								   : VFW_E_NOT_FOUND;
			camera->Release();
			return result;
		} catch (...) {
			return E_OUTOFMEMORY;
		}
	}
	STDMETHODIMP LockServer(BOOL lock) override
	{
		if (lock) {
			++server_locks;
		} else {
			--server_locks;
		}
		return S_OK;
	}
};
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID iid, void **output)
{
	if (!output) {
		return E_POINTER;
	}
	*output = nullptr;
	try {
		wchar_t device_id[40];
		if (!StringFromGUID2(clsid, device_id, 40) || dvc::active_session(device_id).empty()) {
			return CLASS_E_CLASSNOTAVAILABLE;
		}
		auto *factory = new (std::nothrow) dvc::Factory(clsid);
		if (!factory) {
			return E_OUTOFMEMORY;
		}
		const HRESULT result = factory->QueryInterface(iid, output);
		factory->Release();
		return result;
	} catch (...) {
		return E_OUTOFMEMORY;
	}
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
	return dvc::object_count == 0 && dvc::server_locks == 0 ? S_OK : S_FALSE;
}
