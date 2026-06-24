#pragma once

#include <string>

namespace overlay::util {

std::string WideToUtf8(const std::wstring &wide);
std::wstring Utf8ToWide(const char *utf8);
std::wstring Utf8ToWide(const std::string &utf8);
std::wstring ModuleTextW(const char *key);

} // namespace overlay::util
