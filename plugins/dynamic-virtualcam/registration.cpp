#include "ipc.hpp"
#include "media-format.hpp"

#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <memory>
#include <sddl.h>

namespace dvc {

struct __declspec(uuid("97F7C4D4-547B-4A5F-8332-536430AD2E4D")) FilterDataEncoder : IUnknown {
	virtual HRESULT STDMETHODCALLTYPE ParseFilterData(BYTE *, ULONG, BYTE **) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateFilterData(REGFILTER2 *, BYTE **, ULONG *) = 0;
};

static bool make_filter_data(std::vector<uint8_t> &data, PixelFormat format)
{
	struct ComScope {
		HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		~ComScope()
		{
			if (SUCCEEDED(result)) {
				CoUninitialize();
			}
		}
	} scope;
	if (FAILED(scope.result) && scope.result != RPC_E_CHANGED_MODE) {
		return false;
	}
	Microsoft::WRL::ComPtr<FilterDataEncoder> encoder;
	if (FAILED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)))) {
		return false;
	}
	const REGPINTYPES media = {&MEDIATYPE_Video, &media_subtype(format)};
	REGFILTERPINS2 pin = {};
	pin.dwFlags = REG_PINFLAG_B_OUTPUT;
	pin.cInstances = 1;
	pin.nMediaTypes = 1;
	pin.lpMediaType = &media;
	pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
	REGFILTER2 filter = {};
	filter.dwVersion = 2;
	filter.dwMerit = MERIT_DO_NOT_USE;
	filter.cPins2 = 1;
	filter.rgPins2 = &pin;
	BYTE *encoded = nullptr;
	ULONG size = 0;
	const HRESULT result = encoder->CreateFilterData(&filter, &encoded, &size);
	std::unique_ptr<BYTE, decltype(&CoTaskMemFree)> buffer(encoded, &CoTaskMemFree);
	if (FAILED(result) || !encoded || !size) {
		return false;
	}
	data.assign(encoded, encoded + size);
	return true;
}

class RegistryLock {
	Handle mutex;
	bool acquired = false;

public:
	RegistryLock(RegistrationScope scope)
		: mutex(CreateMutexW(nullptr, FALSE,
				     scope == RegistrationScope::Machine
					     ? L"Local\\OBS-DVC-Machine-Registration-D02ABF12"
					     : L"Local\\OBS-DVC-Registration-D02ABF12"))
	{
		const DWORD result = WaitForSingleObject(mutex, INFINITE);
		acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
	}
	~RegistryLock()
	{
		if (acquired) {
			ReleaseMutex(mutex);
		}
	}
	explicit operator bool() const { return acquired; }
};

static std::wstring class_path(const std::wstring &device_id)
{
	return L"Software\\Classes\\CLSID\\" + device_id;
}

static std::wstring category_path(const std::wstring &device_id)
{
	return L"Software\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\" + device_id;
}

static bool valid_id(const std::wstring &value)
{
	GUID id;
	wchar_t canonical[40];
	return value.size() == 38 && SUCCEEDED(CLSIDFromString(value.c_str(), &id)) &&
	       StringFromGUID2(id, canonical, 40) && _wcsicmp(value.c_str(), canonical) == 0;
}

static bool set_string(HKEY key, const wchar_t *name, const std::wstring &value)
{
	return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
			      DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static HKEY registry_hive(RegistrationScope scope)
{
	return scope == RegistrationScope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

static std::wstring read_string(HKEY hive, const std::wstring &path, REGSAM view, const wchar_t *name)
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS) {
		return {};
	}
	wchar_t value[256] = {};
	DWORD bytes = sizeof(value), type = 0;
	const LONG result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(value), &bytes);
	RegCloseKey(key);
	return result == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t) && bytes <= sizeof(value) &&
			       value[bytes / sizeof(wchar_t) - 1] == 0
		       ? value
		       : L"";
}

static std::wstring read_session(HKEY hive, REGSAM view, const std::wstring &device_id)
{
	const auto path = class_path(device_id);
	return read_string(hive, path, view, L"OBS-DVC-Owner") == camera_id ? read_string(hive, path, view, L"Session")
									    : L"";
}

std::wstring active_session(const std::wstring &device_id)
{
	if (!valid_id(device_id)) {
		return {};
	}
	const auto machine = read_session(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, device_id);
	return machine.empty() ? read_session(HKEY_CURRENT_USER, KEY_WOW64_64KEY, device_id) : machine;
}

static std::wstring process_user(HANDLE process)
{
	HANDLE raw_token = nullptr;
	if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
		return {};
	}
	Handle token(raw_token);
	DWORD bytes = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
	std::vector<uint8_t> buffer(bytes);
	if (!bytes || !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
		return {};
	}
	wchar_t *text = nullptr;
	if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
		return {};
	}
	const std::wstring user = text;
	LocalFree(text);
	return user;
}

static bool key_exists(HKEY hive, const std::wstring &path, REGSAM view)
{
	HKEY key = nullptr;
	const LONG result = RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | view, &key);
	if (key) {
		RegCloseKey(key);
	}
	return result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND;
}

static void remove_tree(HKEY hive, const std::wstring &path, REGSAM view)
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(hive, path.c_str(), 0, KEY_ALL_ACCESS | view, &key) != ERROR_SUCCESS) {
		return;
	}
	RegDeleteTreeW(key, nullptr);
	RegCloseKey(key);
	RegDeleteKeyExW(hive, path.c_str(), view, 0);
}

static void unregister_view(HKEY hive, REGSAM view, const std::wstring &session, const std::wstring &device_id)
{
	if (read_session(hive, view, device_id) != session) {
		return;
	}
	const auto category = category_path(device_id);
	const auto marker = read_string(hive, category, view, L"OBS-DVC-Owner");
	if ((marker == camera_id && read_string(hive, category, view, L"Session") == session) ||
	    (hive == HKEY_CURRENT_USER && marker.empty() && read_string(hive, category, view, L"CLSID") == device_id)) {
		remove_tree(hive, category, view);
	}
	remove_tree(hive, class_path(device_id), view);
}

void unregister_device(const std::wstring &session, const std::wstring &device_id, RegistrationScope scope)
{
	RegistryLock lock(scope);
	if (!lock || !valid_id(session) || !valid_id(device_id)) {
		return;
	}
	for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
		unregister_view(registry_hive(scope), view, session, device_id);
	}
}

static bool register_view(HKEY hive, const std::wstring &user, const std::wstring &session, const std::wstring &dll,
			  REGSAM view, const std::wstring &device_id, const std::wstring &name,
			  const std::vector<uint8_t> &filter_data)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(hive, class_path(device_id).c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &key,
			    nullptr) != ERROR_SUCCESS) {
		return false;
	}
	bool success = set_string(key, L"OBS-DVC-Owner", camera_id) && set_string(key, L"Session", session) &&
		       set_string(key, L"OBS-DVC-User", user) && set_string(key, nullptr, name);
	HKEY server = nullptr;
	if (success && RegCreateKeyExW(key, L"InprocServer32", 0, nullptr, 0, KEY_WRITE | view, nullptr, &server,
				       nullptr) == ERROR_SUCCESS) {
		success = set_string(server, nullptr, dll) && set_string(server, L"ThreadingModel", L"Both");
		RegCloseKey(server);
	} else {
		success = false;
	}
	RegCloseKey(key);
	if (!success) {
		return false;
	}
	if (RegCreateKeyExW(hive, category_path(device_id).c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &key,
			    nullptr) != ERROR_SUCCESS) {
		return false;
	}
	success = set_string(key, L"FriendlyName", name) && set_string(key, L"CLSID", device_id) &&
		  set_string(key, L"OBS-DVC-Owner", camera_id) && set_string(key, L"Session", session) &&
		  RegSetValueExW(key, L"FilterData", 0, REG_BINARY, filter_data.data(), DWORD(filter_data.size())) ==
			  ERROR_SUCCESS;
	RegCloseKey(key);
	return success;
}

bool register_device(const std::wstring &session, const std::wstring &dll64, const std::wstring &dll32,
		     const std::wstring &device_id, const std::wstring &name, RegistrationScope scope)
{
	RegistryLock lock(scope);
	if (!lock || !valid_id(session) || !valid_id(device_id) || name.empty() || name.size() > 128 ||
	    GetFileAttributesW(dll64.c_str()) == INVALID_FILE_ATTRIBUTES ||
	    GetFileAttributesW(dll32.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	Queue queue;
	if (!queue.open(session)) {
		return false;
	}
	const auto user = process_user(GetCurrentProcess());
	Handle publisher(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, queue.header()->owner_pid));
	if (user.empty() || !publisher || process_start(publisher) != queue.header()->owner_start ||
	    process_user(publisher) != user) {
		return false;
	}
	const HKEY hive = registry_hive(scope);
	for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
		const auto previous = read_session(hive, view, device_id);
		if (key_exists(hive, class_path(device_id), view)) {
			if (previous.empty()) {
				return false;
			}
			if (previous != session) {
				Queue live;
				if (scope != RegistrationScope::Machine ||
				    read_string(hive, class_path(device_id), view, L"OBS-DVC-User") != user ||
				    live.open(previous)) {
					return false;
				}
			}
		}
		if (key_exists(hive, category_path(device_id), view)) {
			const auto marker = read_string(hive, category_path(device_id), view, L"OBS-DVC-Owner");
			if ((marker != camera_id ||
			     read_string(hive, category_path(device_id), view, L"Session") != previous) &&
			    !(scope == RegistrationScope::User && !previous.empty() && marker.empty() &&
			      read_string(hive, category_path(device_id), view, L"CLSID") == device_id)) {
				return false;
			}
		}
	}
	std::vector<uint8_t> filter_data;
	if (!make_filter_data(filter_data, PixelFormat(queue.header()->pixel_format))) {
		return false;
	}
	if (register_view(hive, user, session, dll64, KEY_WOW64_64KEY, device_id, name, filter_data) &&
	    register_view(hive, user, session, dll32, KEY_WOW64_32KEY, device_id, name, filter_data)) {
		return true;
	}
	for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
		unregister_view(hive, view, session, device_id);
	}
	return false;
}

}
