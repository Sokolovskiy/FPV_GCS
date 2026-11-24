#include "window.h"
#include "mappingdialog.h"
#include "video.h"
#include "capture.h"
#include <QSplitter>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QHeaderView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QSettings>
#include <QShortcut>
#include <algorithm>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace fpv {
namespace {
QIcon settingsIcon() {
    QPixmap pixmap(48, 48); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath gear;
    constexpr double pi = 3.141592653589793;
    for (int point = 0; point < 64; ++point) {
        const double angle = 2 * pi * point / 64;
        const double radius = (point % 8 >= 2 && point % 8 <= 5) ? 21 : 16;
        const QPointF position(24 + radius * std::cos(angle), 24 + radius * std::sin(angle));
        if (point == 0) gear.moveTo(position); else gear.lineTo(position);
    }
    gear.closeSubpath(); gear.addEllipse(QPointF(24, 24), 8, 8);
    gear.setFillRule(Qt::OddEvenFill);
    painter.fillPath(gear, QColor("#b8d4eb"));
    return QIcon(pixmap);
}
}

Window::Window() {
    setWindowTitle("FPV GCS — RC Channels · 0.1.10"); resize(1090, 860); setMinimumSize(930, 740);
    resize(1580, 860);
    auto* fullscreen = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreen->setAutoRepeat(false);
    connect(fullscreen, &QShortcut::activated, this, [this] {
        if (isFullScreen()) {
            setWindowState(m_windowedState);
        } else {
            m_windowedState = windowState();
            showFullScreen();
        }
    });
    auto* split = new QSplitter(Qt::Horizontal);
    split->setHandleWidth(8);
    m_video = new VideoPanel;
    auto* central = new QWidget;
    split->addWidget(m_video); split->addWidget(central);
    split->setSizes({1090, 490}); split->setCollapsible(0, false); split->setCollapsible(1, false);
    setCentralWidget(split);
    connect(m_video, &VideoPanel::stopped, this, &QWidget::close);
    auto* layout = new QVBoxLayout(central); layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(12);
    auto* top = new QHBoxLayout;
    auto* title = new QLabel("FPV GCS"); title->setObjectName("title"); top->addWidget(title); top->addStretch();
    auto* local = new QLabel("RC → UDP · 100 Hz"); local->setObjectName("localBadge"); top->addWidget(local); layout->addLayout(top);
    auto* deviceRow = new QHBoxLayout;
    m_status = new QLabel("DISCONNECTED"); m_status->setMinimumWidth(125); deviceRow->addWidget(m_status);
    m_devices = new QComboBox; m_devices->setMinimumWidth(120);
    m_devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_devices->setMinimumContentsLength(8); deviceRow->addWidget(m_devices, 1);
    auto* inputs = new QPushButton("Діагностика…"); deviceRow->addWidget(inputs); layout->addLayout(deviceRow);
    m_udp.port = quint16(std::clamp(QSettings().value("udp/port", 14600).toInt(), 1, 65535));
    auto* captureBar = new CaptureBar;
    layout->addWidget(captureBar);
    connect(m_video, &VideoPanel::statusChanged, captureBar, &CaptureBar::videoStatus);
    m_table = new QTableWidget(ChannelCount, 4);
    m_table->setHorizontalHeaderLabels({"Function", "RC Level", "Функція каналу", ""});
    m_table->verticalHeader()->hide(); m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection); m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setShowGrid(false); m_table->setAlternatingRowColors(true);
    auto* header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(32);
    // Keep the reference widths; only RC Level absorbs changes in pane width.
    m_table->setColumnWidth(0, 60);
    m_table->setColumnWidth(2, 155); m_table->setColumnWidth(3, 46);
    // Preserve the fixed columns and enough space for the RC value at minimum pane width.
    m_table->setMinimumWidth(60 + 155 + 46 + 80 + 2 * m_table->frameWidth());
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const auto gear = settingsIcon();
    for (int i = 0; i < ChannelCount; ++i) {
        m_table->setRowHeight(i, 34);
        m_table->setItem(i, 0, new QTableWidgetItem(channelName(i)));
        m_bars[i] = new QProgressBar; m_bars[i]->setRange(1000, 2000);
        m_bars[i]->setTextVisible(true); m_bars[i]->setFormat("%v"); m_bars[i]->setAlignment(Qt::AlignCenter); m_bars[i]->setFixedHeight(20);
        m_bars[i]->setAccessibleName(QString("RC Level CH%1").arg(i + 1));
        auto* barHolder = new QWidget; auto* barLayout = new QHBoxLayout(barHolder); barLayout->setContentsMargins(8, 4, 12, 4); barLayout->addWidget(m_bars[i]); m_table->setCellWidget(i, 1, barHolder);
        auto* function = new QLineEdit; function->setMaxLength(100);
        function->setPlaceholderText("Вкажіть функцію…"); function->setAccessibleName(QString("Функція CH%1").arg(i + 1));
        function->setEnabled(false); m_functions[i] = function;
        auto* functionHolder = new QWidget;
        auto* functionLayout = new QVBoxLayout(functionHolder);
        functionLayout->setContentsMargins(0, 3, 0, 3);
        function->setStyleSheet("padding:2px 5px;");
        functionLayout->addWidget(function);
        m_table->setCellWidget(i, 2, functionHolder);
        connect(function, &QLineEdit::editingFinished, this, [this, i, function] {
            if (m_controller.identity().isEmpty() || m_mappings[i].functionText == function->text()) return;
            m_mappings[i].functionText = function->text(); save();
        });
        auto* configureButton = new QPushButton; configureButton->setIcon(gear); configureButton->setIconSize(QSize(19, 19));
        configureButton->setFixedSize(32, 28); configureButton->setStyleSheet("padding:4px;");
        configureButton->setToolTip(QString("Налаштування CH%1").arg(i + 1));
        configureButton->setAccessibleName(QString("Налаштування CH%1").arg(i + 1));
        auto* settingsHolder = new QWidget; auto* settingsLayout = new QHBoxLayout(settingsHolder);
        settingsLayout->setContentsMargins(0, 0, 0, 0); settingsLayout->setAlignment(Qt::AlignCenter); settingsLayout->addWidget(configureButton);
        m_table->setCellWidget(i, 3, settingsHolder);
        connect(configureButton, &QPushButton::clicked, this, [this, i] { configure(i); });
    }
    // Stop the table at the last row; leave surplus window height outside its frame.
    m_table->ensurePolished();
    header->ensurePolished();
    m_table->setMaximumHeight(header->sizeHint().height() +
        m_table->verticalHeader()->length() + 2 * m_table->frameWidth());
    layout->addWidget(m_table, 1);
    layout->addStretch();
    connect(inputs, &QPushButton::clicked, this, [this] { rawMonitor(); });
    connect(m_devices, &QComboBox::currentIndexChanged, this, [this](int index) {
        selectDevice(index < 0 ? 0 : m_devices->itemData(index).toUInt());
    });
    auto* polling = new QTimer(this); polling->setTimerType(Qt::PreciseTimer); polling->setInterval(8);
    connect(polling, &QTimer::timeout, this, [this] { m_controller.update(); m_channels = evaluateAll(m_mappings, m_controller.snapshot()); }); polling->start();
    auto* transmission = new QTimer(this); transmission->setTimerType(Qt::PreciseTimer); transmission->setInterval(10);
    connect(transmission, &QTimer::timeout, this, [this] {
        // Refresh USB immediately before sending; never transmit disconnected safe defaults.
        m_controller.update(); m_channels = evaluateAll(m_mappings, m_controller.snapshot());
        m_udp.send(m_channels, commandsReady(m_mappings, m_controller.snapshot()));
    }); transmission->start();
    auto* display = new QTimer(this); display->setInterval(20); connect(display, &QTimer::timeout, this, [this] { refreshValues(); }); display->start();
    auto* enumeration = new QTimer(this); enumeration->setInterval(750); connect(enumeration, &QTimer::timeout, this, [this] { refreshDevices(); }); enumeration->start();
    refreshDevices(); refreshValues();
    QTimer::singleShot(0, this, [this, split] {
        // Apply the reference layout after the maximized window has its final size.
        const int width = split->width() - split->handleWidth();
        split->setSizes({width * 69 / 100, width - width * 69 / 100});
    });
}
void Window::closeEvent(QCloseEvent* event) {
    // Keep the native video HWND and GUI event processing alive during shutdown.
    if (!m_video->stop()) { event->ignore(); return; }
    QMainWindow::closeEvent(event);
}
void Window::refreshDevices() {
    const auto available = m_controller.devices();
    const auto wanted = QSettings().value("controller/identity").toString();
    // A reconnect retains the in-memory channel mapping. Never adopt a sole
    // unknown device when the selected radio is unplugged or after app restart.
    if (!m_controller.snapshot().connected) m_controller.reconnect(wanted);
    bool same = available.size() == m_known.size();
    if (same) for (int i = 0; i < available.size(); ++i) if (available[i].id != m_known[i].id) { same = false; break; }
    if (same && m_devices->count()) return;
    const auto oldId = m_controller.selected();
    m_known = available;
    { QSignalBlocker block(m_devices); m_devices->clear(); m_devices->addItem("Select controller…", 0);
      for (const auto& device : available) m_devices->addItem(device.name, device.id);
      const int oldIndex = m_devices->findData(oldId); m_devices->setCurrentIndex(oldIndex >= 0 ? oldIndex : 0); }
    if (m_controller.selected() != 0 && m_controller.identity() == wanted && m_loadedIdentity != wanted) {
        selectDevice(m_controller.selected());
    } else if (m_devices->currentIndex() == 0 && oldId != 0) {
        // Retain the last controller's explicit safe values while it is absent.
        // Switching to a different device still loads that device's own profile.
        m_devices->setItemText(0, "Controller disconnected — reconnect or select another");
    }
}
void Window::selectDevice(SDL_JoystickID id) {
    if (!m_controller.select(id)) return;
    QSettings().setValue("controller/identity", m_controller.identity());
    m_loadedIdentity = m_controller.identity();
    QString error;
    m_mappings = m_profiles.load(m_controller.identity(), &error);
    if (error.isEmpty() && std::all_of(m_mappings.begin(), m_mappings.end(), [](const Mapping& mapping) { return mapping.source == Source::Unassigned; })) {
        if (const auto radio = m_controller.radioMappings()) {
            m_mappings = *radio;
            m_profiles.save(m_controller.identity(), m_mappings, &error);
        }
    }
    m_channels = evaluateAll(m_mappings, m_controller.snapshot());
    if (!error.isEmpty()) QMessageBox::warning(this, "Профіль пульта", error);
    for (int i = 0; i < ChannelCount; ++i) {
        m_functions[i]->setText(m_mappings[i].functionText);
        m_functions[i]->setEnabled(!m_controller.identity().isEmpty());
    }
    refreshValues();
}
void Window::refreshValues() {
    const auto& snapshot = m_controller.snapshot();
    m_status->setText(snapshot.connected ? "● CONNECTED" : "○ DISCONNECTED");
    m_status->setStyleSheet(snapshot.connected ? "color:#57d7b3;font-weight:600" : "color:#f3b76b;font-weight:600");
    m_devices->setToolTip(m_controller.description().isEmpty() ? m_controller.error() : m_controller.description());
    for (int i = 0; i < ChannelCount; ++i) {
        const auto result = evaluate(m_mappings[i], snapshot);
        m_bars[i]->setValue(m_channels[i]);
        m_table->item(i, 0)->setToolTip(QString("CH%1 · %2").arg(i + 1).arg(mappingDescription(m_mappings[i])) +
            (result.reason.isEmpty() ? QString() : "\n" + result.reason));

    }
}
void Window::configure(int index) {
    if (!m_controller.snapshot().connected) { QMessageBox::information(this, "Connect controller", "Connect and select a controller before assigning a channel."); return; }
    MappingDialog dialog(index, m_mappings[index], m_controller, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_mappings[index] = dialog.mapping();
        save(); m_channels = evaluateAll(m_mappings, m_controller.snapshot()); refreshValues();
    }
}
void Window::save() {
    QString error;
    if (!m_profiles.save(m_controller.identity(), m_mappings, &error))
        QMessageBox::warning(this, "Не вдалося зберегти профіль", error);
}
void Window::rawMonitor() {
    QDialog dialog(this); dialog.setWindowTitle("FPV GCS — Controller inputs"); dialog.resize(650, 610);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel("Для розпізнаної USB-схеми показано CH1–CH16 пульта.\nВісь HID — внутрішній номер Windows; він може мати зворотний порядок.");
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* text = new QPlainTextEdit; text->setReadOnly(true); layout->addWidget(text);
    QTimer timer;
    connect(&timer, &QTimer::timeout, &dialog, [this, text] {
        const auto& s = m_controller.snapshot(); QString value = m_controller.description();
        if (const auto radio = m_controller.radioMappings()) {
            value += "\n\nКАНАЛИ ПУЛЬТА · USB\n";
            for (int channel = 0; channel < ChannelCount; ++channel) {
                const auto& mapping = (*radio)[channel];
                if (mapping.index >= 0 && mapping.index < int(s.axes.size()))
                    value += QString("CH%1: %2  |  вісь HID %3: %4\n").arg(channel + 1, 2)
                        .arg(evaluate(mapping, s).value).arg(mapping.index + 1).arg(s.axes[mapping.index]);
            }
        } else {
            value += "\n\nФІЗИЧНІ ОСІ HID · НОМЕРИ НЕ Є RC-КАНАЛАМИ\n";
            for (int i = 0; i < int(s.axes.size()); ++i) value += QString("Axis %1: %2\n").arg(i + 1).arg(s.axes[i]);
        }
        value += "\nSWITCH INPUTS\n";
        for (int i = 0; i < int(s.buttons.size()); ++i) value += QString("Input %1: %2%3").arg(i + 1).arg(s.buttons[i] ? "ON " : "off").arg((i + 1) % 4 ? "    " : "\n");
        value += "\n\nPOV / HAT\n";
        for (int i = 0; i < int(s.hats.size()); ++i) value += QString("POV %1: %2\n").arg(i + 1).arg(s.hats[i]);
        if (s.hats.empty()) value += "None\n";
        if (!s.connected) value = "DISCONNECTED — all RC channels use safe values.";
        if (text->toPlainText() != value) text->setPlainText(value);
    }); timer.start(50);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
}
