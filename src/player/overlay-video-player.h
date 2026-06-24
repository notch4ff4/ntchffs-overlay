#pragma once

#ifdef _WIN32

	// Opaque libav state owned by overlay-video-player.cpp.
	struct LibAvState;
	// Packet queues and A/V sync for demux, video, and audio threads.
	struct ThreeThreadState;

#include <windows.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <memory>
#include <utility>

class OverlayVideoPlayer {
	friend struct ThreeThreadState;
public:
	struct FrameView {
		const uint8_t *data = nullptr;
		int width = 0;
		int height = 0;
		std::unique_lock<std::mutex> lock;
	};

	OverlayVideoPlayer();
	~OverlayVideoPlayer();

	bool Initialize();
	void Shutdown();

	bool Open(const std::wstring &path);
	void Close();

	void SetPlaying(bool playing);
	bool IsPlaying() const { return m_playing; }
	void TogglePlaying();

	void Seek(double seconds);
	// Steps by exactly one frame using 1/fps so scrubbing stays frame-accurate.
	void SeekOneFrameForward();
	void SeekOneFrameBackward();
	double GetDuration() const { return m_duration.load(); }
	double GetCurrentTime() const { return m_currentTime.load(); }
	double GetVideoFps() const { return m_videoFps; }
	bool IsDurationScanActive() const { return false; }
	void SetMuted(bool muted);
	bool IsMuted() const { return m_muted.load(); }
	void SetVolume(double volume);
	double GetVolume() const { return m_volume.load(); }
	void SetTargetSize(int maxWidth, int maxHeight);

	bool Update();
	bool HasFrame() const { return !m_frameData.empty(); }
	bool GetFrameSnapshot(std::vector<uint8_t> &out, int &width, int &height) const;
	// EN: Returns a locked view of the latest frame to avoid an extra buffer copy. The pointer stays valid only while FrameView::lock is held.
	// RU: Возвращает заблокированный вид последнего кадра без лишнего копирования буфера. Указатель действителен только пока удерживается FrameView::lock.
	bool AcquireFrame(FrameView &out) const;

	void SetTrimIn(double seconds) { m_trimIn = seconds; }
	void SetTrimOut(double seconds) { m_trimOut = seconds; }
	double GetTrimIn() const { return m_trimIn; }
	double GetTrimOut() const { return m_trimOut; }

	// --- Audio tracks ---
	struct AudioTrackInfo {
		int streamIndex = -1;
		// Metadata title or generated "Audio N" label.
		std::string name;
		std::string codecName;
		std::string lang;
		int sampleRate = 0;
		int channels = 0;
	};
	int GetAudioTrackCount() const;
	void GetAudioTrackInfo(int index, AudioTrackInfo &out) const;
	int GetCurrentAudioTrackIndex() const;
	void SetCurrentAudioTrack(int index);

	int GetVideoStreamIndex() const;
	// Uses metadata title when present, otherwise "Video".
	std::string GetVideoStreamName() const;

private:
	void UpdateOutputSize();
	void StartPlayback(double startTime);
	void StopPlayback();
	// Legacy single-frame decode path.
	void StartDecodeThread(double startTime, bool singleFrame);
	void StartThreeThreadPlayback(double startTime);
	void StopThreeThreadPlayback();
	void StartAudioThread(int sampleRate, int channels);
	void StopAudioPlayback();

	bool m_initialized;
	bool m_playing;
	std::atomic<double> m_duration;
	std::atomic<double> m_currentTime;
	std::atomic<bool> m_muted;
	std::atomic<double> m_volume;
	double m_trimIn;
	double m_trimOut;
	int m_logFrameCount;
	ULONGLONG m_lastTick;
	std::atomic<ULONGLONG> m_lastFrameTick;
	ULONGLONG m_lastStallLogTick;
	int m_sourceWidth;
	int m_sourceHeight;
	int m_frameWidth;
	int m_frameHeight;
	int m_targetMaxWidth;
	int m_targetMaxHeight;
	double m_videoFps;
	std::vector<uint8_t> m_frameData;
	mutable std::mutex m_frameMutex;
	std::wstring m_openPath;
	std::thread m_decodeThread;
	std::thread m_demuxThread;
	std::thread m_videoThread;
	std::thread m_audioDecodeThread;
	std::thread m_audioThread;
	std::unique_ptr<ThreeThreadState> m_tt;
	std::atomic<bool> m_demuxStop;
	std::atomic<double> m_seekTargetSec;
	std::atomic<bool> m_seekRequested;
	std::queue<std::vector<uint8_t>> m_audioQueue;
	std::mutex m_audioQueueMutex;
	std::condition_variable m_audioQueueCond;
	std::atomic<bool> m_videoStop;
	std::atomic<bool> m_audioStop;
	double m_audioClock;
	double m_playStartTime;
	LibAvState *m_av;
};

#endif // _WIN32
