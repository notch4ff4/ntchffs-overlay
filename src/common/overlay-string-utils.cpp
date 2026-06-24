#ifdef _WIN32

#include "overlay-string-utils.h"
#include <obs-module.h>
#include <cstring>
#include <windows.h>

namespace overlay::util {

std::string WideToUtf8(const std::wstring &wide)
{
	if (wide.empty()) {
		return std::string();
	}
	int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, NULL, NULL);
	if (needed <= 0) {
		return std::string();
	}
	std::string utf8(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], needed, NULL, NULL);
	utf8.resize(strlen(utf8.c_str()));
	return utf8;
}

std::wstring Utf8ToWide(const char *utf8)
{
	if (!utf8 || utf8[0] == '\0') {
		return std::wstring();
	}
	int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring wide(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], needed);
	wide.resize(wcslen(wide.c_str()));
	return wide;
}

std::wstring Utf8ToWide(const std::string &utf8)
{
	return Utf8ToWide(utf8.c_str());
}

std::wstring ModuleTextW(const char *key)
{
	return Utf8ToWide(obs_module_text(key));
}

} // namespace overlay::util

#endif // _WIN32
