// Adapted from Segra Mp4BoxReader (Segra.Backend.Media):
// walks ISO-BMFF moov/trak to read OBS audio encoder names from trak/udta/name,
// which FFmpeg's mov demuxer does not surface.

#include "overlay-mp4-box-reader.h"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace overlay::mp4 {
namespace {

struct BoxSpan {
	char type[5] = {};
	int payloadStart = 0;
	int payloadLength = 0;
};

uint32_t ReadUInt32BE(const uint8_t *buf, int offset)
{
	return (static_cast<uint32_t>(buf[offset]) << 24) | (static_cast<uint32_t>(buf[offset + 1]) << 16) |
	       (static_cast<uint32_t>(buf[offset + 2]) << 8) | static_cast<uint32_t>(buf[offset + 3]);
}

uint64_t ReadUInt64BE(const uint8_t *buf, int offset)
{
	return (static_cast<uint64_t>(ReadUInt32BE(buf, offset)) << 32) | ReadUInt32BE(buf, offset + 4);
}

bool TypeEquals(const char type[5], const char *fourCc)
{
	return std::memcmp(type, fourCc, 4) == 0;
}

// Iterates direct children of a box region.
std::vector<BoxSpan> EnumerateBoxes(const std::vector<uint8_t> &data, int start, int length)
{
	std::vector<BoxSpan> boxes;
	int pos = start;
	const int end = start + length;
	while (pos + 8 <= end) {
		const uint32_t boxSize32 = ReadUInt32BE(data.data(), pos);
		BoxSpan span;
		std::memcpy(span.type, data.data() + pos + 4, 4);

		int payloadStart;
		int totalSize;
		if (boxSize32 == 1) {
			if (pos + 16 > end)
				break;
			const uint64_t large = ReadUInt64BE(data.data(), pos + 8);
			if (large > static_cast<uint64_t>(INT_MAX))
				break;
			totalSize = static_cast<int>(large);
			payloadStart = pos + 16;
		} else if (boxSize32 == 0) {
			totalSize = end - pos;
			payloadStart = pos + 8;
		} else {
			totalSize = static_cast<int>(boxSize32);
			payloadStart = pos + 8;
		}

		if (totalSize < 8 || pos + totalSize > end)
			break;

		span.payloadStart = payloadStart;
		span.payloadLength = (pos + totalSize) - payloadStart;
		boxes.push_back(span);
		pos += totalSize;
	}
	return boxes;
}

// Scans top-level boxes and returns the payload of the first matching fourCc.
std::vector<uint8_t> ReadTopLevelBoxPayload(HANDLE file, const char *fourCc, LARGE_INTEGER fileSize)
{
	std::vector<uint8_t> empty;
	int64_t pos = 0;
	uint8_t header[16];

	while (pos + 8 <= fileSize.QuadPart) {
		LARGE_INTEGER seekPos;
		seekPos.QuadPart = pos;
		if (!SetFilePointerEx(file, seekPos, nullptr, FILE_BEGIN))
			return empty;

		DWORD read = 0;
		if (!ReadFile(file, header, 8, &read, nullptr) || read != 8)
			return empty;

		const uint32_t boxSize32 = ReadUInt32BE(header, 0);
		char type[5] = {};
		std::memcpy(type, header + 4, 4);

		int64_t payloadStart;
		int64_t totalBoxSize;
		if (boxSize32 == 1) {
			if (!ReadFile(file, header + 8, 8, &read, nullptr) || read != 8)
				return empty;
			totalBoxSize = static_cast<int64_t>(ReadUInt64BE(header, 8));
			payloadStart = pos + 16;
		} else if (boxSize32 == 0) {
			totalBoxSize = fileSize.QuadPart - pos;
			payloadStart = pos + 8;
		} else {
			totalBoxSize = boxSize32;
			payloadStart = pos + 8;
		}

		if (totalBoxSize < 8 || pos + totalBoxSize > fileSize.QuadPart)
			return empty;

		if (TypeEquals(type, fourCc)) {
			const int64_t payloadSize = (pos + totalBoxSize) - payloadStart;
			if (payloadSize < 0 || payloadSize > INT_MAX)
				return empty;
			std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
			seekPos.QuadPart = payloadStart;
			if (!SetFilePointerEx(file, seekPos, nullptr, FILE_BEGIN))
				return empty;
			if (payloadSize > 0) {
				if (!ReadFile(file, payload.data(), static_cast<DWORD>(payloadSize), &read, nullptr) ||
				    read != static_cast<DWORD>(payloadSize)) {
					return empty;
				}
			}
			return payload;
		}

		pos += totalBoxSize;
	}
	return empty;
}

void ParseMoov(const std::vector<uint8_t> &moov, std::vector<std::optional<std::string>> &audioTrackNames)
{
	for (const BoxSpan &trak : EnumerateBoxes(moov, 0, static_cast<int>(moov.size()))) {
		if (!TypeEquals(trak.type, "trak"))
			continue;

		bool isAudio = false;
		std::optional<std::string> trackName;

		for (const BoxSpan &child : EnumerateBoxes(moov, trak.payloadStart, trak.payloadLength)) {
			if (TypeEquals(child.type, "mdia")) {
				for (const BoxSpan &mdiaChild :
				     EnumerateBoxes(moov, child.payloadStart, child.payloadLength)) {
					if (!TypeEquals(mdiaChild.type, "hdlr"))
						continue;
					// hdlr: fullbox(4) + pre_defined(4) + handler_type(4) + ...
					if (mdiaChild.payloadLength >= 12) {
						char handlerType[5] = {};
						std::memcpy(handlerType, moov.data() + mdiaChild.payloadStart + 8, 4);
						isAudio = TypeEquals(handlerType, "soun");
					}
					break;
				}
			} else if (TypeEquals(child.type, "udta")) {
				for (const BoxSpan &udtaChild :
				     EnumerateBoxes(moov, child.payloadStart, child.payloadLength)) {
					if (!TypeEquals(udtaChild.type, "name"))
						continue;
					int len = udtaChild.payloadLength;
					while (len > 0 && moov[static_cast<size_t>(udtaChild.payloadStart + len - 1)] == 0)
						len--;
					if (len > 0) {
						trackName = std::string(
							reinterpret_cast<const char *>(moov.data() + udtaChild.payloadStart),
							static_cast<size_t>(len));
					}
					break;
				}
			}
		}

		if (isAudio)
			audioTrackNames.push_back(std::move(trackName));
	}
}

} // namespace

std::vector<std::optional<std::string>> ReadMp4AudioTrackNames(const std::wstring &filePath)
{
	std::vector<std::optional<std::string>> empty;

	HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return empty;

	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 8) {
		CloseHandle(file);
		return empty;
	}

	std::vector<uint8_t> moov = ReadTopLevelBoxPayload(file, "moov", fileSize);
	CloseHandle(file);
	if (moov.empty())
		return empty;

	std::vector<std::optional<std::string>> rawNames;
	ParseMoov(moov, rawNames);
	return rawNames;
}

} // namespace overlay::mp4
