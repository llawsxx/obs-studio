#include "ipc.hpp"
#include <shellapi.h>

static int register_and_watch(wchar_t **arguments)
{
	const std::wstring session = arguments[2], device_id = arguments[5];
	const DWORD pid = wcstoul(arguments[3], nullptr, 10);
	const uint64_t start = _wcstoui64(arguments[4], nullptr, 10);
	dvc::Handle owner(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
	dvc::Handle stop(OpenEventW(SYNCHRONIZE, FALSE, dvc::object_name(session, L"watchdog").c_str()));
	dvc::Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, dvc::object_name(session, L"registered").c_str()));
	dvc::Queue queue;
	if (!owner || !stop || !ready || dvc::process_start(owner) != start ||
	    WaitForSingleObject(owner, 0) != WAIT_TIMEOUT || WaitForSingleObject(stop, 0) != WAIT_TIMEOUT ||
	    !queue.open(session) || queue.header()->owner_pid != pid || queue.header()->owner_start != start) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!dvc::register_device(session, arguments[6], arguments[7], device_id, arguments[8],
				  dvc::RegistrationScope::Machine)) {
		return ERROR_ACCESS_DENIED;
	}
	HANDLE handles[] = {owner, stop};
	const bool running = WaitForMultipleObjects(2, handles, FALSE, 0) == WAIT_TIMEOUT;
	if (running && SetEvent(ready)) {
		WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	}
	dvc::unregister_device(session, device_id, dvc::RegistrationScope::Machine);
	return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int count = 0;
	wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
	if (arguments && count == 9 && wcscmp(arguments[1], L"--register-machine") == 0) {
		const int result = register_and_watch(arguments);
		LocalFree(arguments);
		return result;
	}
	if (!arguments || (count != 4 && count != 5)) {
		if (arguments) {
			LocalFree(arguments);
		}
		return 1;
	}
	const std::wstring session = arguments[1];
	const std::wstring device_id = count == 5 ? arguments[4] : dvc::camera_id;
	const DWORD pid = wcstoul(arguments[2], nullptr, 10);
	const uint64_t start = _wcstoui64(arguments[3], nullptr, 10);
	LocalFree(arguments);
	dvc::Handle owner(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
	dvc::Handle stop(OpenEventW(SYNCHRONIZE, FALSE, dvc::object_name(session, L"watchdog").c_str()));
	if (owner && stop && dvc::process_start(owner) == start) {
		HANDLE handles[] = {owner, stop};
		WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	}
	dvc::unregister_device(session, device_id);
	return 0;
}
