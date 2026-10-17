#ifdef _WIN32

#include "overlay-export-queue.h"
#include "overlay-string-utils.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using overlay::util::Utf8ToWide;
using overlay::util::WideToUtf8;

constexpr const char *kQueueFile = "export_queue.json";

std::wstring ParentFolder(const std::wstring &path)
{
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos)
		return L"";
	return path.substr(0, pos);
}

std::wstring FileStem(const std::wstring &path)
{
	size_t slash = path.find_last_of(L"\\/");
	std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	size_t dot = name.find_last_of(L'.');
	if (dot != std::wstring::npos)
		name = name.substr(0, dot);
	return name;
}

std::wstring ExtensionForPreset(const RebuildPreset &p)
{
	if (p.mode == RebuildMode::Copy)
		return L""; // keep source extension
	switch (p.format) {
	case RebuildOutputFormat::Gif:
		return L".gif";
	case RebuildOutputFormat::Mov:
		return L".mov";
	case RebuildOutputFormat::Mp4:
	default:
		return L".mp4";
	}
}

std::wstring SourceExtension(const std::wstring &path)
{
	size_t dot = path.find_last_of(L'.');
	if (dot == std::wstring::npos)
		return L".mp4";
	return path.substr(dot);
}

std::wstring TimestampSuffix()
{
	SYSTEMTIME st{};
	GetLocalTime(&st);
	wchar_t buf[64];
	swprintf(buf, 64, L"_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
		 st.wSecond);
	return buf;
}

// Colors for queue markers.
static const int kMarkerPalette[] = {
	0xE67E22, // orange
	0x3498DB, // blue
	0x2ECC71, // green
	0xE74C3C, // red
	0x9B59B6, // purple
	0xF1C40F, // yellow
	0x1ABC9C, // teal
	0xE91E63, // pink
};

static int AllocateMarkerColor(const std::vector<ExportQueueJob> &jobs)
{
	constexpr int n = static_cast<int>(sizeof(kMarkerPalette) / sizeof(kMarkerPalette[0]));
	int counts[n] = {};
	for (const auto &j : jobs) {
		if (j.markerColorRgb == 0)
			continue;
		for (int i = 0; i < n; i++) {
			if (j.markerColorRgb == kMarkerPalette[i]) {
				counts[i]++;
				break;
			}
		}
	}
	int best = 0;
	for (int i = 1; i < n; i++) {
		if (counts[i] < counts[best])
			best = i;
	}
	return kMarkerPalette[best];
}

int StatusToInt(ExportJobStatus s)
{
	return static_cast<int>(s);
}

ExportJobStatus StatusFromInt(int v)
{
	switch (v) {
	case 1:
		return ExportJobStatus::Running;
	case 2:
		return ExportJobStatus::Success;
	case 3:
		return ExportJobStatus::Failed;
	case 4:
		return ExportJobStatus::Cancelled;
	default:
		return ExportJobStatus::Queued;
	}
}

obs_data_t *PresetToData(const RebuildPreset &p)
{
	return RebuildPresetToObsData(p);
}

RebuildPreset PresetFromData(obs_data_t *o)
{
	if (!o)
		return RebuildPresetStore::MakeSeedBalanced();
	RebuildPreset p = RebuildPresetFromObsData(o);
	if (p.id.empty())
		p = RebuildPresetStore::MakeSeedBalanced();
	return p;
}

} // namespace

OverlayExportQueue::OverlayExportQueue() = default;

OverlayExportQueue::~OverlayExportQueue()
{
	CancelBatch();
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &w : m_workers) {
		if (w && w->trimmer.GetStatus() == OverlayTrimmer::Status::Trimming)
			w->trimmer.RequestCancel();
	}

	for (auto &w : m_workers) {
		if (w)
			w->trimmer.Reset();
	}
	m_workers.clear();
}

void OverlayExportQueue::SetMaxParallel(int n)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_maxParallel = std::clamp(n, 1, 4);
}

int OverlayExportQueue::GetMaxParallel() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_maxParallel;
}

void OverlayExportQueue::SetMode(ExportQueueMode mode)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_batchRunning)
		return;
	m_mode = mode;
	PersistLocked();
}

ExportQueueMode OverlayExportQueue::GetMode() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_mode;
}

void OverlayExportQueue::SetBatchPreset(const RebuildPreset &preset)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_batchPreset = preset;
	PersistLocked();
}

RebuildPreset OverlayExportQueue::GetBatchPreset() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_batchPreset;
}

void OverlayExportQueue::SetToastHandler(ToastFn fn)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_toast = std::move(fn);
}

void OverlayExportQueue::SetDefaultExportFolder(const std::wstring &folder)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_defaultExportFolder = folder;
}

std::string OverlayExportQueue::GenerateJobId() const
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	char buf[40];
	snprintf(buf, sizeof(buf), "job_%lld", static_cast<long long>(now));
	std::string id = buf;
	int n = 0;
	while (true) {
		bool exists = false;
		for (const auto &j : m_jobs) {
			if (j.id == id) {
				exists = true;
				break;
			}
		}
		if (!exists)
			return id;
		snprintf(buf, sizeof(buf), "job_%lld_%d", static_cast<long long>(now), ++n);
		id = buf;
	}
}

std::string OverlayExportQueue::Enqueue(const ExportQueueJob &job)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	ExportQueueJob j = job;
	if (j.id.empty())
		j.id = GenerateJobId();
	if (j.displayName.empty())
		j.displayName = FileStem(j.inputPath);
	if (j.markerColorRgb == 0)
		j.markerColorRgb = AllocateMarkerColor(m_jobs);
	j.status = ExportJobStatus::Queued;
	j.errorText.clear();
	j.progress = 0.0;
	m_jobs.push_back(std::move(j));
	PersistLocked();
	return m_jobs.back().id;
}

bool OverlayExportQueue::UpdateJob(const std::string &id, const ExportQueueJob &job)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &j : m_jobs) {
		if (j.id != id)
			continue;
		if (j.status == ExportJobStatus::Running)
			return false;
		const ExportJobStatus keepStatus = j.status;
		const std::wstring keepError = j.errorText;
		const double keepProgress = j.progress;
		const std::wstring keepOut = j.outputPath;
		const std::wstring keepFolder = j.outputFolder;
		const int keepColor = j.markerColorRgb;
		j = job;
		j.id = id;
		if (j.markerColorRgb == 0)
			j.markerColorRgb = keepColor != 0 ? keepColor : AllocateMarkerColor(m_jobs);
		if (keepStatus == ExportJobStatus::Success) {
			j.status = keepStatus;
			j.outputPath = keepOut;
			j.outputFolder = keepFolder;
			j.progress = keepProgress;
		} else {
			j.status = ExportJobStatus::Queued;
			j.errorText.clear();
			j.progress = 0.0;
			j.outputPath.clear();
			j.outputFolder.clear();
		}
		(void)keepError;
		PersistLocked();
		return true;
	}
	return false;
}

bool OverlayExportQueue::RenameJob(const std::string &id, const std::wstring &displayName)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::wstring name = displayName;
	while (!name.empty() && (name.front() == L' ' || name.front() == L'\t'))
		name.erase(name.begin());
	while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
		name.pop_back();
	if (name.empty())
		return false;
	for (auto &j : m_jobs) {
		if (j.id != id)
			continue;
		if (j.status == ExportJobStatus::Running)
			return false;
		j.displayName = name;
		PersistLocked();
		return true;
	}
	return false;
}

bool OverlayExportQueue::RemoveJob(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	bool found = false;
	for (auto &w : m_workers) {
		if (w && w->jobId == id) {
			w->trimmer.RequestCancel();
			found = true;
		}
	}
	auto it = std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const ExportQueueJob &j) {
		if (j.id != id)
			return false;
		found = true;
		return true;
	});
	if (!found)
		return false;
	m_jobs.erase(it, m_jobs.end());
	if (m_jobs.empty()) {
		m_batchRunning = false;
		ClearPersistence();
	} else {
		PersistLocked();
	}
	return true;
}

void OverlayExportQueue::ClearAll()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &w : m_workers) {
		if (w)
			w->trimmer.RequestCancel();
	}
	m_jobs.clear();
	m_batchRunning = false;
	m_mergeActive = false;
	ClearPersistence();
}

bool OverlayExportQueue::IsEmpty() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_jobs.empty();
}

bool OverlayExportQueue::Reorder(const std::string &id, int newIndex)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_batchRunning)
		return false;
	int oldIndex = -1;
	for (int i = 0; i < static_cast<int>(m_jobs.size()); i++) {
		if (m_jobs[static_cast<size_t>(i)].id == id) {
			oldIndex = i;
			break;
		}
	}
	if (oldIndex < 0)
		return false;
	newIndex = std::clamp(newIndex, 0, static_cast<int>(m_jobs.size()) - 1);
	if (newIndex == oldIndex)
		return true;
	ExportQueueJob job = m_jobs[static_cast<size_t>(oldIndex)];
	m_jobs.erase(m_jobs.begin() + oldIndex);
	m_jobs.insert(m_jobs.begin() + newIndex, std::move(job));
	PersistLocked();
	return true;
}

std::vector<ExportQueueJob> OverlayExportQueue::GetJobs() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_jobs;
}

OverlayExportOptions OverlayExportQueue::OptionsForJob(const ExportQueueJob &job) const
{
	(void)job;

	OverlayExportOptions o = OverlayExportOptions::FromPreset(m_batchPreset);
	o.useCpuEncoder = true;
	return o;
}

static std::wstring SanitizeOutputStem(std::wstring name)
{
	while (!name.empty() && (name.front() == L' ' || name.front() == L'\t'))
		name.erase(name.begin());
	while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
		name.pop_back();

	const size_t slash = name.find_last_of(L"\\/");
	const size_t dot = name.find_last_of(L'.');
	if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) &&
	    dot + 1 < name.size() && name.size() - dot <= 5)
		name = name.substr(0, dot);
	for (wchar_t &c : name) {
		if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
		    c == L'|' || c == L'?' || c == L'*')
			c = L'_';
	}
	while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
		name.pop_back();
	return name;
}

std::wstring OverlayExportQueue::BuildOutputPath(const ExportQueueJob &job, bool merge) const
{
	std::wstring folder = m_defaultExportFolder;
	if (folder.empty())
		folder = ParentFolder(job.inputPath);
	if (!folder.empty())
		CreateDirectoryW(folder.c_str(), nullptr);

	const RebuildPreset &p = m_batchPreset;
	std::wstring ext = ExtensionForPreset(p);
	if (ext.empty())
		ext = SourceExtension(job.inputPath);
	std::wstring base;
	if (merge) {
		base = L"merge";
	} else {
		base = SanitizeOutputStem(job.displayName);
		if (base.empty())
			base = FileStem(job.inputPath);
	}
	return folder + L"\\" + base + L"_share" + TimestampSuffix() + ext;
}

bool OverlayExportQueue::Start()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_batchRunning || !m_workers.empty() || m_mergeActive)
		return false;
	if (m_jobs.empty())
		return false;

	for (auto &j : m_jobs) {
		j.status = ExportJobStatus::Queued;
		j.errorText.clear();
		j.progress = 0.0;
		j.outputPath.clear();
		j.outputFolder.clear();
	}
	m_batchRunning = true;
	Toast("Export queue started");
	DispatchLocked();
	PersistLocked();
	return true;
}

void OverlayExportQueue::CancelJob(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == ExportQueueMode::Merge) {
		for (auto &w : m_workers) {
			if (w)
				w->trimmer.RequestCancel();
		}
		for (auto &j : m_jobs) {
			if (j.status == ExportJobStatus::Queued) {
				j.status = ExportJobStatus::Cancelled;
				j.errorText = L"Cancelled";
			}
		}
		PersistLocked();
		return;
	}
	for (auto &w : m_workers) {
		if (w && w->jobId == id)
			w->trimmer.RequestCancel();
	}
	for (auto &j : m_jobs) {
		if (j.id == id && j.status == ExportJobStatus::Queued) {
			j.status = ExportJobStatus::Cancelled;
			j.errorText = L"Cancelled";
		}
	}
	PersistLocked();
}

void OverlayExportQueue::CancelBatch()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &w : m_workers) {
		if (w)
			w->trimmer.RequestCancel();
	}
	for (auto &j : m_jobs) {
		if (j.status == ExportJobStatus::Queued) {
			j.status = ExportJobStatus::Cancelled;
			j.errorText = L"Cancelled";
		}
	}
	PersistLocked();
}

bool OverlayExportQueue::Retry(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &j : m_jobs) {
		if (j.id != id)
			continue;
		if (j.status != ExportJobStatus::Failed && j.status != ExportJobStatus::Cancelled)
			return false;
		j.status = ExportJobStatus::Queued;
		j.errorText.clear();
		j.progress = 0.0;
		m_batchRunning = true;
		DispatchLocked();
		PersistLocked();
		Toast("Retrying export: " + WideToUtf8(j.displayName));
		return true;
	}
	return false;
}

void OverlayExportQueue::ClearCompleted()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
				    [](const ExportQueueJob &j) {
					    return j.status == ExportJobStatus::Success ||
						   j.status == ExportJobStatus::Cancelled;
				    }),
		     m_jobs.end());
	PersistLocked();
}

bool OverlayExportQueue::IsBusy() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mergeActive)
		return true;
	for (const auto &w : m_workers) {
		if (w && w->trimmer.GetStatus() == OverlayTrimmer::Status::Trimming)
			return true;
	}
	return false;
}

bool OverlayExportQueue::HasUnfinished() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto &j : m_jobs) {
		if (j.status == ExportJobStatus::Queued || j.status == ExportJobStatus::Running ||
		    j.status == ExportJobStatus::Failed)
			return true;
	}
	return false;
}

bool OverlayExportQueue::IsPathLocked(const std::wstring &path) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto &j : m_jobs) {
		if (j.inputPath != path)
			continue;
		if (j.status == ExportJobStatus::Queued || j.status == ExportJobStatus::Running)
			return true;
	}
	return false;
}

bool OverlayExportQueue::AllOutputsSameFolder(std::wstring &outFolder) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	outFolder.clear();
	bool any = false;
	for (const auto &j : m_jobs) {
		if (j.status != ExportJobStatus::Success || j.outputFolder.empty())
			continue;
		if (!any) {
			outFolder = j.outputFolder;
			any = true;
		} else if (_wcsicmp(outFolder.c_str(), j.outputFolder.c_str()) != 0) {
			outFolder.clear();
			return false;
		}
	}
	return any;
}

bool OverlayExportQueue::HasAnySuccess() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto &j : m_jobs) {
		if (j.status == ExportJobStatus::Success)
			return true;
	}
	return false;
}

bool OverlayExportQueue::GetMergeProgress(double &outProgress) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_mergeActive && m_mode != ExportQueueMode::Merge)
		return false;
	for (const auto &w : m_workers) {
		if (!w || !w->mergeWorker)
			continue;
		if (w->trimmer.GetStatus() == OverlayTrimmer::Status::Trimming) {
			outProgress = w->trimmer.GetProgress();
			return true;
		}
	}
	return false;
}

void OverlayExportQueue::Toast(const std::string &utf8) const
{
	if (m_toast)
		m_toast(utf8);
}

void OverlayExportQueue::HarvestWorkersLocked()
{
	for (auto it = m_workers.begin(); it != m_workers.end();) {
		if (!*it) {
			it = m_workers.erase(it);
			continue;
		}
		WorkerSlot &slot = **it;
		const auto st = slot.trimmer.GetStatus();
		if (st == OverlayTrimmer::Status::Trimming) {
			if (slot.mergeWorker) {
				// Merge progress is exposed via GetMergeProgress(); keep rows status-only.
				for (auto &j : m_jobs) {
					if (j.status == ExportJobStatus::Running || j.id == slot.jobId)
						j.status = ExportJobStatus::Running;
				}
			} else {
				for (auto &j : m_jobs) {
					if (j.id == slot.jobId) {
						j.progress = slot.trimmer.GetProgress();
						j.status = ExportJobStatus::Running;
						break;
					}
				}
			}
			++it;
			continue;
		}

		std::wstring err = slot.trimmer.GetLastError();
		std::wstring out = slot.trimmer.GetLastOutput();
		const bool merge = slot.mergeWorker;
		const std::string jobId = slot.jobId;

		if (st == OverlayTrimmer::Status::Success) {
			if (merge) {
				for (auto &j : m_jobs) {
					if (j.status == ExportJobStatus::Running || j.status == ExportJobStatus::Queued) {
						j.status = ExportJobStatus::Success;
						j.progress = 1.0;
						j.outputPath = out;
						j.outputFolder = ParentFolder(out);
						j.errorText.clear();
					}
				}
				m_mergeActive = false;
				Toast("Merge export finished");
			} else {
				for (auto &j : m_jobs) {
					if (j.id == jobId) {
						j.status = ExportJobStatus::Success;
						j.progress = 1.0;
						j.outputPath = out;
						j.outputFolder = ParentFolder(out);
						j.errorText.clear();
						Toast("Export done: " + WideToUtf8(j.displayName));
						break;
					}
				}
			}
		} else if (st == OverlayTrimmer::Status::Error) {
			const bool cancelled = (err == L"Cancelled");
			if (merge) {
				for (auto &j : m_jobs) {
					if (j.status == ExportJobStatus::Running || j.status == ExportJobStatus::Queued) {
						j.status = cancelled ? ExportJobStatus::Cancelled : ExportJobStatus::Failed;
						j.errorText = err.empty() ? L"Merge failed" : err;
					}
				}
				m_mergeActive = false;
				Toast(cancelled ? "Merge export cancelled"
						: ("Merge export failed: " + WideToUtf8(err)));
			} else {
				for (auto &j : m_jobs) {
					if (j.id == jobId) {
						j.status = cancelled ? ExportJobStatus::Cancelled : ExportJobStatus::Failed;
						j.errorText = err.empty() ? L"Export failed" : err;
						Toast(cancelled ? ("Export cancelled: " + WideToUtf8(j.displayName))
								: ("Export failed: " + WideToUtf8(j.displayName) +
								   " — " + WideToUtf8(j.errorText)));
						break;
					}
				}
			}
		}

		slot.trimmer.Reset();
		it = m_workers.erase(it);
	}

	const bool anyActive = m_mergeActive || !m_workers.empty();
	if (!anyActive) {
		bool pending = false;
		for (const auto &j : m_jobs) {
			if (j.status == ExportJobStatus::Queued) {
				pending = true;
				break;
			}
		}
		if (!pending)
			m_batchRunning = false;
	}
}

void OverlayExportQueue::DispatchLocked()
{
	if (!m_batchRunning)
		return;

	if (m_mode == ExportQueueMode::Merge) {
		if (m_mergeActive || !m_workers.empty())
			return;
		std::vector<OverlayExportSegment> segments;
		std::string firstId;
		for (auto &j : m_jobs) {
			if (j.status != ExportJobStatus::Queued)
				continue;
			if (firstId.empty())
				firstId = j.id;
			OverlayExportSegment seg;
			seg.inputPath = j.inputPath;
			seg.startSeconds = j.startSeconds;
			seg.endSeconds = j.endSeconds;
			segments.push_back(seg);
			j.status = ExportJobStatus::Running;
			j.progress = 0.0;
		}
		if (segments.empty() || firstId.empty()) {
			m_batchRunning = false;
			return;
		}
		ExportQueueJob dummy;
		dummy.inputPath = segments.front().inputPath;
		dummy.preset = m_batchPreset;
		std::wstring outPath = BuildOutputPath(dummy, true);
		auto slot = std::make_unique<WorkerSlot>();
		slot->jobId = firstId;
		slot->mergeWorker = true;
		OverlayExportOptions opts = OverlayExportOptions::FromPreset(m_batchPreset);
		opts.useCpuEncoder = true;
		if (!slot->trimmer.StartMergeExport(segments, outPath, opts)) {
			for (auto &j : m_jobs) {
				if (j.status == ExportJobStatus::Running) {
					j.status = ExportJobStatus::Failed;
					j.errorText = L"Could not start merge worker";
				}
			}
			m_batchRunning = false;
			Toast("Merge export failed to start");
			return;
		}
		m_mergeActive = true;
		m_workers.push_back(std::move(slot));
		return;
	}

	int running = static_cast<int>(m_workers.size());
	while (running < m_maxParallel) {
		ExportQueueJob *next = nullptr;
		for (auto &j : m_jobs) {
			if (j.status == ExportJobStatus::Queued) {
				next = &j;
				break;
			}
		}
		if (!next)
			break;

		std::wstring outPath = BuildOutputPath(*next, false);
		auto slot = std::make_unique<WorkerSlot>();
		slot->jobId = next->id;
		slot->mergeWorker = false;
		OverlayExportOptions opts = OptionsForJob(*next);
		if (!slot->trimmer.StartRebuildExport(next->inputPath, outPath, next->startSeconds, next->endSeconds,
						      opts)) {
			next->status = ExportJobStatus::Failed;
			next->errorText = L"Could not start export worker";
			Toast("Export failed to start: " + WideToUtf8(next->displayName));
			break;
		}
		next->status = ExportJobStatus::Running;
		next->outputPath = outPath;
		next->outputFolder = ParentFolder(outPath);
		m_workers.push_back(std::move(slot));
		running++;
	}
}

void OverlayExportQueue::Tick()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	HarvestWorkersLocked();
	DispatchLocked();
	PersistLocked();
}

void OverlayExportQueue::PersistLocked() const
{
	if (m_jobs.empty()) {
		ClearPersistence();
		return;
	}

	char *path = obs_module_config_path(kQueueFile);
	if (!path)
		return;

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "mode", static_cast<int>(m_mode));
	obs_data_set_int(root, "max_parallel", m_maxParallel);
	obs_data_t *batch = PresetToData(m_batchPreset);
	obs_data_set_obj(root, "batch_preset", batch);
	obs_data_release(batch);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &j : m_jobs) {
		ExportJobStatus st = j.status;
		if (st == ExportJobStatus::Running)
			st = ExportJobStatus::Queued;

		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", j.id.c_str());
		obs_data_set_string(o, "input", WideToUtf8(j.inputPath).c_str());
		obs_data_set_string(o, "name", WideToUtf8(j.displayName).c_str());
		obs_data_set_double(o, "start", j.startSeconds);
		obs_data_set_double(o, "end", j.endSeconds);
		obs_data_set_int(o, "marker_color", j.markerColorRgb);
		obs_data_set_int(o, "status", StatusToInt(st));
		obs_data_set_string(o, "error", WideToUtf8(j.errorText).c_str());
		obs_data_set_string(o, "output", WideToUtf8(j.outputPath).c_str());
		obs_data_set_string(o, "output_folder", WideToUtf8(j.outputFolder).c_str());
		obs_data_t *preset = PresetToData(j.preset);
		obs_data_set_obj(o, "preset", preset);
		obs_data_release(preset);
		obs_data_array_t *tracks = obs_data_array_create();
		for (int idx : j.streamIndicesToKeep) {
			obs_data_t *t = obs_data_create();
			obs_data_set_int(t, "i", idx);
			obs_data_array_push_back(tracks, t);
			obs_data_release(t);
		}
		obs_data_set_array(o, "tracks", tracks);
		obs_data_array_release(tracks);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(root, "jobs", arr);
	obs_data_array_release(arr);

	obs_data_save_json_safe(root, path, "tmp", "bak");
	obs_data_release(root);
	bfree(path);
}

void OverlayExportQueue::Save() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	PersistLocked();
}

void OverlayExportQueue::Load()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_jobs.clear();
	m_workers.clear();
	m_batchRunning = false;
	m_mergeActive = false;

	char *path = obs_module_config_path(kQueueFile);
	if (!path)
		return;
	obs_data_t *root = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!root)
		return;

	m_mode = (obs_data_get_int(root, "mode") == 1) ? ExportQueueMode::Merge : ExportQueueMode::Separate;
	m_maxParallel = static_cast<int>(obs_data_get_int(root, "max_parallel"));
	if (m_maxParallel < 1)
		m_maxParallel = 1;
	if (m_maxParallel > 4)
		m_maxParallel = 4;
	obs_data_t *batch = obs_data_get_obj(root, "batch_preset");
	if (batch) {
		m_batchPreset = PresetFromData(batch);
		obs_data_release(batch);
	}

	obs_data_array_t *arr = obs_data_get_array(root, "jobs");
	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(arr, i);
			if (!o)
				continue;
			ExportQueueJob j;
			j.id = obs_data_get_string(o, "id");
			j.inputPath = Utf8ToWide(obs_data_get_string(o, "input"));
			j.displayName = Utf8ToWide(obs_data_get_string(o, "name"));
			j.startSeconds = obs_data_get_double(o, "start");
			j.endSeconds = obs_data_get_double(o, "end");
			j.markerColorRgb = static_cast<int>(obs_data_get_int(o, "marker_color"));
			j.status = StatusFromInt(static_cast<int>(obs_data_get_int(o, "status")));
			if (j.status == ExportJobStatus::Running)
				j.status = ExportJobStatus::Queued;
			j.errorText = Utf8ToWide(obs_data_get_string(o, "error"));
			j.outputPath = Utf8ToWide(obs_data_get_string(o, "output"));
			j.outputFolder = Utf8ToWide(obs_data_get_string(o, "output_folder"));
			obs_data_t *preset = obs_data_get_obj(o, "preset");
			if (preset) {
				j.preset = PresetFromData(preset);
				obs_data_release(preset);
			}
			obs_data_array_t *tracks = obs_data_get_array(o, "tracks");
			if (tracks) {
				const size_t tn = obs_data_array_count(tracks);
				for (size_t t = 0; t < tn; t++) {
					obs_data_t *ti = obs_data_array_item(tracks, t);
					if (ti) {
						j.streamIndicesToKeep.push_back(static_cast<int>(obs_data_get_int(ti, "i")));
						obs_data_release(ti);
					}
				}
				obs_data_array_release(tracks);
			}
			if (j.markerColorRgb == 0)
				j.markerColorRgb = AllocateMarkerColor(m_jobs);
			m_jobs.push_back(std::move(j));
			obs_data_release(o);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(root);
}

void OverlayExportQueue::ClearPersistence() const
{
	char *path = obs_module_config_path(kQueueFile);
	if (!path)
		return;
	DeleteFileA(path);
	bfree(path);
}

#endif
