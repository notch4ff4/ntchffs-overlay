#pragma once

// Adapted from Segra Mp4BoxReader (Segra.Backend.Media):
// walks ISO-BMFF moov/trak to read OBS audio encoder names from trak/udta/name,
// which FFmpeg's mov demuxer does not surface.

#include <optional>
#include <string>
#include <vector>

namespace overlay::mp4 {

// Returns per-track audio names in container order.
// nullopt = track has no udta/name; empty vector = not ISO-BMFF / error / no audio.
std::vector<std::optional<std::string>> ReadMp4AudioTrackNames(const std::wstring &filePath);

} // namespace overlay::mp4
