#ifdef _WIN32

#include "overlay-renderer.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#ifdef ENABLE_QT
#include <QMetaObject>
#include <QWidget>
#endif

bool OverlayRenderer::IsPointInRect(const POINT &pt, const RECT &rect) const
{
	return pt.x >= rect.left && pt.x <= rect.right && pt.y >= rect.top && pt.y <= rect.bottom;
}

void OverlayRenderer::HandleClick(int x, int y)
{
	m_lastInteractionTick = GetTickCount64();
	if (m_settingsOpen) {
		HandleSettingsClick(x, y);
		return;
	}
	if (m_galleryOpen) {
		if (m_suppressNextGalleryClick) {
			m_suppressNextGalleryClick = false;
			return;
		}
		HandleGalleryClick(x, y);
		return;
	}
	POINT pt = ClientToLayoutPoint(x, y);

	if (IsPointInRect(pt, m_layout.recordingPanelRect)) {
		m_stateManager.ToggleRecording();
		UpdateStatus();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_layout.replayPanelRect)) {
		if (m_state.replayActive) {
			int localY = pt.y - m_layout.replayPanelRect.top;
			int half = (m_layout.replayPanelRect.bottom - m_layout.replayPanelRect.top) / 2;
			if (localY < half) {
				m_stateManager.ToggleReplayBuffer();
			} else if (!m_state.replaySaving) {
				m_stateManager.SaveReplay();
			}
		} else {
			m_stateManager.ToggleReplayBuffer();
		}
		UpdateStatus();
		Render();
		return;
	}

	if (IsPointInRect(pt, m_layout.recordFolderButtonRect)) {
		if (!m_state.recordingPathConfigured) {
			return;
		}
		if (m_galleryEnabled) {
			OpenGallery();
		} else {
			std::string path = m_stateManager.GetConfiguredRecordingPath();
			if (!path.empty()) {
				ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWDEFAULT);
			}
			Hide();
		}
		return;
	}

	if (IsPointInRect(pt, m_layout.showOBSButtonRect)) {
		try {
#ifdef ENABLE_QT
			QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
			if (mainWindow) {
				if (!QMetaObject::invokeMethod(mainWindow, "ToggleShowHide", Qt::QueuedConnection)) {
					const bool isVisible = mainWindow->isVisible();
					const bool isMinimized = mainWindow->isMinimized();
					if (isVisible && !isMinimized) {
						mainWindow->hide();
					} else {
						mainWindow->show();
						mainWindow->raise();
						mainWindow->activateWindow();
						HWND obsHwnd = (HWND)mainWindow->winId();
						if (obsHwnd && IsWindow(obsHwnd)) {
							ShowWindow(obsHwnd, SW_RESTORE);
							SetForegroundWindow(obsHwnd);
						}
					}
				}
			}
#else
			void *mainWindow = obs_frontend_get_main_window();
			if (mainWindow) {
				HWND obsHwnd = (HWND)mainWindow;
				if (obsHwnd && IsWindow(obsHwnd)) {
					ShowWindow(obsHwnd, SW_RESTORE);
					SetForegroundWindow(obsHwnd);
				}
			}
#endif
		} catch (...) {
			obs_log(LOG_ERROR, "Failed to show OBS window");
		}
		Hide();
		return;
	}

	if (IsPointInRect(pt, m_layout.settingsButtonRect)) {
		OpenSettings();
		return;
	}

	if (IsPointInRect(pt, m_layout.statsButtonRect)) {
		m_state.statsVisible = !m_state.statsVisible;
		ApplyPosition();
		return;
	}
}

void OverlayRenderer::HandleMouseDown(int x, int y)
{
	m_lastInteractionTick = GetTickCount64();
	if (m_galleryOpen) {
		POINT pt = ClientToLayoutPoint(x, y);
		// Popups must consume mouse down so timeline drag does not start behind them.
		if (m_galleryAudioTrackSubmenuOpen || m_galleryExportSubmenuOpen) {
			// WM_LBUTTONDOWN also invokes Click; suppress the duplicate gallery action.
			m_suppressNextGalleryClick = true;
			HandleGalleryClick(x, y);
			return;
		}
		if (IsPointInRect(pt, m_galleryTimelineRect)) {
			m_timelineDragging = true;
			HandleGalleryClick(x, y);
			return;
		}
		m_dragCandidate = HandleGalleryMouseDown(x, y);
		m_dragStart = ClientToLayoutPoint(x, y);
	}
}

void OverlayRenderer::HandleMouseUp(int x, int y)
{
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	m_dragCandidate = false;
	m_dragActive = false;
	m_timelineDragging = false;
}

void OverlayRenderer::HandleMouseMove(int x, int y)
{
	m_lastInteractionTick = GetTickCount64();
	if (m_settingsOpen) {
		POINT spt = ClientToLayoutPoint(x, y);
		int idx = SettingsHoverIndexAt(spt);
		if (idx != m_settingsHoverIndex) {
			m_settingsHoverIndex = idx;
			Render();
		}
		return;
	}
	if (m_galleryOpen && m_timelineDragging) {
		HandleGalleryClick(x, y);
		return;
	}
	if (m_galleryOpen && m_dragCandidate && !m_dragActive) {
		MaybeStartDrag(x, y);
	}
	POINT pt = ClientToLayoutPoint(x, y);
	HoverTarget target = GetHoverTarget(pt);
	if (target != m_hoverTarget) {
		m_hoverTarget = target;
		Render();
	}
}

void OverlayRenderer::HandleMouseLeave()
{
	if (m_settingsOpen) {
		if (m_settingsHoverIndex != -1) {
			m_settingsHoverIndex = -1;
			Render();
		}
		return;
	}
	if (m_hoverTarget != HoverTarget::None) {
		m_hoverTarget = HoverTarget::None;
		Render();
	}
}

OverlayRenderer::HoverTarget OverlayRenderer::GetHoverTarget(const POINT &pt) const
{
	if (m_settingsOpen) {
		return HoverTarget::None;
	}
	if (m_galleryOpen) {
		if (IsPointInRect(pt, m_galleryBackRect))
			return HoverTarget::GalleryBack;
		if (IsPointInRect(pt, m_galleryFolderBackRect))
			return HoverTarget::GalleryFolderBack;
		if (IsPointInRect(pt, m_galleryFullscreenRect))
			return HoverTarget::GalleryFullscreen;
		if (IsPointInRect(pt, m_galleryMuteRect))
			return HoverTarget::GalleryMute;
		if (IsPointInRect(pt, m_galleryPlayRect))
			return HoverTarget::GalleryPlay;
		if (IsPointInRect(pt, m_gallerySetInRect))
			return HoverTarget::GallerySetIn;
		if (IsPointInRect(pt, m_gallerySetOutRect))
			return HoverTarget::GallerySetOut;
		if (IsPointInRect(pt, m_galleryTrimRect))
			return HoverTarget::GalleryTrim;
		if (IsPointInRect(pt, m_galleryDeleteRect))
			return HoverTarget::GalleryDelete;
		return HoverTarget::None;
	}
	if (!IsPointInRect(pt, m_layout.mainOverlayRect) &&
	    (m_layout.statsRect.right <= 0 || !IsPointInRect(pt, m_layout.statsRect)))
		return HoverTarget::None;
	if (IsPointInRect(pt, m_layout.settingsButtonRect))
		return HoverTarget::SettingsButton;
	if (IsPointInRect(pt, m_layout.statsButtonRect))
		return HoverTarget::StatsButton;
	if (m_state.recordingPathConfigured && IsPointInRect(pt, m_layout.recordFolderButtonRect))
		return HoverTarget::RecordFolderButton;
	if (IsPointInRect(pt, m_layout.showOBSButtonRect))
		return HoverTarget::ShowOBSButton;
	if (IsPointInRect(pt, m_layout.recordingPanelRect))
		return HoverTarget::RecordingPanel;
	if (IsPointInRect(pt, m_layout.replayPanelRect))
		return HoverTarget::ReplayPanel;
	return HoverTarget::None;
}

#endif // _WIN32
