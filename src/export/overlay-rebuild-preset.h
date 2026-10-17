#pragma once

#ifdef _WIN32

#include <obs-data.h>
#include <string>
#include <vector>

// Top-level processing type shown in UI.
enum class RebuildMode {
	Copy = 0,
	Reencode = 1
};

// Output container/codec family used only when mode == Reencode.
enum class RebuildOutputFormat {
	Mp4 = 0,
	Mov = 1,
	Gif = 2
};

// Coarse quality tiers. Video uses `crf` primarily.
enum class RebuildQuality {
	Low = 0,      // smaller file
	Medium = 1,   // balanced
	High = 2,     // better quality
	VeryHigh = 3  // largest / best
};

struct RebuildPreset {
	std::string id;
	std::wstring name;
	RebuildMode mode = RebuildMode::Reencode;
	RebuildOutputFormat format = RebuildOutputFormat::Mp4;
	RebuildQuality quality = RebuildQuality::Medium;
	int crf = 28;     // 18..51 for H.264 reencode (lower = higher quality)
	int width = 0;    // 0 = source
	int height = 0;   // 0 = source / auto from width
	double fps = 0.0; // 0 = source
	bool keepAudio = true;
	// GIF extras (when format == Gif)
	int gifColors = 256; // 2..256
	int gifDither = 1;   // 0=none, 1=bayer, 2=floyd
	int gifLoop = 0;     // 0 = infinite

	// Track selection (portable across files via audio ordinals).
	// tracksConfigured==false → keep all streams (default / seed presets).
	bool tracksConfigured = false;
	bool keepVideoTrack = true;
	std::vector<int> keepAudioOrdinals; // 0-based audio track indices to export

	int Crf() const;
	void SetCrf(int value);
	const char *ContainerName() const; // "mp4" / "mov" / "gif"
	std::wstring FormatLabel() const;
	std::wstring QualityLabel() const; // CRF / GIF colors detail
	std::wstring ResolutionLabel() const;
	std::wstring FpsLabel() const;

	bool ShouldKeepAudioOrdinal(int ordinal) const;
};


obs_data_t *RebuildPresetToObsData(const RebuildPreset &p);
RebuildPreset RebuildPresetFromObsData(obs_data_t *o);

int CrfFromQuality(RebuildQuality q);
RebuildQuality QualityFromCrf(int crf);

struct RebuildPresetStore {
	std::vector<RebuildPreset> presets;
	std::string selectedId;
	int maxParallel = 1;

	static RebuildPreset MakeSeedStrong();
	static RebuildPreset MakeSeedBalanced();
	static RebuildPreset MakeSeedQuality();
	static RebuildPreset MakeSeedCopy();
	static std::vector<RebuildPreset> DefaultSeedPresets();

	void EnsureSeeded(obs_data_t *settings);
	void Load(obs_data_t *settings);
	void Save(obs_data_t *settings) const;

	const RebuildPreset *FindById(const std::string &id) const;
	RebuildPreset *FindByIdMutable(const std::string &id);
	RebuildPreset GetSelectedOrDefault() const;
	bool Upsert(const RebuildPreset &preset);
	bool Remove(const std::string &id);
	std::string GenerateId() const;
};

RebuildMode RebuildModeFromInt(int v);
int RebuildModeToInt(RebuildMode mode);
RebuildOutputFormat RebuildFormatFromInt(int v);
int RebuildFormatToInt(RebuildOutputFormat f);
RebuildQuality RebuildQualityFromInt(int v);
int RebuildQualityToInt(RebuildQuality q);
std::wstring RebuildModeLabel(RebuildMode mode);

std::string MigrateLegacyCompressToPresetId(bool compress, int qualityIndex);

#endif
