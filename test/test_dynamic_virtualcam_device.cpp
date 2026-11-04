#include "ipc.hpp"
#include "media-format.hpp"
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <shellapi.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s (Win32=%lu)\n", __FILE__, __LINE__, #condition, GetLastError()); abort(); } } while (0)

using Microsoft::WRL::ComPtr;

static std::wstring unique_session()
{
	GUID id;
	CHECK(SUCCEEDED(CoCreateGuid(&id)));
	wchar_t value[40];
	CHECK(StringFromGUID2(id, value, 40));
	return value;
}

static std::wstring quote(const std::wstring &value)
{
	return L"\"" + value + L"\"";
}

static void launch(const std::wstring &executable, const std::wstring &arguments, dvc::Handle &process)
{
	std::wstring command = quote(executable) + L" " + arguments;
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION information = {};
	CHECK(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
			     nullptr, &startup, &information));
	CloseHandle(information.hThread);
	process.reset(information.hProcess);
}

static void wait_success(HANDLE process)
{
	CHECK(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
	DWORD code;
	CHECK(GetExitCodeProcess(process, &code) && code == 0);
}

static ComPtr<IMoniker> find_device(const std::wstring &device_id = dvc::camera_id)
{
	ComPtr<ICreateDevEnum> devices;
	CHECK(SUCCEEDED(
		CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices))));
	ComPtr<IEnumMoniker> enumerator;
	if (devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumerator, 0) != S_OK) {
		return {};
	}
	ComPtr<IMoniker> moniker;
	while (enumerator->Next(1, &moniker, nullptr) == S_OK) {
		ComPtr<IPropertyBag> properties;
		if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties)))) {
			VARIANT value;
			VariantInit(&value);
			const HRESULT result = properties->Read(L"CLSID", &value, nullptr);
			const bool match = SUCCEEDED(result) && value.vt == VT_BSTR &&
					   _wcsicmp(value.bstrVal, device_id.c_str()) == 0;
			VariantClear(&value);
			if (match) {
				return moniker;
			}
		}
		moniker.Reset();
	}
	return {};
}

static bool find_matching_device(const std::wstring &device_id, REFGUID subtype, const GUID *category)
{
	ComPtr<IFilterMapper2> mapper;
	CHECK(SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper))));
	const GUID output_types[] = {MEDIATYPE_Video, subtype};
	ComPtr<IEnumMoniker> enumerator;
	const HRESULT result = mapper->EnumMatchingFilters(&enumerator, 0, TRUE, MERIT_DO_NOT_USE, FALSE, 0, nullptr,
							   nullptr, nullptr, FALSE, TRUE, 1, output_types, nullptr,
							   category);
	CHECK(SUCCEEDED(result));
	if (!enumerator) {
		return false;
	}
	ComPtr<IMoniker> moniker;
	while (enumerator->Next(1, &moniker, nullptr) == S_OK) {
		ComPtr<IPropertyBag> properties;
		if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties)))) {
			VARIANT value;
			VariantInit(&value);
			const HRESULT read = properties->Read(L"CLSID", &value, nullptr);
			const bool match = SUCCEEDED(read) && value.vt == VT_BSTR &&
					   _wcsicmp(value.bstrVal, device_id.c_str()) == 0;
			VariantClear(&value);
			if (match) {
				return true;
			}
		}
		moniker.Reset();
	}
	return false;
}

static void probe(const std::wstring &session, const std::wstring &device_id = dvc::camera_id)
{
	dvc::Queue queue;
	CHECK(queue.open(session));
	auto moniker = find_device(device_id);
	CHECK(moniker);
	for (const GUID &subtype :
	     {MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2, MEDIASUBTYPE_RGB24, MEDIASUBTYPE_RGB32, MEDIASUBTYPE_ARGB32}) {
		const bool supported = subtype == dvc::media_subtype(dvc::PixelFormat(queue.header()->pixel_format));
		CHECK(find_matching_device(device_id, subtype, nullptr) == supported);
		CHECK(find_matching_device(device_id, subtype, &PIN_CATEGORY_CAPTURE) == supported);
	}
	CHECK(!find_matching_device(device_id, MEDIASUBTYPE_MJPG, nullptr));
	ComPtr<IPropertyBag> properties_bag;
	CHECK(SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties_bag))));
	VARIANT metadata;
	VariantInit(&metadata);
	CHECK(SUCCEEDED(properties_bag->Read(L"FilterData", &metadata, nullptr)));
	CHECK(metadata.vt == (VT_ARRAY | VT_UI1));
	VariantClear(&metadata);
	ComPtr<IBaseFilter> camera;
	HRESULT result = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&camera));
	if (FAILED(result)) {
		fprintf(stderr, "BindToObject failed: %08lx\n", result);
	}
	CHECK(SUCCEEDED(result));
	CLSID id, expected_id;
	CHECK(SUCCEEDED(CLSIDFromString(device_id.c_str(), &expected_id)));
	CHECK(SUCCEEDED(camera->GetClassID(&id)) && id == expected_id);
	ComPtr<IEnumPins> pins;
	CHECK(SUCCEEDED(camera->EnumPins(&pins)));
	ComPtr<IPin> pin;
	CHECK(pins->Next(1, &pin, nullptr) == S_OK);
	ComPtr<IKsPropertySet> properties;
	CHECK(SUCCEEDED(pin.As(&properties)));
	GUID category = GUID_NULL;
	DWORD returned = 0, support = 0;
	CHECK(properties->QuerySupported(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, &support) == S_OK);
	CHECK(support == KSPROPERTY_SUPPORT_GET);
	CHECK(properties->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category),
			      &returned) == S_OK);
	CHECK(category == PIN_CATEGORY_CAPTURE && returned == sizeof(GUID));
	ComPtr<IAMStreamConfig> config;
	CHECK(SUCCEEDED(pin.As(&config)));
	int count, size;
	CHECK(SUCCEEDED(config->GetNumberOfCapabilities(&count, &size)) && count == 1);
	ComPtr<IGraphBuilder> graph;
	CHECK(SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))));
	const CLSID null_renderer = {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0xb, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
	ComPtr<IBaseFilter> sink;
	CHECK(SUCCEEDED(CoCreateInstance(null_renderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sink))));
	CHECK(SUCCEEDED(graph->AddFilter(camera.Get(), L"DVC camera")));
	CHECK(SUCCEEDED(graph->AddFilter(sink.Get(), L"DVC null renderer")));
	ComPtr<IEnumPins> input_pins;
	CHECK(SUCCEEDED(sink->EnumPins(&input_pins)));
	ComPtr<IPin> input;
	CHECK(input_pins->Next(1, &input, nullptr) == S_OK);
	CHECK(SUCCEEDED(graph->ConnectDirect(pin.Get(), input.Get(), nullptr)));
	ComPtr<IMediaControl> control;
	CHECK(SUCCEEDED(graph.As(&control)));
	CHECK(SUCCEEDED(control->Run()));
	Sleep(300);
	bool delivered = false;
	CHECK(queue.lock_control());
	for (const auto &client : queue.header()->clients) {
		if (client.pid == GetCurrentProcessId() && dvc::load(client.delivered) >= 3) {
			delivered = true;
		}
	}
	queue.unlock_control();
	CHECK(delivered);
	CHECK(SUCCEEDED(control->Stop()));
	CHECK(SUCCEEDED(graph->Disconnect(pin.Get())));
	CHECK(SUCCEEDED(graph->Disconnect(input.Get())));
	CHECK(SUCCEEDED(graph->RemoveFilter(camera.Get())));
	CHECK(SUCCEEDED(graph->RemoveFilter(sink.Get())));
	printf("%zu-bit device enumeration, COM activation and graph delivery passed\n", sizeof(void *) * 8);
}

static void start_watchdog(const std::wstring &executable, const std::wstring &session, dvc::Handle &stop,
			   dvc::Handle &process, const std::wstring &device_id = dvc::camera_id)
{
	stop.reset(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(session, L"watchdog").c_str()));
	CHECK(stop);
	launch(executable,
	       quote(session) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
		       std::to_wstring(dvc::process_start(GetCurrentProcess())) + L" " + quote(device_id),
	       process);
	CHECK(WaitForSingleObject(process, 20) == WAIT_TIMEOUT);
}

static void test_multiple_devices(const std::wstring &dll64, const std::wstring &dll32, const std::wstring &helper,
				  const std::wstring &probe32)
{
	const std::wstring first_id = unique_session(), second_id = unique_session();
	const std::wstring first_session = unique_session(), second_session = unique_session();
	dvc::Queue first, second;
	CHECK(first.create(first_session, 2, 2, 3, {60, 1}, dvc::PixelFormat::NV12));
	CHECK(second.create(second_session, 2, 2, 5, {60, 1}, dvc::PixelFormat::YUY2));
	dvc::store(first.header()->phase, 3000000);
	dvc::store(second.header()->phase, 6000000);
	dvc::Handle first_stop, first_watchdog, second_stop, second_watchdog;
	start_watchdog(helper, first_session, first_stop, first_watchdog, first_id);
	start_watchdog(helper, second_session, second_stop, second_watchdog, second_id);
	CHECK(dvc::register_device(first_session, dll64, dll32, first_id, L"OBS DVC test A"));
	CHECK(dvc::register_device(second_session, dll64, dll32, second_id, L"OBS DVC test B"));
	CHECK(find_device(first_id) && find_device(second_id));
	CHECK(dvc::active_session(first_id) == first_session);
	CHECK(dvc::active_session(second_id) == second_session);
	CHECK(!dvc::register_device(first_session, dll64, dll32, second_id, L"must not replace B"));
	dvc::unregister_device(first_session, second_id);
	CHECK(find_device(second_id));
	std::atomic<bool> running{true};
	std::thread producer([&] {
		uint8_t first_image[16] = {}, second_image[16];
		memset(second_image, 255, sizeof(second_image));
		while (running) {
			first.write(first_image, 2, dvc::now_ns());
			second.write(second_image, 4, dvc::now_ns());
			Sleep(5);
		}
	});
	dvc::Handle child;
	launch(probe32, L"--probe " + quote(second_session) + L" " + quote(second_id), child);
	probe(first_session, first_id);
	wait_success(child);
	dvc::unregister_device(first_session, first_id);
	SetEvent(first_stop);
	wait_success(first_watchdog);
	CHECK(!find_device(first_id) && find_device(second_id));
	CHECK(dvc::load(second.header()->phase) == 6000000);
	probe(second_session, second_id);
	running = false;
	producer.join();
	dvc::unregister_device(second_session, second_id);
	SetEvent(second_stop);
	wait_success(second_watchdog);
	CHECK(!find_device(second_id));
	puts("Multiple CLSIDs, independent queues/phases, x86/x64 activation and isolated removal passed");
}

static void crash_publisher(const std::wstring &session, const std::wstring &dll64, const std::wstring &dll32,
			    const std::wstring &helper, const std::wstring &device_id)
{
	dvc::Queue queue;
	CHECK(queue.create(session, 2, 2, 3, {60, 1}));
	dvc::Handle stop, watchdog;
	start_watchdog(helper, session, stop, watchdog, device_id);
	CHECK(dvc::register_device(session, dll64, dll32, device_id));
	dvc::Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, dvc::object_name(session, L"test-ready").c_str()));
	CHECK(ready);
	SetEvent(ready);
	Sleep(INFINITE);
}

static int inspect_device(const std::wstring &device_id)
{
	dvc::Handle token;
	HANDLE raw_token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
		token.reset(raw_token);
		TOKEN_ELEVATION elevation = {};
		DWORD bytes = 0;
		if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) {
			printf("elevated=%lu architecture=%zu\n", elevation.TokenIsElevated, sizeof(void *) * 8);
		}
	}
	ComPtr<ICreateDevEnum> devices;
	HRESULT result =
		CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices));
	printf("CreateDeviceEnum=0x%08lX\n", static_cast<unsigned long>(result));
	if (FAILED(result)) {
		return 1;
	}
	ComPtr<IEnumMoniker> enumerator;
	result = devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumerator, 0);
	printf("CreateClassEnumerator=0x%08lX\n", static_cast<unsigned long>(result));
	if (result != S_OK) {
		return 1;
	}
	bool found = false;
	bool activated = false;
	ComPtr<IMoniker> moniker;
	while (enumerator->Next(1, &moniker, nullptr) == S_OK) {
		ComPtr<IPropertyBag> properties;
		if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties)))) {
			VARIANT name, class_id;
			VariantInit(&name);
			VariantInit(&class_id);
			properties->Read(L"FriendlyName", &name, nullptr);
			properties->Read(L"CLSID", &class_id, nullptr);
			printf("device=%ls clsid=%ls\n", name.vt == VT_BSTR ? name.bstrVal : L"",
			       class_id.vt == VT_BSTR ? class_id.bstrVal : L"");
			if (class_id.vt == VT_BSTR && _wcsicmp(class_id.bstrVal, device_id.c_str()) == 0) {
				found = true;
				ComPtr<IBaseFilter> camera;
				result = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&camera));
				activated = SUCCEEDED(result);
				printf("BindToObject=0x%08lX\n", static_cast<unsigned long>(result));
			}
			VariantClear(&class_id);
			VariantClear(&name);
		}
		moniker.Reset();
	}
	printf("requested_device_found=%d\n", found);
	return !found ? 1 : activated ? 0 : 3;
}

static void launch_elevated(const std::wstring &executable, const std::wstring &arguments, dvc::Handle &process)
{
	SHELLEXECUTEINFOW information = {};
	information.cbSize = sizeof(information);
	information.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	information.lpVerb = L"runas";
	information.lpFile = executable.c_str();
	information.lpParameters = arguments.c_str();
	information.nShow = SW_HIDE;
	CHECK(ShellExecuteExW(&information));
	process.reset(information.hProcess);
	CHECK(process);
}

static void check_machine_keys_removed(const std::wstring &device_id)
{
	for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
		for (const auto &path :
		     {L"Software\\Classes\\CLSID\\" + device_id,
		      L"Software\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\" + device_id}) {
			HKEY key = nullptr;
			CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | view, &key) ==
			      ERROR_FILE_NOT_FOUND);
		}
	}
}

static void start_machine_watchdog(const std::wstring &helper, const std::wstring &dll64, const std::wstring &dll32,
				   const std::wstring &session, const std::wstring &device_id, dvc::Handle &stop,
				   dvc::Handle &process)
{
	stop.reset(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(session, L"watchdog").c_str()));
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(session, L"registered").c_str()));
	CHECK(stop && ready);
	launch_elevated(helper,
			L"--register-machine " + quote(session) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
				std::to_wstring(dvc::process_start(GetCurrentProcess())) + L" " + quote(device_id) +
				L" " + quote(dll64) + L" " + quote(dll32) + L" " + quote(L"OBS DVC machine test"),
			process);
	HANDLE handles[] = {ready, process};
	CHECK(WaitForMultipleObjects(2, handles, FALSE, 15000) == WAIT_OBJECT_0);
	CHECK(dvc::active_session(device_id) == session);
}

static void machine_elevated_probe(const std::wstring &session, const std::wstring &device_id,
				   const std::wstring &probe32, const std::wstring &helper)
{
	CHECK(inspect_device(device_id) == 0);
	probe(session, device_id);
	dvc::Handle child;
	launch(probe32, L"--probe " + quote(session) + L" " + quote(device_id), child);
	wait_success(child);
	dvc::unregister_device(unique_session(), device_id, dvc::RegistrationScope::Machine);
	CHECK(find_device(device_id));
	const std::wstring stale_id = unique_session(), old_session = unique_session(), new_session = unique_session();
	dvc::Queue old_queue, new_queue;
	CHECK(old_queue.create(old_session, 2, 2, 3, {60, 1}));
	CHECK(new_queue.create(new_session, 2, 2, 3, {60, 1}));
	const std::wstring root = L"Software\\Classes\\CLSID\\" + device_id + L"\\InprocServer32";
	std::wstring dll64, dll32;
	for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
		HKEY key = nullptr;
		CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, root.c_str(), 0, KEY_READ | view, &key) == ERROR_SUCCESS);
		wchar_t path[32768] = {};
		DWORD bytes = sizeof(path), type = 0;
		CHECK(RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE *>(path), &bytes) ==
		      ERROR_SUCCESS);
		RegCloseKey(key);
		(view == KEY_WOW64_64KEY ? dll64 : dll32) = path;
	}
	dvc::Handle old_stop, old_watchdog, new_stop, new_watchdog;
	start_machine_watchdog(helper, dll64, dll32, old_session, stale_id, old_stop, old_watchdog);
	CHECK(!dvc::register_device(new_session, dll64, dll32, stale_id, L"must not replace a live session",
				    dvc::RegistrationScope::Machine));
	dvc::store(old_queue.header()->active, 0);
	start_machine_watchdog(helper, dll64, dll32, new_session, stale_id, new_stop, new_watchdog);
	CHECK(SetEvent(old_stop));
	wait_success(old_watchdog);
	CHECK(dvc::active_session(stale_id) == new_session);
	CHECK(SetEvent(new_stop));
	wait_success(new_watchdog);
	check_machine_keys_removed(stale_id);
	puts("Elevated x86/x64 activation, live-session protection, stale recovery and session-safe removal passed");
}

static void machine_registration_test(const std::wstring &dll64, const std::wstring &dll32, const std::wstring &helper,
				      const std::wstring &probe32)
{
	const std::wstring session = unique_session(), device_id = unique_session();
	dvc::Queue queue;
	CHECK(queue.create(session, 2, 2, 3, {60, 1}, dvc::PixelFormat::NV12));
	dvc::Handle stop, watchdog;
	start_machine_watchdog(helper, dll64, dll32, session, device_id, stop, watchdog);
	std::atomic<bool> running{true};
	std::thread producer([&] {
		uint8_t pixels[6] = {};
		while (running) {
			queue.write(pixels, 2, dvc::now_ns());
			Sleep(5);
		}
	});
	dvc::Handle child;
	launch(probe32, L"--probe " + quote(session) + L" " + quote(device_id), child);
	probe(session, device_id);
	wait_success(child);
	wchar_t executable[32768];
	CHECK(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))));
	launch_elevated(executable,
			L"--probe-machine-elevated " + quote(session) + L" " + quote(device_id) + L" " +
				quote(probe32) + L" " + quote(helper),
			child);
	wait_success(child);
	const std::wstring crash_session = unique_session(), crash_id = unique_session();
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(crash_session, L"test-ready").c_str()));
	CHECK(ready);
	launch(executable,
	       L"--crash-machine-publisher " + quote(crash_session) + L" " + quote(crash_id) + L" " + quote(dll64) +
		       L" " + quote(dll32) + L" " + quote(helper),
	       child);
	CHECK(WaitForSingleObject(ready, 120000) == WAIT_OBJECT_0);
	CHECK(find_device(device_id) && find_device(crash_id));
	CHECK(TerminateProcess(child, 123));
	CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
	const uint64_t until = dvc::now_ns() + 10000000000;
	while (!dvc::active_session(crash_id).empty() && dvc::now_ns() < until) {
		Sleep(10);
	}
	CHECK(dvc::active_session(crash_id).empty());
	check_machine_keys_removed(crash_id);
	CHECK(find_device(device_id));
	CHECK(SetEvent(stop));
	wait_success(watchdog);
	running = false;
	producer.join();
	CHECK(!find_device(device_id));
	check_machine_keys_removed(device_id);
	puts("Machine registration: normal/elevated x86/x64 receivers, isolated crash cleanup and normal stop passed");
}

int wmain(int count, wchar_t **arguments)
{
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	setvbuf(stdout, nullptr, _IONBF, 0);
	CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
	if (count == 6 && wcscmp(arguments[1], L"--probe-machine-elevated") == 0) {
		machine_elevated_probe(arguments[2], arguments[3], arguments[4], arguments[5]);
		CoUninitialize();
		return 0;
	}
	if (count == 7 && wcscmp(arguments[1], L"--crash-machine-publisher") == 0) {
		dvc::Queue queue;
		CHECK(queue.create(arguments[2], 2, 2, 3, {60, 1}));
		dvc::Handle stop, watchdog;
		start_machine_watchdog(arguments[6], arguments[4], arguments[5], arguments[2], arguments[3], stop,
				       watchdog);
		dvc::Handle ready(
			OpenEventW(EVENT_MODIFY_STATE, FALSE, dvc::object_name(arguments[2], L"test-ready").c_str()));
		CHECK(ready && SetEvent(ready));
		Sleep(INFINITE);
	}
	if (count == 6 && wcscmp(arguments[1], L"--machine-registration") == 0) {
		machine_registration_test(arguments[2], arguments[3], arguments[4], arguments[5]);
		CoUninitialize();
		return 0;
	}
	if ((count == 3 || count == 4) && wcscmp(arguments[1], L"--inspect") == 0) {
		if (count == 4) {
			FILE *report = nullptr;
			if (_wfreopen_s(&report, arguments[3], L"w", stdout) != 0) {
				CoUninitialize();
				return 2;
			}
		}
		const int result = inspect_device(arguments[2]);
		CoUninitialize();
		return result;
	}
	if ((count == 3 || count == 4) && wcscmp(arguments[1], L"--probe") == 0) {
		probe(arguments[2], count == 4 ? arguments[3] : dvc::camera_id);
		CoUninitialize();
		return 0;
	}
	if (count == 7 && wcscmp(arguments[1], L"--crash-publisher") == 0) {
		crash_publisher(arguments[2], arguments[3], arguments[4], arguments[5], arguments[6]);
	}
	if (count != 6 || wcscmp(arguments[1], L"--registration") != 0) {
		puts("Usage: dvc-device-check --registration dll64 dll32 watchdog probe32-exe");
		puts("       dvc-device-check --machine-registration dll64 dll32 watchdog probe32-exe (requires UAC)");
		puts("       dvc-device-check --inspect device-clsid [report-file] (read-only)");
		return 2;
	}
	const std::wstring device_id = unique_session();
	dvc::Handle owner(CreateMutexW(nullptr, TRUE, dvc::object_name(device_id, L"owner").c_str()));
	CHECK(owner && GetLastError() != ERROR_ALREADY_EXISTS);
	CHECK(dvc::active_session(device_id).empty());
	const std::wstring session = unique_session();
	dvc::Queue writer;
	CHECK(writer.create(session, 2, 2, 5, {60, 1}));
	dvc::Handle stop, watchdog;
	start_watchdog(arguments[4], session, stop, watchdog, device_id);
	CHECK(dvc::register_device(session, arguments[2], arguments[3], device_id));
	std::atomic<bool> running{true};
	std::thread producer([&] {
		uint8_t pixels[16] = {};
		while (running) {
			writer.write(pixels, 8, dvc::now_ns());
			++pixels[0];
			Sleep(5);
		}
	});
	dvc::Handle child;
	launch(arguments[5], L"--probe " + quote(session) + L" " + quote(device_id), child);
	probe(session, device_id);
	wait_success(child);
	puts("32-bit probe and concurrent receivers passed");
	running = false;
	producer.join();
	dvc::store(writer.header()->active, 0);
	dvc::unregister_device(session, device_id);
	SetEvent(stop);
	wait_success(watchdog);
	CHECK(!find_device(device_id));
	CHECK(dvc::active_session(device_id).empty());
	test_multiple_devices(arguments[2], arguments[3], arguments[4], arguments[5]);
	const std::wstring crash_session = unique_session();
	dvc::Handle ready(CreateEventW(nullptr, TRUE, FALSE, dvc::object_name(crash_session, L"test-ready").c_str()));
	wchar_t executable[32768];
	CHECK(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))));
	launch(executable,
	       L"--crash-publisher " + quote(crash_session) + L" " + quote(arguments[2]) + L" " + quote(arguments[3]) +
		       L" " + quote(arguments[4]) + L" " + quote(device_id),
	       child);
	CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
	CHECK(find_device(device_id));
	CHECK(TerminateProcess(child, 123));
	CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
	const uint64_t until = dvc::now_ns() + 5000000000;
	while (!dvc::active_session(device_id).empty() && dvc::now_ns() < until) {
		Sleep(10);
	}
	CHECK(dvc::active_session(device_id).empty());
	CHECK(!find_device(device_id));
	ReleaseMutex(owner);
	puts("Dynamic device lifecycle and crash cleanup passed");
	CoUninitialize();
	return 0;
}
