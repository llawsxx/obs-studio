#pragma once

#include "pixels.hpp"
#include <dshow.h>

namespace dvc {

inline const GUID &media_subtype(PixelFormat format)
{
	switch (format) {
	case PixelFormat::NV12:
		return MEDIASUBTYPE_NV12;
	case PixelFormat::YUY2:
		return MEDIASUBTYPE_YUY2;
	case PixelFormat::RGB24:
		return MEDIASUBTYPE_RGB24;
	case PixelFormat::RGB32:
		return MEDIASUBTYPE_RGB32;
	case PixelFormat::ARGB32:
		return MEDIASUBTYPE_ARGB32;
	}
	return GUID_NULL;
}

inline WORD media_bits(PixelFormat format)
{
	return format == PixelFormat::NV12    ? 12
	       : format == PixelFormat::YUY2  ? 16
	       : format == PixelFormat::RGB24 ? 24
					      : 32;
}

inline DWORD media_compression(PixelFormat format)
{
	return format == PixelFormat::NV12   ? MAKEFOURCC('N', 'V', '1', '2')
	       : format == PixelFormat::YUY2 ? MAKEFOURCC('Y', 'U', 'Y', '2')
					     : BI_RGB;
}

}
