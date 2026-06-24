#pragma once

#ifdef _WIN32

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
	// RU: Обрезка в процессе через libav; при включённом сжатии перекодирует в H.264/AAC. Непустой streamIndicesToKeep экспортирует только указанные индексы потоков; пустой — все потоки. customFfmpegArgs игнорируется (устаревший API; только встроенный режим). useCpuEncoder зарезервирован для выбора кодировщика при перекодировании.
	bool StartExport(const std::wstring &inputPath,
	                 const std::wstring &outputPath,
	                 double startSeconds,
	                 double endSeconds,
	                 bool compress,
	                 int crf,
	                 const std::vector<int> &streamIndicesToKeep = {},
	                 const std::wstring &customFfmpegArgs = L"",
	                 bool useCpuEncoder = false);
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
	void RunTranscode(const std::wstring &inputPath,
	                  const std::wstring &outputPath,
	                  int crf);

	std::atomic<Status> m_status;
	std::atomic<double> m_progress;
	std::atomic<bool> m_cancelRequested;
	double m_exportTotalDuration;
	std::wstring m_lastError;
	std::wstring m_lastOutput;
	std::thread m_worker;
};

#endif // _WIN32
