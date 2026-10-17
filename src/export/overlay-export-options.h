#pragma once

#ifdef _WIN32

#include "overlay-rebuild-preset.h"
#include <string>
#include <vector>

struct OverlayExportOptions {
	RebuildMode mode = RebuildMode::Copy;
	RebuildOutputFormat format = RebuildOutputFormat::Mp4;
	int crf = 28;
	int width = 0;
	int height = 0;
	double fps = 0.0;
	bool keepAudio = true;
	std::vector<int> streamIndicesToKeep;
	bool useCpuEncoder = true;

	bool tracksConfigured = false;
	bool keepVideoTrack = true;
	std::vector<int> keepAudioOrdinals;

	int gifColors = 256;
	int gifDither = 1;
	int gifLoop = 0;

	const char *ContainerName() const
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

	bool IsGif() const { return mode == RebuildMode::Reencode && format == RebuildOutputFormat::Gif; }

	static OverlayExportOptions FromPreset(const RebuildPreset &p)
	{
		OverlayExportOptions o;
		o.mode = p.mode;
		o.format = p.format;
		o.crf = p.Crf();
		o.width = p.width;
		o.height = p.height;
		o.fps = p.fps;
		o.keepAudio = p.keepAudio;
		o.gifColors = p.gifColors;
		o.gifDither = p.gifDither;
		o.gifLoop = p.gifLoop;
		o.tracksConfigured = p.tracksConfigured;
		o.keepVideoTrack = p.keepVideoTrack;
		o.keepAudioOrdinals = p.keepAudioOrdinals;
		if (o.IsGif()) {
			if (o.width <= 0)
				o.width = 480;
			if (o.fps <= 0.5)
				o.fps = 15.0;
		}
		return o;
	}
};

struct OverlayExportSegment {
	std::wstring inputPath;
	double startSeconds = 0.0;
	double endSeconds = 0.0;
};

#endif
