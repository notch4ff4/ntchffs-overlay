#ifdef _WIN32

#include "overlay-gallery.h"
#include "overlay-string-utils.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>

OverlayGallery::OverlayGallery() {}

std::wstring OverlayGallery::ToWide(const std::string &text)
{
	return overlay::util::Utf8ToWide(text);
}

bool OverlayGallery::IsSupportedVideo(const std::wstring &name)
{
	size_t dotPos = name.find_last_of(L'.');
	if (dotPos == std::wstring::npos) {
		return false;
	}
	std::wstring ext = name.substr(dotPos + 1);
	for (auto &ch : ext) {
		ch = static_cast<wchar_t>(towlower(ch));
	}
	return ext == L"mp4" || ext == L"mkv" || ext == L"mov" || ext == L"avi";
}

void OverlayGallery::Load(const std::string &folderPath)
{
	OpenRoot(folderPath);
}

void OverlayGallery::OpenRoot(const std::string &folderPath)
{
	m_folderPath = folderPath;
	m_rootPath = NormalizeDirectoryPath(ToWide(folderPath));
	m_currentPath = m_rootPath;
	LoadCurrentDirectory();
}

void OverlayGallery::ReloadCurrent()
{
	LoadCurrentDirectory();
}

std::wstring OverlayGallery::NormalizeDirectoryPath(const std::wstring &path)
{
	if (path.empty()) {
		return std::wstring();
	}
	std::wstring normalized = path;
	for (auto &ch : normalized) {
		if (ch == L'/') {
			ch = L'\\';
		}
	}
	while (normalized.size() > 1 && normalized.back() == L'\\') {
		normalized.pop_back();
	}
	return normalized;
}

std::wstring OverlayGallery::ParentDirectoryPath(const std::wstring &path)
{
	std::wstring normalized = NormalizeDirectoryPath(path);
	size_t slashPos = normalized.find_last_of(L'\\');
	if (slashPos == std::wstring::npos) {
		return normalized;
	}
	if (slashPos == 2 && normalized.size() > 2 && normalized[1] == L':') {
		return normalized.substr(0, 3);
	}
	if (slashPos == 0) {
		return normalized.substr(0, 1);
	}
	return normalized.substr(0, slashPos);
}

bool OverlayGallery::IsSamePath(const std::wstring &a, const std::wstring &b)
{
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (towlower(a[i]) != towlower(b[i])) {
			return false;
		}
	}
	return true;
}

bool OverlayGallery::CanGoUp() const
{
	if (m_rootPath.empty() || m_currentPath.empty()) {
		return false;
	}
	return !IsSamePath(m_rootPath, m_currentPath);
}

bool OverlayGallery::GoUp()
{
	if (!CanGoUp()) {
		return false;
	}
	std::wstring parent = ParentDirectoryPath(m_currentPath);
	if (parent.empty()) {
		return false;
	}
	m_currentPath = parent;
	LoadCurrentDirectory();
	return true;
}

bool OverlayGallery::EnterFolder(size_t index)
{
	const GalleryItem *item = GetItem(index);
	if (!item || item->type != GalleryItem::Type::Folder) {
		return false;
	}
	std::wstring nextPath = NormalizeDirectoryPath(item->path);
	if (nextPath.empty()) {
		return false;
	}
	m_currentPath = nextPath;
	LoadCurrentDirectory();
	return true;
}

void OverlayGallery::LoadCurrentDirectory()
{
	m_items.clear();

	if (m_currentPath.empty()) {
		return;
	}

	std::wstring folderW = m_currentPath + L'\\';
	std::wstring pattern = folderW + L"*.*";

	WIN32_FIND_DATAW findData = {};
	HANDLE hFind = FindFirstFileW(pattern.c_str(), &findData);
	if (hFind == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		std::wstring name = findData.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		ULARGE_INTEGER mod = {};
		mod.HighPart = findData.ftLastWriteTime.dwHighDateTime;
		mod.LowPart = findData.ftLastWriteTime.dwLowDateTime;

		GalleryItem item;
		bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (isDirectory) {
			item.type = GalleryItem::Type::Folder;
		} else if (!IsSupportedVideo(name)) {
			continue;
		}
		item.name = name;
		item.path = folderW + name;
		item.sizeBytes = 0;
		if (!isDirectory) {
			ULARGE_INTEGER size = {};
			size.HighPart = findData.nFileSizeHigh;
			size.LowPart = findData.nFileSizeLow;
			item.type = GalleryItem::Type::Video;
			item.sizeBytes = static_cast<uint64_t>(size.QuadPart);
		}
		item.modifiedTime = static_cast<uint64_t>(mod.QuadPart);
		m_items.push_back(item);
	} while (FindNextFileW(hFind, &findData));

	FindClose(hFind);

	std::sort(m_items.begin(), m_items.end(),
		  [](const GalleryItem &a, const GalleryItem &b) {
			  if (a.type != b.type) {
				  return a.type == GalleryItem::Type::Folder;
			  }
			  if (a.type == GalleryItem::Type::Folder) {
				  std::wstring an = a.name;
				  std::wstring bn = b.name;
				  for (auto &ch : an) {
					  ch = static_cast<wchar_t>(towlower(ch));
				  }
				  for (auto &ch : bn) {
					  ch = static_cast<wchar_t>(towlower(ch));
				  }
				  return an < bn;
			  }
			  return a.modifiedTime > b.modifiedTime;
		  });
}

const GalleryItem *OverlayGallery::GetItem(size_t index) const
{
	if (index >= m_items.size()) {
		return nullptr;
	}
	return &m_items[index];
}

std::string OverlayGallery::GetCurrentPath() const
{
	return overlay::util::WideToUtf8(m_currentPath);
}

#endif // _WIN32
