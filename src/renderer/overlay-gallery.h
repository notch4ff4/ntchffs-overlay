#pragma once

#ifdef _WIN32

#include <string>
#include <vector>
#include <stdint.h>

struct GalleryItem {
	enum class Type {
		Folder,
		Video
	};

	Type type = Type::Video;
	std::wstring path;
	std::wstring name;
	uint64_t sizeBytes;
	uint64_t modifiedTime;
};

class OverlayGallery {
public:
	OverlayGallery();

	void Load(const std::string &folderPath);
	void OpenRoot(const std::string &folderPath);
	void ReloadCurrent();
	bool EnterFolder(size_t index);
	bool GoUp();
	bool CanGoUp() const;
	const std::vector<GalleryItem> &GetItems() const { return m_items; }
	const GalleryItem *GetItem(size_t index) const;
	const std::string &GetFolderPath() const { return m_folderPath; }
	std::string GetCurrentPath() const;
	const std::wstring &GetCurrentPathWide() const { return m_currentPath; }

private:
	static bool IsSupportedVideo(const std::wstring &name);
	static std::wstring ToWide(const std::string &text);
	static std::wstring NormalizeDirectoryPath(const std::wstring &path);
	static std::wstring ParentDirectoryPath(const std::wstring &path);
	static bool IsSamePath(const std::wstring &a, const std::wstring &b);
	void LoadCurrentDirectory();

	std::string m_folderPath;
	std::wstring m_rootPath;
	std::wstring m_currentPath;
	std::vector<GalleryItem> m_items;
};

#endif // _WIN32
