#pragma once

#ifdef _WIN32

#include "overlay-export-options.h"
#include "overlay-rebuild-preset.h"
#include "overlay-trimmer.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class ExportQueueMode {
	Separate = 0,
	Merge = 1
};

enum class ExportJobStatus {
	Queued = 0,
	Running = 1,
	Success = 2,
	Failed = 3,
	Cancelled = 4
};

struct ExportQueueJob {
	std::string id;
	std::wstring inputPath;
	std::wstring displayName;
	double startSeconds = 0.0;
	double endSeconds = 0.0;
	RebuildPreset preset;
	std::vector<int> streamIndicesToKeep;
	std::wstring outputPath;
	std::wstring outputFolder;
	ExportJobStatus status = ExportJobStatus::Queued;
	std::wstring errorText;
	double progress = 0.0;
	// 0xRRGGBB timeline/queue marker. 0 = assign on enqueue.
	int markerColorRgb = 0;
};

class OverlayExportQueue {
public:
	using ToastFn = std::function<void(const std::string &utf8)>;

	OverlayExportQueue();
	~OverlayExportQueue();

	void SetMaxParallel(int n);
	int GetMaxParallel() const;
	void SetMode(ExportQueueMode mode);
	ExportQueueMode GetMode() const;
	void SetBatchPreset(const RebuildPreset &preset);
	RebuildPreset GetBatchPreset() const;
	void SetToastHandler(ToastFn fn);
	void SetDefaultExportFolder(const std::wstring &folder);

	std::string Enqueue(const ExportQueueJob &job);
	bool UpdateJob(const std::string &id, const ExportQueueJob &job);
	bool RenameJob(const std::string &id, const std::wstring &displayName);
	bool RemoveJob(const std::string &id);
	void ClearAll();
	bool Reorder(const std::string &id, int newIndex);
	std::vector<ExportQueueJob> GetJobs() const;
	bool IsEmpty() const;

	bool Start();
	void CancelJob(const std::string &id);
	void CancelBatch();
	bool Retry(const std::string &id);
	void ClearCompleted();

	bool IsBusy() const;
	bool HasUnfinished() const;
	bool IsPathLocked(const std::wstring &path) const;
	bool AllOutputsSameFolder(std::wstring &outFolder) const;
	bool HasAnySuccess() const;
	// Overall Merge batch progress while the merge worker is running; false if not merging.
	bool GetMergeProgress(double &outProgress) const;


	void Tick();

	void Load();
	void Save() const;
	void ClearPersistence() const;

private:
	struct WorkerSlot {
		OverlayTrimmer trimmer;
		std::string jobId;
		bool mergeWorker = false;
	};

	void DispatchLocked();
	void HarvestWorkersLocked();
	void PersistLocked() const;
	std::string GenerateJobId() const;
	OverlayExportOptions OptionsForJob(const ExportQueueJob &job) const;
	std::wstring BuildOutputPath(const ExportQueueJob &job, bool merge) const;
	void Toast(const std::string &utf8) const;

	mutable std::mutex m_mutex;
	std::vector<ExportQueueJob> m_jobs;
	ExportQueueMode m_mode = ExportQueueMode::Separate;
	RebuildPreset m_batchPreset = RebuildPresetStore::MakeSeedBalanced();
	int m_maxParallel = 1;
	std::vector<std::unique_ptr<WorkerSlot>> m_workers;
	bool m_batchRunning = false;
	bool m_mergeActive = false;
	ToastFn m_toast;
	std::wstring m_defaultExportFolder;
};

#endif
