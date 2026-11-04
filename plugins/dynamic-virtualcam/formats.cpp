#include "filter.hpp"
#include "media-format.hpp"

#include <algorithm>
#include <cstring>

namespace dvc {

HRESULT Camera::make_media(int index, AM_MEDIA_TYPE *output) const
{
	if (index != 0) {
		return E_INVALIDARG;
	}
	*output = {};
	output->majortype = MEDIATYPE_Video;
	output->subtype = media_subtype(format);
	output->bFixedSizeSamples = TRUE;
	output->lSampleSize = ULONG(image_size(format, width, height));
	output->formattype = FORMAT_VideoInfo;
	output->cbFormat = sizeof(VIDEOINFOHEADER);
	output->pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(output->cbFormat));
	if (!output->pbFormat) {
		return E_OUTOFMEMORY;
	}
	auto *info = reinterpret_cast<VIDEOINFOHEADER *>(output->pbFormat);
	*info = {};
	info->AvgTimePerFrame = REFERENCE_TIME(rate.offset(1) / 100);
	info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info->bmiHeader.biWidth = LONG(width);
	info->bmiHeader.biHeight = LONG(height);
	info->bmiHeader.biPlanes = 1;
	info->bmiHeader.biSizeImage = output->lSampleSize;
	info->bmiHeader.biBitCount = media_bits(format);
	info->bmiHeader.biCompression = media_compression(format);
	info->dwBitRate = DWORD(std::min<uint64_t>(MAXDWORD, mul_div(uint64_t(output->lSampleSize) * 8, rate.numerator,
								     rate.denominator)));
	return S_OK;
}

HRESULT Camera::media_at(int index, AM_MEDIA_TYPE *output) const
{
	return make_media(index, output);
}

HRESULT Camera::validate(const AM_MEDIA_TYPE *value, PixelFormat &pixel_format, bool &flipped) const
{
	if (!value || value->majortype != MEDIATYPE_Video || value->formattype != FORMAT_VideoInfo ||
	    !value->pbFormat || value->cbFormat < sizeof(VIDEOINFOHEADER) || value->bTemporalCompression) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	const auto *info = reinterpret_cast<const VIDEOINFOHEADER *>(value->pbFormat);
	const auto &bitmap = info->bmiHeader;
	if (bitmap.biSize != sizeof(BITMAPINFOHEADER) || bitmap.biWidth != LONG(width) ||
	    bitmap.biHeight != LONG(height) || bitmap.biPlanes != 1 || info->AvgTimePerFrame <= 0) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	const uint64_t interval = rate.offset(1) / 100;
	const uint64_t requested = uint64_t(info->AvgTimePerFrame);
	if (requested > interval + 1 || requested + 1 < interval) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	if (value->subtype != media_subtype(format) || bitmap.biCompression != media_compression(format) ||
	    bitmap.biBitCount != media_bits(format)) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}
	pixel_format = format;
	flipped = format != PixelFormat::NV12 && format != PixelFormat::YUY2 && bitmap.biHeight > 0;
	return S_OK;
}

HRESULT Camera::QueryAccept(const AM_MEDIA_TYPE *value)
{
	PixelFormat selected;
	bool flipped;
	return SUCCEEDED(validate(value, selected, flipped)) ? S_OK : S_FALSE;
}

HRESULT Camera::SetFormat(AM_MEDIA_TYPE *value)
{
	std::lock_guard lock(mutex);
	if (state != State_Stopped || connected) {
		return VFW_E_NOT_STOPPED;
	}
	PixelFormat selected;
	bool flipped;
	const HRESULT result = validate(value, selected, flipped);
	if (FAILED(result)) {
		return result;
	}
	AM_MEDIA_TYPE copied = {};
	const HRESULT copied_result = copy_media(copied, *value);
	if (FAILED(copied_result)) {
		return copied_result;
	}
	free_media(media);
	media = copied;
	format = selected;
	bottom_up = flipped;
	return S_OK;
}

HRESULT Camera::GetFormat(AM_MEDIA_TYPE **output)
{
	if (!output) {
		return E_POINTER;
	}
	std::lock_guard lock(mutex);
	*output = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
	if (!*output) {
		return E_OUTOFMEMORY;
	}
	const HRESULT result = copy_media(**output, media);
	if (FAILED(result)) {
		CoTaskMemFree(*output);
		*output = nullptr;
	}
	return result;
}

HRESULT Camera::GetNumberOfCapabilities(int *count, int *size)
{
	if (!count || !size) {
		return E_POINTER;
	}
	*count = 1;
	*size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
	return S_OK;
}

HRESULT Camera::GetStreamCaps(int index, AM_MEDIA_TYPE **output, BYTE *caps)
{
	if (!output || !caps) {
		return E_POINTER;
	}
	*output = nullptr;
	if (index != 0) {
		return S_FALSE;
	}
	*output = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
	if (!*output) {
		return E_OUTOFMEMORY;
	}
	const HRESULT result = make_media(index, *output);
	if (FAILED(result)) {
		CoTaskMemFree(*output);
		*output = nullptr;
		return result;
	}
	VIDEO_STREAM_CONFIG_CAPS capability = {};
	capability.guid = FORMAT_VideoInfo;
	capability.InputSize = {LONG(width), LONG(height)};
	capability.MinCroppingSize = capability.MaxCroppingSize = capability.InputSize;
	capability.MinOutputSize = capability.MaxOutputSize = capability.InputSize;
	capability.CropGranularityX = capability.CropGranularityY = 1;
	capability.OutputGranularityX = capability.OutputGranularityY = 1;
	capability.MinFrameInterval = capability.MaxFrameInterval = REFERENCE_TIME(rate.offset(1) / 100);
	capability.MinBitsPerSecond = capability.MaxBitsPerSecond = LONG(std::min<uint64_t>(
		MAXLONG, mul_div(uint64_t((*output)->lSampleSize) * 8, rate.numerator, rate.denominator)));
	memcpy(caps, &capability, sizeof(capability));
	return S_OK;
}

}
