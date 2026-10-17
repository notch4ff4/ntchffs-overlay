#pragma once

#ifdef _WIN32

#include "overlay-export-options.h"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

class OverlayTrimmer {
public:
	enum class Status {
		Idle,
		Trimming,
		Success,
		Error
	};

	OverlayTrimmer();
	~OverlayTrimmer();

	bool StartTrim(const std::wstring &inputPath, const std::wstring &outputPath,
	               double startSeconds, double endSeconds);
	// --- Export ---
	// EN: Trims in-process via libav; re-encodes to H.264/AAC when compress is enabled. Non-empty streamIndicesToKeep exports only those stream indices; empty keeps all streams. customFfmpegArgs is ignored (legacy API; in-process mode only). useCpuEncoder is reserved for encoder selection when re-encoding.
	// RU: Обрезка в процессе через libav; при включенном сжатии перекодирует в H.264/AAC. Непустой streamIndicesToKeep экспортирует только указанные индексы потоков; пустой — все потоки. customFfmpegArgs игнорируется (устаревший API; только встроенный режим). useCpuEncoder зарезервирован для выбора кодировщика при перекодировании.
	bool StartExport(const std::wstring &inputPath,
	                 const std::wstring &outputPath,
	                 double startSeconds,
	                 double endSeconds,
	                 bool compress,
	                 int crf,
	                 const std::vector<int> &streamIndicesToKeep = {},
	                 const std::wstring &customFfmpegArgs = L"",
	                 bool useCpuEncoder = false);

	// Rebuild export (Copy / Reencode / GIF) for a single segment.
	bool StartRebuildExport(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
				double endSeconds, const OverlayExportOptions &options);

	// Merge: concat segments in order, then apply batch Rebuild options to one output.
	bool StartMergeExport(const std::vector<OverlayExportSegment> &segments, const std::wstring &outputPath,
			      const OverlayExportOptions &options);

	bool StartTranscode(const std::wstring &inputPath, const std::wstring &outputPath,
	                    int crf);
	Status GetStatus() const { return m_status.load(); }
	std::wstring GetLastError() const { return m_lastError; }
	std::wstring GetLastOutput() const { return m_lastOutput; }
	void Reset();
	// Sets a flag read by the worker thread so an in-flight export can stop cooperatively.
	void RequestCancel();
	// Best-effort progress fraction during Trimming; stays at 0 when duration is unknown.
	double GetProgress() const { return m_progress.load(); }
	// Human-readable progress, e.g. "1:23 / 5:00" or "45%".
	std::wstring GetProgressText() const;

private:
	void RunTrim(const std::wstring &inputPath,
	             const std::wstring &outputPath,
	             double startSeconds,
	             double endSeconds);
	void RunExport(const std::wstring &inputPath,
	               const std::wstring &outputPath,
	               double startSeconds,
	               double endSeconds,
	               bool compress,
	               int crf,
	               const std::vector<int> &streamIndicesToKeep,
	               const std::wstring &customFfmpegArgs = L"",
	               bool useCpuEncoder = false);
	void RunRebuildExport(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			      double endSeconds, OverlayExportOptions options);
	void RunMergeExport(std::vector<OverlayExportSegment> segments, std::wstring outputPath,
			    OverlayExportOptions options);
	void RunTranscode(const std::wstring &inputPath,
	                  const std::wstring &outputPath,
	                  int crf);

	bool BeginWorker();

	std::atomic<Status> m_status;
	std::atomic<double> m_progress;
	std::atomic<bool> m_cancelRequested;
	double m_exportTotalDuration;
	std::wstring m_lastError;
	std::wstring m_lastOutput;
	std::thread m_worker;
};

// Lossless stream-copy trim used by smart replay overlap removal.
bool overlay_replay_lossless_trim(const std::wstring &inputPath, const std::wstring &outputPath,
				  double startSeconds, std::wstring &outError);
// Reads playback range in seconds for overlap trim calculations.
bool overlay_replay_probe_playback_range(const std::wstring &inputPath, double &outStartSeconds,
					 double &outEndSeconds, std::wstring &outError,
					 int *outRefStreamIndex = nullptr, int *outVideoPacketCount = nullptr,
					 double *outContainerDurationSeconds = nullptr);
// Keeps the last keepSeconds of media via stream-copy trim.
bool overlay_replay_lossless_trim_keep_last(const std::wstring &inputPath, const std::wstring &outputPath,
					    double keepSeconds, std::wstring &outError);

// Synchronous helpers used by the export queue (worker already owns the thread).
bool overlay_export_run(const std::wstring &inputPath, const std::wstring &outputPath, double startSeconds,
			double endSeconds, const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			std::atomic<double> &progress, std::wstring &outError);
bool overlay_export_merge_run(const std::vector<OverlayExportSegment> &segments, const std::wstring &outputPath,
			      const OverlayExportOptions &options, std::atomic<bool> &cancelRequested,
			      std::atomic<double> &progress, std::wstring &outError);

#endif // _WIN32
