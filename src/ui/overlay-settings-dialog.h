#pragma once

#ifdef ENABLE_QT
#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QGroupBox>
#include <QTabWidget>
#include <QSlider>
#include <QEvent>
#include <QLineEdit>

class OverlaySettingsDialog : public QDialog
{
	Q_OBJECT

public:
	explicit OverlaySettingsDialog(QWidget *parent = nullptr);
	~OverlaySettingsDialog();

	// --- Position ---
	enum Position {
		Top = 0,
		Bottom = 1,
		Left = 2,
		Right = 3,
		TopLeft = 4,
		TopRight = 5,
		BottomLeft = 6,
		BottomRight = 7,
		Center = 8
	};

	// --- Orientation ---
	enum Orientation {
		Horizontal = 0,
		Vertical = 1
	};

	// --- Getters ---
	Position getPosition() const;
	int getMargin() const;
	Orientation getOrientation() const;
	bool getAutoHideEnabled() const;
	int getAutoHideSeconds() const;
	bool getIndicatorsEnabled() const;
	Position getIndicatorsPosition() const;
	bool getIndicatorsOledProtection() const;
	bool getSmartReplayEnabled() const;
	bool getGalleryInOverlay() const;
	QString getGalleryExportPath() const;
	bool getCaptureFocus() const;
	double getOverlayBackgroundAlpha() const;

	// --- Setters ---
	void setPosition(Position position);
	void setMargin(int margin);
	void setOrientation(Orientation orientation);
	void setAutoHideEnabled(bool enabled);
	void setAutoHideSeconds(int seconds);
	void setIndicatorsEnabled(bool enabled);
	void setIndicatorsPosition(Position position);
	void setIndicatorsOledProtection(bool enabled);
	void setSmartReplayEnabled(bool enabled);
	void setGalleryInOverlay(bool enabled);
	void setGalleryExportPath(const QString &path);
	void setCaptureFocus(bool capture);
	void setOverlayBackgroundAlpha(double alpha);

	// --- Settings ---
	void loadSettings();

signals:
	void settingsChanged();
	void indicatorsChanged(bool enabled, int position, bool oledProtection);

private slots:
	void onAccepted();
	void onRejected();
	void onHotkeyButtonClicked();
	void onBrowseGalleryExportPath();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void setupUI();
	void saveSettings();
	QString hotkeyDisplayName(int vk, int mods) const;
	void loadHotkeys();
	void saveHotkeys();

	QComboBox *m_positionCombo;
	QSpinBox *m_marginSpin;
	QComboBox *m_orientationCombo;
	QCheckBox *m_autoHideCheck;
	QSpinBox *m_autoHideSecondsSpin;
	QCheckBox *m_indicatorsCheck;
	QComboBox *m_indicatorsPositionCombo;
	QCheckBox *m_indicatorsOledProtectionCheck;
	QCheckBox *m_smartReplayCheck;
	QCheckBox *m_galleryInOverlayCheck;
	QLineEdit *m_galleryExportPathEdit;
	QPushButton *m_galleryExportPathBrowseButton;
	QCheckBox *m_captureFocusCheck;
	QSlider *m_overlayOpacitySlider;
	QLabel *m_overlayOpacityLabel;

	QPushButton *m_okButton;
	QPushButton *m_cancelButton;

	QTabWidget *m_tabWidget;
	QWidget *m_generalPage;
	QWidget *m_hotkeysPage;
	QPushButton *m_hotkeyPlayBtn;
	QPushButton *m_hotkeySeekFwd5Btn;
	QPushButton *m_hotkeySeekBack5Btn;
	QPushButton *m_hotkeyFrameFwdBtn;
	QPushButton *m_hotkeyFrameBackBtn;
	QPushButton *m_hotkeyGoInBtn;
	QPushButton *m_hotkeyGoOutBtn;
	// Index of hotkey row awaiting key capture, or -1 when idle.
	int m_capturingHotkeyIndex;

	Position m_position;
	int m_margin;
	Orientation m_orientation;
	bool m_autoHideEnabled;
	int m_autoHideSeconds;
	bool m_indicatorsEnabled;
	Position m_indicatorsPosition;
	bool m_indicatorsOledProtection;
	bool m_smartReplayEnabled;
	bool m_galleryInOverlay;
	QString m_galleryExportPath;
	bool m_captureFocus;
	double m_overlayBackgroundAlpha;
};

#endif // ENABLE_QT
