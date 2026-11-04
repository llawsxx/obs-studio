#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <string>
#include <vector>
#include "timing.hpp"
#include "frame-buffer.hpp"
#include "pixels.hpp"

namespace dvc {

inline constexpr wchar_t camera_id[] = L"{D02ABF12-4D12-4876-8B4C-B9DE7C623FF1}";
inline constexpr CLSID camera_clsid = {0xd02abf12, 0x4d12, 0x4876, {0x8b, 0x4c, 0xb9, 0xde, 0x7c, 0x62, 0x3f, 0xf1}};
inline constexpr wchar_t camera_name[] = L"OBS Dynamic Virtual Camera";
inline constexpr wchar_t registry_root[] = L"Software\\Classes\\CLSID\\{D02ABF12-4D12-4876-8B4C-B9DE7C623FF1}";
inline constexpr uint32_t protocol = 1;
inline constexpr size_t max_clients = 32;
inline constexpr size_t max_slots = 30;

class Handle {
	HANDLE value = nullptr;

public:
	Handle() = default;
	explicit Handle(HANDLE handle) : value(handle) {}
	~Handle() { reset(); }
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	void reset(HANDLE handle = nullptr)
	{
		if (value && value != INVALID_HANDLE_VALUE) {
			CloseHandle(value);
		}
		value = handle;
	}
	operator HANDLE() const { return value; }
	explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct alignas(8) Slot {
	alignas(8) LONG64 sequence;
	alignas(8) LONG64 timestamp;
	alignas(8) LONG64 generation;
};

struct alignas(8) Client {
	uint32_t pid;
	uint32_t reserved;
	uint64_t process_start;
	uint64_t token;
	alignas(8) LONG64 delivered;
	alignas(8) LONG64 repeated;
	alignas(8) LONG64 skipped;
	alignas(8) LONG64 late_ns;
	alignas(8) LONG64 heartbeat;
	alignas(8) LONG64 streaming;
	alignas(8) LONG64 expired;
	alignas(8) LONG64 prepare_ns;
	alignas(8) LONG64 receive_ns;
	alignas(8) LONG64 last_result;
	alignas(8) LONG64 pixel_format;
	alignas(8) LONG64 buffered;
	alignas(8) LONG64 rebuffers;
};

struct alignas(8) Shared {
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t slots;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t owner_pid;
	uint32_t pixel_format;
	uint64_t owner_start;
	uint32_t target_frames;
	uint32_t reserved;
	alignas(8) LONG64 active;
	alignas(8) LONG64 wake_epoch;
	alignas(8) LONG64 generation;
	alignas(8) LONG64 generation_start;
	alignas(8) LONG64 published;
	alignas(8) LONG64 dropped;
	alignas(8) LONG64 phase;
	alignas(8) LONG64 delivered;
	alignas(8) LONG64 repeated;
	Slot frames[max_slots];
	Client clients[max_clients];
};

static_assert(sizeof(Slot) == 24);
static_assert(sizeof(Client) == 128);
static_assert(sizeof(Shared) == 120 + sizeof(Slot) * max_slots + sizeof(Client) * max_clients);

inline uint64_t load(const LONG64 &value)
{
	return uint64_t(InterlockedCompareExchange64(const_cast<LONG64 *>(&value), 0, 0));
}

inline void store(LONG64 &value, uint64_t number)
{
	InterlockedExchange64(&value, LONG64(number));
}

uint64_t now_ns();
uint64_t process_start(HANDLE process);
bool process_alive(uint32_t pid, uint64_t start);
std::wstring object_name(const std::wstring &session, const wchar_t *suffix);
enum class RegistrationScope { User, Machine };

std::wstring active_session(const std::wstring &device_id = camera_id);
bool register_device(const std::wstring &session, const std::wstring &dll64, const std::wstring &dll32,
		     const std::wstring &device_id = camera_id, const std::wstring &name = camera_name,
		     RegistrationScope scope = RegistrationScope::User);
void unregister_device(const std::wstring &session, const std::wstring &device_id = camera_id,
		       RegistrationScope scope = RegistrationScope::User);

class Queue {
	Handle mapping;
	Handle control;
	std::array<Handle, max_slots> locks;
	Shared *shared = nullptr;
	uint8_t *pixels = nullptr;
	size_t frame_bytes = 0;
	uint64_t next_sequence = 0;
	uint64_t last_timestamp = 0;
	std::wstring session_name;
	size_t snapshot(std::array<FrameStamp, max_slots> &frames, uint64_t latest, uint64_t generation) const;
	bool copy_frame(const FrameStamp &frame, std::vector<uint8_t> &image);

public:
	~Queue();
	Queue() = default;
	Queue(const Queue &) = delete;
	Queue &operator=(const Queue &) = delete;
	bool create(const std::wstring &session, uint32_t width, uint32_t height, uint32_t slots, Rate rate,
		    PixelFormat format = PixelFormat::ARGB32, uint32_t target_frames = 2);
	bool open(const std::wstring &session);
	Shared *header() const { return shared; }
	bool lock_control(DWORD timeout = 100) const;
	void unlock_control() const;
	bool write(const uint8_t *data, size_t stride, uint64_t timestamp);
	bool write_planes(const uint8_t *const *data, const uint32_t *strides, uint64_t timestamp);
	bool read(uint64_t &consumed, uint64_t now, std::vector<uint8_t> &image, uint64_t &skipped,
		  uint64_t *timestamp = nullptr);
	bool select(FrameBuffer &buffer, uint64_t timestamp, std::vector<uint8_t> &image);
	int attach(uint64_t token);
	void detach(int index, uint64_t token);
};

}
