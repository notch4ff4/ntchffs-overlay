#pragma once

#ifdef _WIN32

#include <string>

struct OverlayState {
	bool recordingActive;
	bool replayActive;
	bool replaySaving;
	
	// --- Stats ---
	double fps;
	uint32_t droppedFrames;
	std::string freeSpaceText;
	
	// --- Visibility ---
	bool statsVisible;

	// Gates folder/gallery actions when OBS has no recording output path configured.
	bool recordingPathConfigured;
	
	OverlayState() : recordingActive(false), replayActive(false), replaySaving(false),
	                 fps(0.0), droppedFrames(0), freeSpaceText("Free: -"),
	                 statsVisible(false), recordingPathConfigured(false) {}
};

class OverlayStateManager {
public:
	OverlayStateManager();
	
	void UpdateStatus(OverlayState &state);
	void UpdateRecordingStatus(OverlayState &state);
	void UpdateReplayStatus(OverlayState &state);
	void UpdateStats(OverlayState &state);
	
	void ToggleRecording();
	void ToggleReplayBuffer();
	void SaveReplay();
	static void SetReplaySaving(bool saving);
	static bool GetReplaySaving();
	
	std::string GetConfiguredRecordingPath();
	std::string GetRecordingPath();
	bool HasRecordingPath();
	
private:
	std::string GetFreeSpaceText(const std::string &path);
};

#endif // _WIN32
