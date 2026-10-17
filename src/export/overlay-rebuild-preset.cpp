#ifdef _WIN32

#include "overlay-rebuild-preset.h"
#include "overlay-string-utils.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

using overlay::util::Utf8ToWide;
using overlay::util::WideToUtf8;

namespace {

constexpr const char *kSeededKey = "rebuild_presets_seeded";
constexpr const char *kArrayKey = "rebuild_presets";
constexpr const char *kSelectedKey = "rebuild_preset_selected";
constexpr const char *kMaxParallelKey = "gallery_export_max_parallel";

} // namespace

int CrfFromQuality(RebuildQuality q)
{
	switch (q) {
	case RebuildQuality::Low:
		return 32;
	case RebuildQuality::High:
		return 23;
	case RebuildQuality::VeryHigh:
		return 18;
	case RebuildQuality::Medium:
	default:
		return 28;
	}
}

RebuildQuality QualityFromCrf(int crf)
{
	if (crf >= 30)
		return RebuildQuality::Low;
	if (crf >= 26)
		return RebuildQuality::Medium;
	if (crf >= 20)
		return RebuildQuality::High;
	return RebuildQuality::VeryHigh;
}

static RebuildPreset Normalize(RebuildPreset p)
{
	if (p.mode != RebuildMode::Copy && p.mode != RebuildMode::Reencode)
		p.mode = RebuildMode::Reencode;
	p.format = RebuildFormatFromInt(RebuildFormatToInt(p.format));
	if (p.crf < 18)
		p.crf = 18;
	if (p.crf > 51)
		p.crf = 51;
	p.quality = QualityFromCrf(p.crf);
	if (p.width < 0)
		p.width = 0;
	if (p.height < 0)
		p.height = 0;
	if (p.fps < 0.0)
		p.fps = 0.0;
	if (p.gifColors < 2)
		p.gifColors = 2;
	if (p.gifColors > 256)
		p.gifColors = 256;
	if (p.gifDither < 0)
		p.gifDither = 0;
	if (p.gifDither > 2)
		p.gifDither = 2;
	if (p.gifLoop < 0)
		p.gifLoop = 0;
	if (p.tracksConfigured)
		p.keepAudio = !p.keepAudioOrdinals.empty();
	return p;
}

RebuildPreset RebuildPresetFromObsData(obs_data_t *o)
{
	RebuildPreset p;
	if (!o)
		return p;
	const char *id = obs_data_get_string(o, "id");
	const char *name = obs_data_get_string(o, "name");
	p.id = id ? id : "";
	p.name = name ? Utf8ToWide(name) : L"";

	int modeInt = static_cast<int>(obs_data_get_int(o, "mode"));

	if (modeInt == 2) {
		p.mode = RebuildMode::Reencode;
		p.format = RebuildOutputFormat::Gif;
	} else {
		p.mode = RebuildModeFromInt(modeInt);
		if (obs_data_has_user_value(o, "format"))
			p.format = RebuildFormatFromInt(static_cast<int>(obs_data_get_int(o, "format")));
		else
			p.format = RebuildOutputFormat::Mp4;
	}

	if (obs_data_has_user_value(o, "crf")) {
		p.crf = static_cast<int>(obs_data_get_int(o, "crf"));
		p.quality = QualityFromCrf(p.crf);
	} else if (obs_data_has_user_value(o, "quality")) {
		p.quality = RebuildQualityFromInt(static_cast<int>(obs_data_get_int(o, "quality")));
		p.crf = CrfFromQuality(p.quality);
	}

	p.width = static_cast<int>(obs_data_get_int(o, "width"));
	p.height = static_cast<int>(obs_data_get_int(o, "height"));
	p.fps = obs_data_get_double(o, "fps");
	if (p.format == RebuildOutputFormat::Gif) {
		if (obs_data_has_user_value(o, "gif_width") && p.width <= 0)
			p.width = static_cast<int>(obs_data_get_int(o, "gif_width"));
		if (obs_data_has_user_value(o, "gif_height") && p.height <= 0)
			p.height = static_cast<int>(obs_data_get_int(o, "gif_height"));
		if (obs_data_has_user_value(o, "gif_fps") && p.fps <= 0.0)
			p.fps = obs_data_get_double(o, "gif_fps");
	}
	p.keepAudio = obs_data_has_user_value(o, "keep_audio") ? obs_data_get_bool(o, "keep_audio") : true;
	p.gifColors = obs_data_has_user_value(o, "gif_colors") ? static_cast<int>(obs_data_get_int(o, "gif_colors"))
							       : 256;
	p.gifDither = obs_data_has_user_value(o, "gif_dither") ? static_cast<int>(obs_data_get_int(o, "gif_dither"))
							       : 1;
	p.gifLoop = obs_data_has_user_value(o, "gif_loop") ? static_cast<int>(obs_data_get_int(o, "gif_loop")) : 0;

	p.tracksConfigured = obs_data_has_user_value(o, "tracks_configured")
				     ? obs_data_get_bool(o, "tracks_configured")
				     : false;
	p.keepVideoTrack = !obs_data_has_user_value(o, "keep_video") || obs_data_get_bool(o, "keep_video");
	p.keepAudioOrdinals.clear();
	obs_data_array_t *tracks = obs_data_get_array(o, "keep_audio_ordinals");
	if (tracks) {
		const size_t n = obs_data_array_count(tracks);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *ti = obs_data_array_item(tracks, i);
			if (ti) {
				p.keepAudioOrdinals.push_back(static_cast<int>(obs_data_get_int(ti, "i")));
				obs_data_release(ti);
			}
		}
		obs_data_array_release(tracks);
		if (!p.tracksConfigured && (!p.keepAudioOrdinals.empty() || obs_data_has_user_value(o, "keep_video")))
			p.tracksConfigured = true;
	}
	return Normalize(p);
}

obs_data_t *RebuildPresetToObsData(const RebuildPreset &p)
{
	obs_data_t *o = obs_data_create();
	obs_data_set_string(o, "id", p.id.c_str());
	obs_data_set_string(o, "name", WideToUtf8(p.name).c_str());
	obs_data_set_int(o, "mode", RebuildModeToInt(p.mode));
	obs_data_set_int(o, "format", RebuildFormatToInt(p.format));
	obs_data_set_int(o, "quality", RebuildQualityToInt(p.quality));
	obs_data_set_int(o, "crf", p.Crf());
	obs_data_set_int(o, "width", p.width);
	obs_data_set_int(o, "height", p.height);
	obs_data_set_double(o, "fps", p.fps);
	obs_data_set_bool(o, "keep_audio", p.keepAudio);
	obs_data_set_int(o, "gif_colors", p.gifColors);
	obs_data_set_int(o, "gif_dither", p.gifDither);
	obs_data_set_int(o, "gif_loop", p.gifLoop);
	obs_data_set_bool(o, "tracks_configured", p.tracksConfigured);
	obs_data_set_bool(o, "keep_video", p.keepVideoTrack);
	obs_data_array_t *tracks = obs_data_array_create();
	for (int ord : p.keepAudioOrdinals) {
		obs_data_t *ti = obs_data_create();
		obs_data_set_int(ti, "i", ord);
		obs_data_array_push_back(tracks, ti);
		obs_data_release(ti);
	}
	obs_data_set_array(o, "keep_audio_ordinals", tracks);
	obs_data_array_release(tracks);
	return o;
}

bool RebuildPreset::ShouldKeepAudioOrdinal(int ordinal) const
{
	if (!tracksConfigured)
		return true;
	for (int o : keepAudioOrdinals) {
		if (o == ordinal)
			return true;
	}
	return false;
}

int RebuildPreset::Crf() const
{
	int v = crf;
	if (v < 18)
		v = 18;
	if (v > 51)
		v = 51;
	return v;
}

void RebuildPreset::SetCrf(int value)
{
	if (value < 18)
		value = 18;
	if (value > 51)
		value = 51;
	crf = value;
	quality = QualityFromCrf(crf);
}

const char *RebuildPreset::ContainerName() const
{
	switch (format) {
	case RebuildOutputFormat::Mov:
		return "mov";
	case RebuildOutputFormat::Gif:
		return "gif";
	case RebuildOutputFormat::Mp4:
	default:
		return "mp4";
	}
}

std::wstring RebuildPreset::FormatLabel() const
{
	switch (format) {
	case RebuildOutputFormat::Mov:
		return L"MOV";
	case RebuildOutputFormat::Gif:
		return L"GIF";
	case RebuildOutputFormat::Mp4:
	default:
		return L"MP4";
	}
}

std::wstring RebuildPreset::QualityLabel() const
{
	if (format == RebuildOutputFormat::Gif) {
		wchar_t buf[32];
		swprintf(buf, 32, L"%d colors", gifColors);
		return buf;
	}
	const int v = Crf();
	const wchar_t *hint = L"Med";
	if (v <= 18)
		hint = L"Max";
	else if (v <= 23)
		hint = L"High";
	else if (v <= 28)
		hint = L"Med";
	else if (v <= 35)
		hint = L"Low";
	else
		hint = L"Min";
	wchar_t buf[48];
	swprintf(buf, 48, L"CRF %d · %s", v, hint);
	return buf;
}

std::wstring RebuildPreset::ResolutionLabel() const
{
	if (width <= 0 && height <= 0)
		return L"Source";
	if (width >= 1920 || height >= 1080)
		return L"1080p";
	if (width >= 1280 || height >= 720)
		return L"720p";
	if (width >= 854 || height >= 480)
		return L"480p";
	wchar_t buf[32];
	swprintf(buf, 32, L"%dx%d", width, height > 0 ? height : 0);
	return buf;
}

std::wstring RebuildPreset::FpsLabel() const
{
	if (fps <= 0.5)
		return L"Source";
	wchar_t buf[16];
	swprintf(buf, 16, L"%.0f", fps);
	return buf;
}

RebuildMode RebuildModeFromInt(int v)
{
	if (v == static_cast<int>(RebuildMode::Copy))
		return RebuildMode::Copy;
	return RebuildMode::Reencode;
}

int RebuildModeToInt(RebuildMode mode)
{
	return static_cast<int>(mode);
}

RebuildOutputFormat RebuildFormatFromInt(int v)
{
	if (v == static_cast<int>(RebuildOutputFormat::Mov))
		return RebuildOutputFormat::Mov;
	if (v == static_cast<int>(RebuildOutputFormat::Gif))
		return RebuildOutputFormat::Gif;
	return RebuildOutputFormat::Mp4;
}

int RebuildFormatToInt(RebuildOutputFormat f)
{
	return static_cast<int>(f);
}

RebuildQuality RebuildQualityFromInt(int v)
{
	if (v <= 0)
		return RebuildQuality::Low;
	if (v == 2)
		return RebuildQuality::High;
	if (v >= 3)
		return RebuildQuality::VeryHigh;
	return RebuildQuality::Medium;
}

int RebuildQualityToInt(RebuildQuality q)
{
	return static_cast<int>(q);
}

std::wstring RebuildModeLabel(RebuildMode mode)
{
	return mode == RebuildMode::Copy ? L"Copy" : L"Reencode";
}

std::string MigrateLegacyCompressToPresetId(bool compress, int qualityIndex)
{
	if (!compress)
		return "seed_copy";
	if (qualityIndex <= 0)
		return "seed_strong";
	if (qualityIndex >= 2)
		return "seed_quality";
	return "seed_balanced";
}

RebuildPreset RebuildPresetStore::MakeSeedStrong()
{
	RebuildPreset p;
	p.id = "seed_strong";
	p.name = L"Strong Compression";
	p.mode = RebuildMode::Reencode;
	p.format = RebuildOutputFormat::Mp4;
	p.SetCrf(32);
	return p;
}

RebuildPreset RebuildPresetStore::MakeSeedBalanced()
{
	RebuildPreset p;
	p.id = "seed_balanced";
	p.name = L"Balanced";
	p.mode = RebuildMode::Reencode;
	p.format = RebuildOutputFormat::Mp4;
	p.SetCrf(28);
	return p;
}

RebuildPreset RebuildPresetStore::MakeSeedQuality()
{
	RebuildPreset p;
	p.id = "seed_quality";
	p.name = L"Quality";
	p.mode = RebuildMode::Reencode;
	p.format = RebuildOutputFormat::Mp4;
	p.SetCrf(23);
	return p;
}

RebuildPreset RebuildPresetStore::MakeSeedCopy()
{
	RebuildPreset p;
	p.id = "seed_copy";
	p.name = L"Copy (Lossless Trim)";
	p.mode = RebuildMode::Copy;
	p.format = RebuildOutputFormat::Mp4;
	p.SetCrf(28);
	return p;
}

std::vector<RebuildPreset> RebuildPresetStore::DefaultSeedPresets()
{
	return {MakeSeedStrong(), MakeSeedBalanced(), MakeSeedQuality(), MakeSeedCopy()};
}

void RebuildPresetStore::EnsureSeeded(obs_data_t *settings)
{
	if (!settings)
		return;
	if (obs_data_get_bool(settings, kSeededKey)) {
		Load(settings);
		if (presets.empty()) {
			presets = DefaultSeedPresets();
			selectedId = "seed_balanced";
			Save(settings);
		}
		return;
	}

	presets = DefaultSeedPresets();
	selectedId = "seed_balanced";

	bool compress = true;
	int quality = 1;
	if (obs_data_has_user_value(settings, "gallery_share_compress"))
		compress = obs_data_get_bool(settings, "gallery_share_compress");
	if (obs_data_has_user_value(settings, "gallery_share_compress_quality"))
		quality = static_cast<int>(obs_data_get_int(settings, "gallery_share_compress_quality"));
	selectedId = MigrateLegacyCompressToPresetId(compress, quality);

	obs_data_set_bool(settings, kSeededKey, true);
	Save(settings);
}

void RebuildPresetStore::Load(obs_data_t *settings)
{
	presets.clear();
	selectedId.clear();
	maxParallel = 1;
	if (!settings)
		return;

	obs_data_array_t *arr = obs_data_get_array(settings, kArrayKey);
	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(arr, i);
			if (!o)
				continue;
			RebuildPreset p = RebuildPresetFromObsData(o);
			if (!p.id.empty())
				presets.push_back(std::move(p));
			obs_data_release(o);
		}
		obs_data_array_release(arr);
	}

	const char *sel = obs_data_get_string(settings, kSelectedKey);
	if (sel)
		selectedId = sel;
	if (obs_data_has_user_value(settings, kMaxParallelKey)) {
		maxParallel = static_cast<int>(obs_data_get_int(settings, kMaxParallelKey));
	}
	if (maxParallel < 1)
		maxParallel = 1;
	if (maxParallel > 4)
		maxParallel = 4;

	if (selectedId.empty() || !FindById(selectedId)) {
		if (!presets.empty())
			selectedId = presets.front().id;
	}
}

void RebuildPresetStore::Save(obs_data_t *settings) const
{
	if (!settings)
		return;
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &p : presets) {
		obs_data_t *o = RebuildPresetToObsData(p);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(settings, kArrayKey, arr);
	obs_data_array_release(arr);
	obs_data_set_string(settings, kSelectedKey, selectedId.c_str());
	obs_data_set_int(settings, kMaxParallelKey, maxParallel);
	obs_data_set_bool(settings, kSeededKey, true);
}

const RebuildPreset *RebuildPresetStore::FindById(const std::string &id) const
{
	for (const auto &p : presets) {
		if (p.id == id)
			return &p;
	}
	return nullptr;
}

RebuildPreset *RebuildPresetStore::FindByIdMutable(const std::string &id)
{
	for (auto &p : presets) {
		if (p.id == id)
			return &p;
	}
	return nullptr;
}

RebuildPreset RebuildPresetStore::GetSelectedOrDefault() const
{
	if (const RebuildPreset *p = FindById(selectedId))
		return *p;
	if (!presets.empty())
		return presets.front();
	return MakeSeedBalanced();
}

bool RebuildPresetStore::Upsert(const RebuildPreset &preset)
{
	if (preset.id.empty())
		return false;
	if (RebuildPreset *existing = FindByIdMutable(preset.id)) {
		*existing = Normalize(preset);
		return true;
	}
	presets.push_back(Normalize(preset));
	return true;
}

bool RebuildPresetStore::Remove(const std::string &id)
{
	auto it = std::remove_if(presets.begin(), presets.end(), [&](const RebuildPreset &p) { return p.id == id; });
	if (it == presets.end())
		return false;
	presets.erase(it, presets.end());
	if (selectedId == id) {
		selectedId = presets.empty() ? "" : presets.front().id;
	}
	return true;
}

std::string RebuildPresetStore::GenerateId() const
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	std::mt19937 rng(static_cast<unsigned>(now));
	std::uniform_int_distribution<int> dist(0, 0xFFFFFF);
	for (int attempt = 0; attempt < 8; attempt++) {
		char buf[32];
		snprintf(buf, sizeof(buf), "preset_%06x", dist(rng));
		if (!FindById(buf))
			return buf;
	}
	return "preset_custom";
}

#endif
