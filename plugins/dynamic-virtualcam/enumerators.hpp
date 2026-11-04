#pragma once

#include "filter.hpp"
#include <new>
#include <type_traits>

namespace dvc {

template<bool Media> class Enumerator final : public std::conditional_t<Media, IEnumMediaTypes, IEnumPins> {
	using Interface = std::conditional_t<Media, IEnumMediaTypes, IEnumPins>;
	using Value = std::conditional_t<Media, AM_MEDIA_TYPE *, IPin *>;
	std::atomic<ULONG> references{1};
	ComPtr<Camera> camera;
	ULONG position;

public:
	Enumerator(Camera *value, ULONG start = 0) : camera(value), position(start) { ++object_count; }
	~Enumerator() { --object_count; }
	STDMETHODIMP QueryInterface(REFIID iid, void **output) override
	{
		if (!output) {
			return E_POINTER;
		}
		*output = nullptr;
		if (iid != IID_IUnknown && iid != __uuidof(Interface)) {
			return E_NOINTERFACE;
		}
		*output = static_cast<Interface *>(this);
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
	STDMETHODIMP Next(ULONG count, Value *output, ULONG *fetched) override
	{
		if (!output || (count != 1 && !fetched)) {
			return E_POINTER;
		}
		ULONG found = 0;
		while (found < count && position < (Media ? 5U : 1U)) {
			if constexpr (Media) {
				output[found] = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
				if (!output[found]) {
					return E_OUTOFMEMORY;
				}
				const HRESULT result = camera->media_at(int(position), output[found]);
				if (FAILED(result)) {
					CoTaskMemFree(output[found]);
					output[found] = nullptr;
					return result;
				}
			} else {
				output[found] = static_cast<IPin *>(camera.Get());
				camera->AddRef();
			}
			++found;
			++position;
		}
		if (fetched) {
			*fetched = found;
		}
		return found == count ? S_OK : S_FALSE;
	}
	STDMETHODIMP Skip(ULONG count) override
	{
		const ULONG available = (Media ? 5U : 1U) - position;
		position += std::min(count, available);
		return count <= available ? S_OK : S_FALSE;
	}
	STDMETHODIMP Reset() override
	{
		position = 0;
		return S_OK;
	}
	STDMETHODIMP Clone(Interface **output) override
	{
		if (!output) {
			return E_POINTER;
		}
		*output = new (std::nothrow) Enumerator(camera.Get(), position);
		return *output ? S_OK : E_OUTOFMEMORY;
	}
};

}
