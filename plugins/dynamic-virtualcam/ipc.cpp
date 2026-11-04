#include "ipc.hpp"

#include <algorithm>
#include <cstring>

namespace dvc {

uint64_t now_ns()
{
	LARGE_INTEGER count, frequency;
	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return mul_div(uint64_t(count.QuadPart), 1000000000, uint64_t(frequency.QuadPart));
}

uint64_t process_start(HANDLE process)
{
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
		return 0;
	}
	return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool process_alive(uint32_t pid, uint64_t start)
{
	Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
	return process && process_start(process) == start && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

std::wstring object_name(const std::wstring &session, const wchar_t *suffix)
{
	return L"Local\\OBS-DVC-" + session + L"-" + suffix;
}

Queue::~Queue()
{
	if (shared) {
		UnmapViewOfFile(shared);
	}
}

bool Queue::create(const std::wstring &session, uint32_t width, uint32_t height, uint32_t slots, Rate rate,
		   PixelFormat format, uint32_t target_frames)
{
	if (shared || !rate.valid() || !width || !height || width > 8192 || height > 8192 || (width & 1) ||
	    (height & 1) || !slots || slots > max_slots || !target_frames || target_frames > slots ||
	    !valid_pixel_format(uint32_t(format))) {
		return false;
	}
	session_name = session;
	frame_bytes = image_size(format, width, height);
	const uint64_t bytes = sizeof(Shared) + uint64_t(frame_bytes) * slots;
	if (bytes > std::numeric_limits<size_t>::max()) {
		return false;
	}
	mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(bytes >> 32),
					 DWORD(bytes), object_name(session, L"frames").c_str()));
	if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
		return false;
	}
	shared = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t(bytes)));
	if (!shared) {
		return false;
	}
	memset(shared, 0, sizeof(Shared));
	shared->version = protocol;
	shared->width = width;
	shared->height = height;
	shared->slots = slots;
	shared->target_frames = target_frames;
	shared->fps_num = rate.numerator;
	shared->fps_den = rate.denominator;
	shared->owner_pid = GetCurrentProcessId();
	shared->pixel_format = uint32_t(format);
	shared->owner_start = process_start(GetCurrentProcess());
	store(shared->wake_epoch, now_ns());
	pixels = reinterpret_cast<uint8_t *>(shared + 1);
	control.reset(CreateMutexW(nullptr, FALSE, object_name(session, L"control").c_str()));
	for (uint32_t index = 0; index < slots; ++index) {
		locks[index].reset(CreateMutexW(
			nullptr, FALSE, object_name(session, (L"slot" + std::to_wstring(index)).c_str()).c_str()));
		if (!locks[index]) {
			return false;
		}
	}
	store(shared->active, 1);
	return bool(control);
}

bool Queue::open(const std::wstring &session)
{
	if (shared || session.empty() || session.size() > 40) {
		return false;
	}
	session_name = session;
	mapping.reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, object_name(session, L"frames").c_str()));
	if (!mapping) {
		return false;
	}
	auto *header = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
	if (!header) {
		return false;
	}
	const uint32_t width = header->width, height = header->height, slots = header->slots;
	const Rate rate{header->fps_num, header->fps_den};
	const uint32_t format = header->pixel_format;
	const uint32_t target_frames = header->target_frames;
	const bool valid = header->version == protocol && width && height && width <= 8192 && height <= 8192 &&
			   !(width & 1) && !(height & 1) && slots && slots <= max_slots && target_frames &&
			   target_frames <= slots && rate.valid() && valid_pixel_format(format) &&
			   load(header->active) && process_alive(header->owner_pid, header->owner_start);
	UnmapViewOfFile(header);
	if (!valid) {
		return false;
	}
	frame_bytes = image_size(PixelFormat(format), width, height);
	const uint64_t bytes = sizeof(Shared) + uint64_t(frame_bytes) * slots;
	if (bytes > std::numeric_limits<size_t>::max()) {
		return false;
	}
	shared = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t(bytes)));
	if (!shared || shared->width != width || shared->height != height || shared->slots != slots ||
	    shared->fps_num != rate.numerator || shared->fps_den != rate.denominator ||
	    shared->pixel_format != format || shared->target_frames != target_frames) {
		return false;
	}
	pixels = reinterpret_cast<uint8_t *>(shared + 1);
	control.reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, object_name(session, L"control").c_str()));
	for (uint32_t index = 0; index < slots; ++index) {
		locks[index].reset(
			OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
				   object_name(session, (L"slot" + std::to_wstring(index)).c_str()).c_str()));
		if (!locks[index]) {
			return false;
		}
	}
	return bool(control);
}

bool Queue::lock_control(DWORD timeout) const
{
	const DWORD result = WaitForSingleObject(control, timeout);
	return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

void Queue::unlock_control() const
{
	ReleaseMutex(control);
}

bool Queue::write(const uint8_t *data, size_t stride, uint64_t timestamp)
{
	if (!shared || stride > MAXDWORD) {
		return false;
	}
	const uint8_t *planes[] = {data, shared->pixel_format == uint32_t(PixelFormat::NV12) && data
						 ? data + stride * shared->height
						 : nullptr};
	const uint32_t strides[] = {uint32_t(stride), uint32_t(stride)};
	return write_planes(planes, strides, timestamp);
}

bool Queue::write_planes(const uint8_t *const *data, const uint32_t *strides, uint64_t timestamp)
{
	if (!shared || !load(shared->active) || !data || !strides || !data[0]) {
		return false;
	}
	const PixelFormat format = PixelFormat(shared->pixel_format);
	const size_t stride = image_stride(format, shared->width);
	const size_t row_bytes = format == PixelFormat::RGB24 ? size_t(shared->width) * 3 : stride;
	if (strides[0] < row_bytes || (format == PixelFormat::NV12 && (!data[1] || strides[1] < shared->width))) {
		return false;
	}
	const uint64_t sequence = next_sequence + 1;
	const uint32_t slot = uint32_t((sequence - 1) % shared->slots);
	const DWORD result = WaitForSingleObject(locks[slot], 0);
	if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
		InterlockedIncrement64(&shared->dropped);
		return false;
	}
	store(shared->frames[slot].sequence, 0);
	auto *output = pixels + slot * frame_bytes;
	for (uint32_t row = 0; row < shared->height; ++row) {
		memcpy(output + row * stride, data[0] + size_t(row) * strides[0], row_bytes);
		if (row_bytes < stride) {
			memset(output + row * stride + row_bytes, 0, stride - row_bytes);
		}
	}
	if (format == PixelFormat::NV12) {
		output += size_t(shared->width) * shared->height;
		for (uint32_t row = 0; row < shared->height / 2; ++row) {
			memcpy(output + size_t(row) * shared->width, data[1] + size_t(row) * strides[1], shared->width);
		}
	}
	store(shared->frames[slot].timestamp, timestamp);
	uint64_t generation = load(shared->generation);
	const bool discontinuity = !next_sequence || timestamp < last_timestamp ||
				   timestamp - last_timestamp > 2000000000ULL;
	if (discontinuity) {
		++generation;
	}
	store(shared->frames[slot].generation, generation);
	store(shared->frames[slot].sequence, sequence);
	ReleaseMutex(locks[slot]);
	if (discontinuity) {
		store(shared->generation_start, sequence);
		store(shared->generation, generation);
	}
	next_sequence = sequence;
	last_timestamp = timestamp;
	store(shared->published, sequence);
	return true;
}

size_t Queue::snapshot(std::array<FrameStamp, max_slots> &frames, uint64_t latest, uint64_t generation) const
{
	size_t count = 0;
	for (uint32_t slot = 0; slot < shared->slots; ++slot) {
		const auto &record = shared->frames[slot];
		const FrameStamp frame{load(record.sequence), load(record.timestamp), load(record.generation)};
		if (frame.sequence && frame.sequence <= latest && frame.generation == generation &&
		    frame.sequence == load(record.sequence)) {
			frames[count++] = frame;
		}
	}
	std::sort(frames.begin(), frames.begin() + count,
		  [](const FrameStamp &first, const FrameStamp &second) { return first.sequence < second.sequence; });
	return count;
}

bool Queue::copy_frame(const FrameStamp &frame, std::vector<uint8_t> &image)
{
	const uint32_t slot = uint32_t((frame.sequence - 1) % shared->slots);
	const DWORD result = WaitForSingleObject(locks[slot], 0);
	if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
		return false;
	}
	const auto &record = shared->frames[slot];
	if (load(record.sequence) != frame.sequence || load(record.timestamp) != frame.timestamp ||
	    load(record.generation) != frame.generation) {
		ReleaseMutex(locks[slot]);
		return false;
	}
	memcpy(image.data(), pixels + slot * frame_bytes, frame_bytes);
	ReleaseMutex(locks[slot]);
	return true;
}

bool Queue::select(FrameBuffer &buffer, uint64_t timestamp, std::vector<uint8_t> &image)
{
	if (!shared || !load(shared->active)) {
		return false;
	}
	const uint64_t generation = load(shared->generation);
	const uint64_t start = load(shared->generation_start);
	const uint64_t latest = load(shared->published);
	std::array<FrameStamp, max_slots> frames;
	const size_t count = snapshot(frames, latest, generation);
	if (generation != load(shared->generation) || start != load(shared->generation_start)) {
		return false;
	}
	const FrameStamp selected = buffer.select(frames.data(), count, latest, generation, start, timestamp,
						  {shared->fps_num, shared->fps_den}, shared->target_frames,
						  shared->slots);
	if (!selected.sequence) {
		return false;
	}
	image.resize(frame_bytes);
	if (!copy_frame(selected, image) || selected.generation != load(shared->generation)) {
		return false;
	}
	buffer.consume(selected.sequence, latest);
	return true;
}

bool Queue::read(uint64_t &consumed, uint64_t now, std::vector<uint8_t> &image, uint64_t &skipped, uint64_t *timestamp)
{
	if (!shared || !load(shared->active)) {
		return false;
	}
	const Rate rate{shared->fps_num, shared->fps_den};
	image.resize(frame_bytes);
	std::array<FrameStamp, max_slots> frames;
	const size_t count = snapshot(frames, load(shared->published), load(shared->generation));
	for (size_t index = 0; index < count; ++index) {
		const auto &frame = frames[index];
		if (!eligible(frame.sequence, consumed, frame.timestamp, now, rate) || !copy_frame(frame, image)) {
			continue;
		}
		if (consumed && frame.sequence > consumed + 1) {
			skipped += frame.sequence - consumed - 1;
		}
		consumed = frame.sequence;
		if (timestamp) {
			*timestamp = frame.timestamp;
		}
		return true;
	}
	return false;
}

int Queue::attach(uint64_t token)
{
	if (!shared || !lock_control()) {
		return -1;
	}
	int selected = -1;
	for (size_t index = 0; index < max_clients; ++index) {
		auto &client = shared->clients[index];
		if (client.pid && process_alive(client.pid, client.process_start)) {
			continue;
		}
		memset(&client, 0, sizeof(client));
		client.token = token;
		client.pid = GetCurrentProcessId();
		client.process_start = process_start(GetCurrentProcess());
		store(client.heartbeat, now_ns());
		selected = int(index);
		break;
	}
	unlock_control();
	return selected;
}

void Queue::detach(int index, uint64_t token)
{
	if (index < 0 || index >= int(max_clients) || !shared || !lock_control()) {
		return;
	}
	if (shared->clients[index].token == token) {
		memset(&shared->clients[index], 0, sizeof(Client));
	}
	unlock_control();
}

}
