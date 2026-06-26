#ifdef ENABLE_QT

#include "overlay-settings-dialog.h"
#include "overlay-settings-storage.h"
#include <obs-frontend-api.h>
#include <obs-data.h>
#include <plugin-support.h>
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QHideEvent>
#include <QFileDialog>

extern "C" {
#include <obs-module.h>
}

#define MT(key) QString::fromUtf8(obs_module_text(key))

OverlaySettingsDialog::OverlaySettingsDialog(QWidget *parent)
	: QDialog(parent)
	, m_position(Top)
	, m_margin(20)
	, m_orientation(Horizontal)
	, m_autoHideEnabled(false)
	, m_autoHideSeconds(5)
	, m_indicatorsEnabled(true)
	, m_indicatorsPosition(TopRight)
	, m_indicatorsOledProtection(false)
	, m_smartReplayEnabled(true)
	, m_galleryInOverlay(false)
	, m_galleryExportPath("")
	, m_captureFocus(true)
	, m_overlayBackgroundAlpha(0.88)
{
	setWindowTitle(MT("OverlaySettings"));
	setModal(true);
	resize(400, 300);
	setStyleSheet(
		"QDialog { background-color: #141419; color: #ffffff; }"
		"QGroupBox { border: 1px solid #3a3a45; border-radius: 8px; margin-top: 12px; padding: 8px; }"
		"QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 6px; color: #a882ff; }"
		"QLabel { color: #ffffff; }"
		"QComboBox, QSpinBox { background-color: #1e1e26; border: 1px solid #3a3a45; border-radius: 6px; padding: 4px 6px; color: #ffffff; }"
		"QComboBox::drop-down { border: none; }"
		"QComboBox QAbstractItemView { background-color: #1e1e26; color: #ffffff; selection-background-color: #a882ff; }"
		"QSpinBox::up-button, QSpinBox::down-button { background-color: #1e1e26; border: none; }"
		"QPushButton { background-color: #2a2433; border: 1px solid #a882ff; border-radius: 8px; padding: 6px 14px; color: #ffffff; }"
		"QPushButton:hover { background-color: #3a2f4a; }"
		"QPushButton:pressed { background-color: #241d32; }"
		"QPushButton#secondaryButton { background-color: #1b1b22; border: 1px solid #3a3a45; color: #cfcfe6; }"
		"QPushButton#secondaryButton:hover { background-color: #242430; }"
	);

	setupUI();
	loadSettings();
}

OverlaySettingsDialog::~OverlaySettingsDialog() {}

void OverlaySettingsDialog::setupUI()
{
	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	m_tabWidget = new QTabWidget(this);
	m_generalPage = new QWidget(this);
	QVBoxLayout *generalLayout = new QVBoxLayout(m_generalPage);

	// --- Position ---
	QGroupBox *positionGroup = new QGroupBox(MT("Position"), this);
	QFormLayout *positionLayout = new QFormLayout(positionGroup);

	m_positionCombo = new QComboBox(this);
	m_positionCombo->addItem(MT("Position.Top"), Top);
	m_positionCombo->addItem(MT("Position.Bottom"), Bottom);
	m_positionCombo->addItem(MT("Position.Left"), Left);
	m_positionCombo->addItem(MT("Position.Right"), Right);
	m_positionCombo->addItem(MT("Position.TopLeft"), TopLeft);
	m_positionCombo->addItem(MT("Position.TopRight"), TopRight);
	m_positionCombo->addItem(MT("Position.BottomLeft"), BottomLeft);
	m_positionCombo->addItem(MT("Position.BottomRight"), BottomRight);
	m_positionCombo->addItem(MT("Position.Center"), Center);
	positionLayout->addRow(MT("Position.Label"), m_positionCombo);

	m_marginSpin = new QSpinBox(this);
	m_marginSpin->setRange(0, 200);
	m_marginSpin->setSuffix(MT("MarginSuffix"));
	positionLayout->addRow(MT("Margin"), m_marginSpin);

	positionGroup->setLayout(positionLayout);
	generalLayout->addWidget(positionGroup);

	// --- Appearance ---
	QGroupBox *appearanceGroup = new QGroupBox(MT("Appearance"), this);
	QFormLayout *appearanceLayout = new QFormLayout(appearanceGroup);
	m_overlayOpacitySlider = new QSlider(Qt::Horizontal, this);
	m_overlayOpacitySlider->setRange(50, 100);
	m_overlayOpacitySlider->setValue(88);
	m_overlayOpacitySlider->setToolTip(MT("OverlayOpacity.Tooltip"));
	m_overlayOpacityLabel = new QLabel("88%", this);
	connect(m_overlayOpacitySlider, &QSlider::valueChanged, this, [this](int v) {
		m_overlayOpacityLabel->setText(QString::number(v) + "%");
	});
	QHBoxLayout *opacityRow = new QHBoxLayout();
	opacityRow->addWidget(m_overlayOpacitySlider);
	opacityRow->addWidget(m_overlayOpacityLabel, 0, Qt::AlignRight);
	appearanceLayout->addRow(MT("OverlayOpacity"), opacityRow);
	appearanceGroup->setLayout(appearanceLayout);
	generalLayout->addWidget(appearanceGroup);

	// --- Orientation ---
	QGroupBox *orientationGroup = new QGroupBox(MT("Orientation"), this);
	QFormLayout *orientationLayout = new QFormLayout(orientationGroup);

	m_orientationCombo = new QComboBox(this);
	m_orientationCombo->addItem(MT("Orientation.Horizontal"), Horizontal);
	m_orientationCombo->addItem(MT("Orientation.Vertical"), Vertical);
	orientationLayout->addRow(MT("Layout"), m_orientationCombo);

	orientationGroup->setLayout(orientationLayout);
	generalLayout->addWidget(orientationGroup);

	// --- Auto-hide ---
	QGroupBox *autoHideGroup = new QGroupBox(MT("AutoHide"), this);
	QFormLayout *autoHideLayout = new QFormLayout(autoHideGroup);

	m_autoHideCheck = new QCheckBox(MT("AutoHide.Enable"), this);
	autoHideLayout->addRow(m_autoHideCheck);

	m_autoHideSecondsSpin = new QSpinBox(this);
	m_autoHideSecondsSpin->setRange(1, 3600);
	m_autoHideSecondsSpin->setSuffix(MT("AutoHide.Suffix"));
	autoHideLayout->addRow(MT("AutoHide.Delay"), m_autoHideSecondsSpin);

	connect(m_autoHideCheck, &QCheckBox::toggled, m_autoHideSecondsSpin, &QSpinBox::setEnabled);

	autoHideGroup->setLayout(autoHideLayout);
	generalLayout->addWidget(autoHideGroup);

	// --- Indicators ---
	QGroupBox *indicatorsGroup = new QGroupBox(MT("Indicators"), this);
	QFormLayout *indicatorsLayout = new QFormLayout(indicatorsGroup);

	m_indicatorsCheck = new QCheckBox(MT("Indicators.Show"), this);
	indicatorsLayout->addRow(m_indicatorsCheck);

	m_indicatorsPositionCombo = new QComboBox(this);
	m_indicatorsPositionCombo->addItem(MT("Position.Top"), Top);
	m_indicatorsPositionCombo->addItem(MT("Position.Bottom"), Bottom);
	m_indicatorsPositionCombo->addItem(MT("Position.Left"), Left);
	m_indicatorsPositionCombo->addItem(MT("Position.Right"), Right);
	m_indicatorsPositionCombo->addItem(MT("Position.TopLeft"), TopLeft);
	m_indicatorsPositionCombo->addItem(MT("Position.TopRight"), TopRight);
	m_indicatorsPositionCombo->addItem(MT("Position.BottomLeft"), BottomLeft);
	m_indicatorsPositionCombo->addItem(MT("Position.BottomRight"), BottomRight);
	m_indicatorsPositionCombo->addItem(MT("Position.Center"), Center);
	indicatorsLayout->addRow(MT("Position.Label"), m_indicatorsPositionCombo);

	m_indicatorsOledProtectionCheck = new QCheckBox(MT("Indicators.OledProtection"), this);
	m_indicatorsOledProtectionCheck->setToolTip(MT("Indicators.OledProtection.Tooltip"));
	indicatorsLayout->addRow(m_indicatorsOledProtectionCheck);

	auto emitIndicatorsChanged = [this]() {
		emit indicatorsChanged(m_indicatorsEnabled, m_indicatorsPosition, m_indicatorsOledProtection);
	};

	connect(m_indicatorsCheck, &QCheckBox::toggled,
	        m_indicatorsPositionCombo, &QComboBox::setEnabled);
	connect(m_indicatorsCheck, &QCheckBox::toggled, m_indicatorsOledProtectionCheck,
	        &QCheckBox::setEnabled);
	connect(m_indicatorsCheck, &QCheckBox::toggled, this, [this, emitIndicatorsChanged](bool enabled) {
		m_indicatorsEnabled = enabled;
		m_indicatorsPosition =
			static_cast<Position>(m_indicatorsPositionCombo->currentData().toInt());
		m_indicatorsOledProtection = m_indicatorsOledProtectionCheck->isChecked();
		emitIndicatorsChanged();
	});
	connect(m_indicatorsPositionCombo,
	        QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this, emitIndicatorsChanged](int) {
		        if (!m_indicatorsCheck->isChecked()) {
			        return;
		        }
		        m_indicatorsPosition = static_cast<Position>(
			        m_indicatorsPositionCombo->currentData().toInt());
		        emitIndicatorsChanged();
	        });
	connect(m_indicatorsOledProtectionCheck, &QCheckBox::toggled, this,
	        [this, emitIndicatorsChanged](bool enabled) {
		        if (!m_indicatorsCheck->isChecked()) {
			        return;
		        }
		        m_indicatorsOledProtection = enabled;
		        emitIndicatorsChanged();
	        });

	indicatorsGroup->setLayout(indicatorsLayout);
	generalLayout->addWidget(indicatorsGroup);

	// --- Replay ---
	QGroupBox *replayGroup = new QGroupBox(MT("Replay"), this);
	QFormLayout *replayLayout = new QFormLayout(replayGroup);
	m_smartReplayCheck = new QCheckBox(MT("SmartReplay"), this);
	replayLayout->addRow(m_smartReplayCheck);
	replayGroup->setLayout(replayLayout);
	generalLayout->addWidget(replayGroup);

	// --- Gallery ---
	QGroupBox *galleryGroup = new QGroupBox(MT("Gallery"), this);
	QFormLayout *galleryLayout = new QFormLayout(galleryGroup);
	m_galleryInOverlayCheck = new QCheckBox(MT("Gallery.InOverlay"), this);
	galleryLayout->addRow(m_galleryInOverlayCheck);
	m_galleryExportPathEdit = new QLineEdit(this);
	m_galleryExportPathEdit->setPlaceholderText(MT("Gallery.ExportPlaceholder"));
	m_galleryExportPathBrowseButton = new QPushButton(MT("Gallery.Browse"), this);
	m_galleryExportPathBrowseButton->setObjectName("secondaryButton");
	connect(m_galleryExportPathBrowseButton, &QPushButton::clicked, this,
		&OverlaySettingsDialog::onBrowseGalleryExportPath);
	QHBoxLayout *galleryPathRow = new QHBoxLayout();
	galleryPathRow->addWidget(m_galleryExportPathEdit);
	galleryPathRow->addWidget(m_galleryExportPathBrowseButton);
	galleryLayout->addRow(MT("Gallery.ExportFolder"), galleryPathRow);
	galleryGroup->setLayout(galleryLayout);
	generalLayout->addWidget(galleryGroup);

	// --- Focus ---
	QGroupBox *focusGroup = new QGroupBox(MT("Focus"), this);
	QFormLayout *focusLayout = new QFormLayout(focusGroup);
	m_captureFocusCheck = new QCheckBox(MT("CaptureFocus"), this);
	m_captureFocusCheck->setToolTip(MT("CaptureFocus.Tooltip"));
	focusLayout->addRow(m_captureFocusCheck);
	focusGroup->setLayout(focusLayout);
	generalLayout->addWidget(focusGroup);

	generalLayout->addStretch();
	m_tabWidget->addTab(m_generalPage, MT("Tab.General"));

	// --- Hotkeys ---
	m_hotkeysPage = new QWidget(this);
	m_capturingHotkeyIndex = -1;
	QVBoxLayout *hotkeysLayout = new QVBoxLayout(m_hotkeysPage);
	QGroupBox *hotkeysGroup = new QGroupBox(MT("Hotkeys.Gallery"), this);
	QFormLayout *hotkeysForm = new QFormLayout(hotkeysGroup);

	m_hotkeyPlayBtn = new QPushButton("Space", this);
	m_hotkeyPlayBtn->setProperty("hotkeyIndex", 0);
	connect(m_hotkeyPlayBtn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.PlayPause"), m_hotkeyPlayBtn);

	m_hotkeySeekFwd5Btn = new QPushButton("Right", this);
	m_hotkeySeekFwd5Btn->setProperty("hotkeyIndex", 1);
	connect(m_hotkeySeekFwd5Btn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.SeekForward5"), m_hotkeySeekFwd5Btn);

	m_hotkeySeekBack5Btn = new QPushButton("Left", this);
	m_hotkeySeekBack5Btn->setProperty("hotkeyIndex", 2);
	connect(m_hotkeySeekBack5Btn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.SeekBack5"), m_hotkeySeekBack5Btn);

	m_hotkeyFrameFwdBtn = new QPushButton(".", this);
	m_hotkeyFrameFwdBtn->setProperty("hotkeyIndex", 3);
	connect(m_hotkeyFrameFwdBtn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.FrameForward"), m_hotkeyFrameFwdBtn);

	m_hotkeyFrameBackBtn = new QPushButton(",", this);
	m_hotkeyFrameBackBtn->setProperty("hotkeyIndex", 4);
	connect(m_hotkeyFrameBackBtn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.FrameBack"), m_hotkeyFrameBackBtn);

	m_hotkeyGoInBtn = new QPushButton("Home", this);
	m_hotkeyGoInBtn->setProperty("hotkeyIndex", 5);
	connect(m_hotkeyGoInBtn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.GoIn"), m_hotkeyGoInBtn);

	m_hotkeyGoOutBtn = new QPushButton("End", this);
	m_hotkeyGoOutBtn->setProperty("hotkeyIndex", 6);
	connect(m_hotkeyGoOutBtn, &QPushButton::clicked, this, &OverlaySettingsDialog::onHotkeyButtonClicked);
	hotkeysForm->addRow(MT("Hotkeys.GoOut"), m_hotkeyGoOutBtn);

	hotkeysGroup->setLayout(hotkeysForm);
	hotkeysLayout->addWidget(hotkeysGroup);
	hotkeysLayout->addStretch();
	m_tabWidget->addTab(m_hotkeysPage, MT("Tab.Hotkeys"));

	mainLayout->addWidget(m_tabWidget);

	// --- Dialog buttons ---
	QHBoxLayout *buttonLayout = new QHBoxLayout();
	m_okButton = new QPushButton(MT("OK"), this);
	m_cancelButton = new QPushButton(MT("Cancel"), this);
	m_cancelButton->setObjectName("secondaryButton");

	connect(m_okButton, &QPushButton::clicked, this, &OverlaySettingsDialog::onAccepted);
	connect(m_cancelButton, &QPushButton::clicked, this, &OverlaySettingsDialog::onRejected);

	buttonLayout->addStretch();
	buttonLayout->addWidget(m_okButton);
	buttonLayout->addWidget(m_cancelButton);

	mainLayout->addLayout(buttonLayout);

	setLayout(mainLayout);
}

void OverlaySettingsDialog::loadSettings()
{
	if (saved_settings_data) {
		int position = obs_data_get_int(saved_settings_data, "position");
		if (position >= Top && position <= Center) {
			m_position = static_cast<Position>(position);
		} else {
			m_position = Top;
		}

		m_margin = obs_data_get_int(saved_settings_data, "margin");
		if (m_margin < 0 || m_margin > 200) {
			m_margin = 20;
		}

		int orientation = obs_data_get_int(saved_settings_data, "orientation");
		if (orientation == Horizontal || orientation == Vertical) {
			m_orientation = static_cast<Orientation>(orientation);
		} else {
			m_orientation = Horizontal;
		}

		m_autoHideEnabled = obs_data_get_bool(saved_settings_data, "auto_hide_enabled");
		int autoHideSeconds = obs_data_get_int(saved_settings_data, "auto_hide_seconds");
		if (autoHideSeconds < 1 || autoHideSeconds > 3600) {
			m_autoHideSeconds = 5;
		} else {
			m_autoHideSeconds = autoHideSeconds;
		}

		const bool hasIndicatorsEnabled =
			obs_data_has_user_value(saved_settings_data, "indicators_enabled");
		m_indicatorsEnabled = hasIndicatorsEnabled
			? obs_data_get_bool(saved_settings_data, "indicators_enabled")
			: true;
		int indicatorsPosition = obs_data_get_int(saved_settings_data, "indicators_position");
		if (indicatorsPosition >= Top && indicatorsPosition <= Center) {
			m_indicatorsPosition = static_cast<Position>(indicatorsPosition);
		} else {
			m_indicatorsPosition = TopRight;
		}

		const bool hasIndicatorsOledProtection =
			obs_data_has_user_value(saved_settings_data, "indicators_oled_protection");
		m_indicatorsOledProtection = hasIndicatorsOledProtection
			? obs_data_get_bool(saved_settings_data, "indicators_oled_protection")
			: false;

		const bool hasSmartReplay =
			obs_data_has_user_value(saved_settings_data, "smart_replay");
		m_smartReplayEnabled = hasSmartReplay
			? obs_data_get_bool(saved_settings_data, "smart_replay")
			: true;

		const bool hasGalleryInOverlay =
			obs_data_has_user_value(saved_settings_data, "gallery_in_overlay");
		m_galleryInOverlay = hasGalleryInOverlay
			? obs_data_get_bool(saved_settings_data, "gallery_in_overlay")
			: false;
		if (obs_data_has_user_value(saved_settings_data, "gallery_export_path")) {
			m_galleryExportPath =
				QString::fromUtf8(obs_data_get_string(saved_settings_data, "gallery_export_path"));
		} else {
			m_galleryExportPath.clear();
		}

		const bool hasCaptureFocus =
			obs_data_has_user_value(saved_settings_data, "capture_focus");
		m_captureFocus = hasCaptureFocus
			? obs_data_get_bool(saved_settings_data, "capture_focus")
			: true;

		if (obs_data_has_user_value(saved_settings_data, "overlay_background_alpha")) {
			double a = obs_data_get_double(saved_settings_data, "overlay_background_alpha");
			if (a >= 0.5 && a <= 1.0)
				m_overlayBackgroundAlpha = a;
		}
	} else {
		m_position = Top;
		m_margin = 20;
		m_orientation = Horizontal;
		m_autoHideEnabled = false;
		m_autoHideSeconds = 5;
		m_indicatorsEnabled = true;
		m_indicatorsPosition = TopRight;
		m_indicatorsOledProtection = false;
		m_smartReplayEnabled = true;
		m_galleryInOverlay = false;
		m_galleryExportPath.clear();
		m_captureFocus = true;
		m_overlayBackgroundAlpha = 0.88;
	}

	int index = m_positionCombo->findData(m_position);
	if (index >= 0) {
		m_positionCombo->setCurrentIndex(index);
	}
	m_marginSpin->setValue(m_margin);
	int orientIndex = m_orientationCombo->findData(m_orientation);
	if (orientIndex >= 0) {
		m_orientationCombo->setCurrentIndex(orientIndex);
	}

	m_autoHideCheck->setChecked(m_autoHideEnabled);
	m_autoHideSecondsSpin->setEnabled(m_autoHideEnabled);
	m_autoHideSecondsSpin->setValue(m_autoHideSeconds);

	m_indicatorsCheck->setChecked(m_indicatorsEnabled);
	m_indicatorsPositionCombo->setEnabled(m_indicatorsEnabled);
	m_indicatorsOledProtectionCheck->setEnabled(m_indicatorsEnabled);
	m_indicatorsOledProtectionCheck->setChecked(m_indicatorsOledProtection);
	int indicatorsIndex = m_indicatorsPositionCombo->findData(m_indicatorsPosition);
	if (indicatorsIndex >= 0) {
		m_indicatorsPositionCombo->setCurrentIndex(indicatorsIndex);
	}

	m_smartReplayCheck->setChecked(m_smartReplayEnabled);
	m_galleryInOverlayCheck->setChecked(m_galleryInOverlay);
	m_galleryExportPathEdit->setText(m_galleryExportPath);
	m_captureFocusCheck->setChecked(m_captureFocus);
	int opacityPct = static_cast<int>(m_overlayBackgroundAlpha * 100.0);
	m_overlayOpacitySlider->setValue(opacityPct);
	m_overlayOpacityLabel->setText(QString::number(opacityPct) + "%");
	loadHotkeys();
}

void OverlaySettingsDialog::saveSettings()
{
	// Keep runtime settings in sync so reopening the dialog reflects current state.
	overlay_settings_ensure();
	obs_data_set_int(saved_settings_data, "position", m_position);
	obs_data_set_int(saved_settings_data, "margin", m_margin);
	obs_data_set_int(saved_settings_data, "orientation", m_orientation);
	obs_data_set_bool(saved_settings_data, "auto_hide_enabled", m_autoHideEnabled);
	obs_data_set_int(saved_settings_data, "auto_hide_seconds", m_autoHideSeconds);
	obs_data_set_bool(saved_settings_data, "indicators_enabled", m_indicatorsEnabled);
	obs_data_set_int(saved_settings_data, "indicators_position", m_indicatorsPosition);
	obs_data_set_bool(saved_settings_data, "indicators_oled_protection", m_indicatorsOledProtection);
	obs_data_set_bool(saved_settings_data, "smart_replay", m_smartReplayEnabled);
	obs_data_set_bool(saved_settings_data, "gallery_in_overlay", m_galleryInOverlay);
	obs_data_set_string(saved_settings_data, "gallery_export_path",
			    m_galleryExportPath.trimmed().toUtf8().constData());
	obs_data_set_bool(saved_settings_data, "capture_focus", m_captureFocus);
	m_overlayBackgroundAlpha = m_overlayOpacitySlider->value() / 100.0;
	obs_data_set_double(saved_settings_data, "overlay_background_alpha", m_overlayBackgroundAlpha);
	saveHotkeys();

	if (!overlay_settings_save())
		obs_log(LOG_ERROR, "Overlay settings not saved to disk");
}

void OverlaySettingsDialog::onAccepted()
{
	m_position = static_cast<Position>(m_positionCombo->currentData().toInt());
	m_margin = m_marginSpin->value();
	m_orientation = static_cast<Orientation>(m_orientationCombo->currentData().toInt());
	m_autoHideEnabled = m_autoHideCheck->isChecked();
	m_autoHideSeconds = m_autoHideSecondsSpin->value();
	m_indicatorsEnabled = m_indicatorsCheck->isChecked();
	m_indicatorsPosition = static_cast<Position>(m_indicatorsPositionCombo->currentData().toInt());
	m_indicatorsOledProtection = m_indicatorsOledProtectionCheck->isChecked();
	m_smartReplayEnabled = m_smartReplayCheck->isChecked();
	m_galleryInOverlay = m_galleryInOverlayCheck->isChecked();
	m_galleryExportPath = m_galleryExportPathEdit->text().trimmed();
	m_captureFocus = m_captureFocusCheck->isChecked();

	saveSettings();
	emit settingsChanged();
	accept();
}

void OverlaySettingsDialog::onRejected()
{
	reject();
}

static const struct { int vk; const char *name; } s_vkNames[] = {
	{ 32, "Space" }, { 37, "Left" }, { 39, "Right" }, { 36, "Home" }, { 35, "End" },
	{ 188, "," }, { 190, "." }, { 112, "F1" }, { 113, "F2" }, { 114, "F3" }, { 115, "F4" },
	{ 116, "F5" }, { 117, "F6" }, { 118, "F7" }, { 119, "F8" }, { 120, "F9" }, { 121, "F10" },
	{ 0, nullptr }
};

QString OverlaySettingsDialog::hotkeyDisplayName(int vk, int mods) const
{
	QString modStr;
	if (mods & 2) modStr += "Ctrl+";
	if (mods & 4) modStr += "Alt+";
	if (mods & 1) modStr += "Shift+";
	for (int i = 0; s_vkNames[i].name != nullptr; i++) {
		if (s_vkNames[i].vk == vk) {
			return modStr + QString::fromUtf8(s_vkNames[i].name);
		}
	}
	if (vk >= 0x41 && vk <= 0x5A) {
		return modStr + QChar(vk);
	}
	if (vk >= 0x30 && vk <= 0x39) {
		return modStr + QChar(vk);
	}
	return modStr + QString::number(vk);
}

void OverlaySettingsDialog::loadHotkeys()
{
	static const struct { const char *name; int defaultVk; int defaultMod; QPushButton **btn; } rows[] = {
		{ "hotkey_play", 32, 0, &m_hotkeyPlayBtn },
		{ "hotkey_seek_forward_5", 39, 0, &m_hotkeySeekFwd5Btn },
		{ "hotkey_seek_back_5", 37, 0, &m_hotkeySeekBack5Btn },
		{ "hotkey_frame_forward", 190, 0, &m_hotkeyFrameFwdBtn },
		{ "hotkey_frame_back", 188, 0, &m_hotkeyFrameBackBtn },
		{ "hotkey_go_in", 36, 0, &m_hotkeyGoInBtn },
		{ "hotkey_go_out", 35, 0, &m_hotkeyGoOutBtn },
	};
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		int vk = rows[i].defaultVk;
		int mod = rows[i].defaultMod;
		if (saved_settings_data) {
			QString vkKey = QString::fromUtf8(rows[i].name) + "_vk";
			QString modKey = QString::fromUtf8(rows[i].name) + "_mod";
			if (obs_data_has_user_value(saved_settings_data, vkKey.toUtf8().constData())) {
				vk = static_cast<int>(obs_data_get_int(saved_settings_data, vkKey.toUtf8().constData()));
				mod = static_cast<int>(obs_data_get_int(saved_settings_data, modKey.toUtf8().constData()));
			}
		}
		QPushButton *b = *rows[i].btn;
		b->setText(hotkeyDisplayName(vk, mod));
		b->setProperty("_vk", vk);
		b->setProperty("_mod", mod);
	}
}

void OverlaySettingsDialog::saveHotkeys()
{
	static const struct { const char *name; QPushButton *btn; } rows[] = {
		{ "hotkey_play", m_hotkeyPlayBtn },
		{ "hotkey_seek_forward_5", m_hotkeySeekFwd5Btn },
		{ "hotkey_seek_back_5", m_hotkeySeekBack5Btn },
		{ "hotkey_frame_forward", m_hotkeyFrameFwdBtn },
		{ "hotkey_frame_back", m_hotkeyFrameBackBtn },
		{ "hotkey_go_in", m_hotkeyGoInBtn },
		{ "hotkey_go_out", m_hotkeyGoOutBtn },
	};
	overlay_settings_ensure();
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		int vk = rows[i].btn->property("_vk").toInt();
		int mod = rows[i].btn->property("_mod").toInt();
		QString vkKey = QString::fromUtf8(rows[i].name) + "_vk";
		QString modKey = QString::fromUtf8(rows[i].name) + "_mod";
		obs_data_set_int(saved_settings_data, vkKey.toUtf8().constData(), vk);
		obs_data_set_int(saved_settings_data, modKey.toUtf8().constData(), mod);
	}
}

void OverlaySettingsDialog::onHotkeyButtonClicked()
{
	QPushButton *btn = qobject_cast<QPushButton *>(sender());
	if (!btn || !btn->property("hotkeyIndex").isValid()) {
		return;
	}
	m_capturingHotkeyIndex = btn->property("hotkeyIndex").toInt();
	btn->setText(MT("Hotkeys.PressKey"));
	qApp->installEventFilter(this);
}

bool OverlaySettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
	if (m_capturingHotkeyIndex >= 0 && event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
		int vk = keyEvent->nativeVirtualKey();
		int mods = 0;
		if (keyEvent->modifiers() & Qt::ShiftModifier) mods |= 1;
		if (keyEvent->modifiers() & Qt::ControlModifier) mods |= 2;
		if (keyEvent->modifiers() & Qt::AltModifier) mods |= 4;
		if (vk > 0) {
			QPushButton *buttons[] = { m_hotkeyPlayBtn, m_hotkeySeekFwd5Btn, m_hotkeySeekBack5Btn,
			                            m_hotkeyFrameFwdBtn, m_hotkeyFrameBackBtn, m_hotkeyGoInBtn, m_hotkeyGoOutBtn };
			QPushButton *btn = buttons[m_capturingHotkeyIndex];
			btn->setProperty("_vk", vk);
			btn->setProperty("_mod", mods);
			btn->setText(hotkeyDisplayName(vk, mods));
			qApp->removeEventFilter(this);
			m_capturingHotkeyIndex = -1;
			return true;
		}
	}
	return QDialog::eventFilter(watched, event);
}

void OverlaySettingsDialog::hideEvent(QHideEvent *event)
{
	QDialog::hideEvent(event);
	// Drop modality once the dialog is closed/hidden. A persistent modal child
	// keeps OBS from minimizing to tray because OBSBasic::ToggleShowHide() bails
	// out whenever EnumDialogs() finds a child QDialog whose isModal() is true,
	// and that flag stays set even while the dialog is hidden. Modality is
	// re-armed right before the dialog is shown again.
	setModal(false);
}

OverlaySettingsDialog::Position OverlaySettingsDialog::getPosition() const
{
	return m_position;
}

int OverlaySettingsDialog::getMargin() const
{
	return m_margin;
}

void OverlaySettingsDialog::setPosition(Position position)
{
	m_position = position;
	int index = m_positionCombo->findData(position);
	if (index >= 0) {
		m_positionCombo->setCurrentIndex(index);
	}
}

void OverlaySettingsDialog::setMargin(int margin)
{
	m_margin = margin;
	m_marginSpin->setValue(margin);
}

OverlaySettingsDialog::Orientation OverlaySettingsDialog::getOrientation() const
{
	return m_orientation;
}

bool OverlaySettingsDialog::getAutoHideEnabled() const
{
	return m_autoHideEnabled;
}

int OverlaySettingsDialog::getAutoHideSeconds() const
{
	return m_autoHideSeconds;
}

bool OverlaySettingsDialog::getIndicatorsEnabled() const
{
	return m_indicatorsEnabled;
}

OverlaySettingsDialog::Position OverlaySettingsDialog::getIndicatorsPosition() const
{
	return m_indicatorsPosition;
}

bool OverlaySettingsDialog::getIndicatorsOledProtection() const
{
	return m_indicatorsOledProtection;
}

bool OverlaySettingsDialog::getSmartReplayEnabled() const
{
	return m_smartReplayEnabled;
}

bool OverlaySettingsDialog::getGalleryInOverlay() const
{
	return m_galleryInOverlay;
}

QString OverlaySettingsDialog::getGalleryExportPath() const
{
	return m_galleryExportPath;
}

bool OverlaySettingsDialog::getCaptureFocus() const
{
	return m_captureFocus;
}

void OverlaySettingsDialog::setOrientation(Orientation orientation)
{
	m_orientation = orientation;
	int index = m_orientationCombo->findData(orientation);
	if (index >= 0) {
		m_orientationCombo->setCurrentIndex(index);
	}
}

void OverlaySettingsDialog::setAutoHideEnabled(bool enabled)
{
	m_autoHideEnabled = enabled;
	m_autoHideCheck->setChecked(enabled);
	m_autoHideSecondsSpin->setEnabled(enabled);
}

void OverlaySettingsDialog::setAutoHideSeconds(int seconds)
{
	m_autoHideSeconds = seconds;
	m_autoHideSecondsSpin->setValue(seconds);
}

void OverlaySettingsDialog::setIndicatorsEnabled(bool enabled)
{
	m_indicatorsEnabled = enabled;
	m_indicatorsCheck->setChecked(enabled);
	m_indicatorsPositionCombo->setEnabled(enabled);
}

void OverlaySettingsDialog::setGalleryInOverlay(bool enabled)
{
	m_galleryInOverlay = enabled;
	m_galleryInOverlayCheck->setChecked(enabled);
}

void OverlaySettingsDialog::setGalleryExportPath(const QString &path)
{
	m_galleryExportPath = path.trimmed();
	if (m_galleryExportPathEdit) {
		m_galleryExportPathEdit->setText(m_galleryExportPath);
	}
}

void OverlaySettingsDialog::setCaptureFocus(bool capture)
{
	m_captureFocus = capture;
	m_captureFocusCheck->setChecked(capture);
}

void OverlaySettingsDialog::onBrowseGalleryExportPath()
{
	const QString basePath = m_galleryExportPathEdit->text().trimmed();
	const QString selected = QFileDialog::getExistingDirectory(
		this, MT("Gallery.SelectExportFolder"), basePath, QFileDialog::ShowDirsOnly);
	if (!selected.isEmpty()) {
		m_galleryExportPathEdit->setText(selected);
	}
}

double OverlaySettingsDialog::getOverlayBackgroundAlpha() const
{
	return m_overlayBackgroundAlpha;
}

void OverlaySettingsDialog::setOverlayBackgroundAlpha(double alpha)
{
	if (alpha >= 0.5 && alpha <= 1.0) {
		m_overlayBackgroundAlpha = alpha;
		if (m_overlayOpacitySlider) {
			m_overlayOpacitySlider->setValue(static_cast<int>(alpha * 100.0));
			if (m_overlayOpacityLabel)
				m_overlayOpacityLabel->setText(QString::number(static_cast<int>(alpha * 100.0)) + "%");
		}
	}
}

void OverlaySettingsDialog::setIndicatorsPosition(Position position)
{
	m_indicatorsPosition = position;
	int index = m_indicatorsPositionCombo->findData(position);
	if (index >= 0) {
		m_indicatorsPositionCombo->setCurrentIndex(index);
	}
}

void OverlaySettingsDialog::setIndicatorsOledProtection(bool enabled)
{
	m_indicatorsOledProtection = enabled;
	m_indicatorsOledProtectionCheck->setChecked(enabled);
}

void OverlaySettingsDialog::setSmartReplayEnabled(bool enabled)
{
	m_smartReplayEnabled = enabled;
	m_smartReplayCheck->setChecked(enabled);
}

#endif // ENABLE_QT
