#include "capture.h"
#include <QDateTime>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QMap>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionSpinBox>
#include <QScreen>
#include <cmath>
#include <limits>

namespace fpv {
namespace {
class CaptureDeviceBox final : public QComboBox {
protected:
    void showPopup() override {
        QComboBox::showPopup();
        auto* popup = view()->window();
        const auto bounds = screen()->availableGeometry();
        const QPoint below = mapToGlobal(QPoint(0, height()));
        // Qt's menu-style popup otherwise aligns the selected item with the
        // field, which makes the camera list appear above the selector.
        popup->resize(popup->width(), qMin(popup->height(), qMax(1, bounds.bottom()-below.y()+1)));
        popup->move(qMax(bounds.left(), qMin(below.x(), bounds.right()-popup->width()+1)), below.y());
    }
};

QString captureNotice(const QJsonObject& state) {
    const auto notice = state.value("notice").toString();
    return notice.startsWith("Camera selected:") ? QString() : notice;
}

template<class Base>
class CaptureNumber final : public Base {
public:
    CaptureNumber() {
        this->setFixedWidth(112);
        this->setFixedHeight(34);
        this->setStyleSheet(
            "QAbstractSpinBox { background:#1c2c3c; border:1px solid #35516c; border-radius:4px; padding:5px; padding-right:26px; }"
            "QAbstractSpinBox::up-button { subcontrol-origin:border; subcontrol-position:top right; width:22px; height:16px; border-left:1px solid #35516c; background:#22374b; }"
            "QAbstractSpinBox::down-button { subcontrol-origin:border; subcontrol-position:bottom right; width:22px; height:16px; border-left:1px solid #35516c; background:#22374b; }"
            "QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background:#30516d; }"
            "QAbstractSpinBox::up-arrow, QAbstractSpinBox::down-arrow { image:none; }");
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        Base::paintEvent(event);
        QStyleOptionSpinBox option; this->initStyleOption(&option);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        for (const bool up : {true, false}) {
            const auto rect = this->style()->subControlRect(QStyle::CC_SpinBox, &option,
                up ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown, this);
            const QPointF center = QRectF(rect).center();
            const bool enabled = this->isEnabled() && (option.stepEnabled &
                (up ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled));
            painter.setPen(QPen(QColor(enabled ? "#dce7f1" : "#647486"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const double direction = up ? -1 : 1;
            QPainterPath arrow;
            arrow.moveTo(center.x()-3.5, center.y()-direction*1.5);
            arrow.lineTo(center.x(), center.y()+direction*2);
            arrow.lineTo(center.x()+3.5, center.y()-direction*1.5);
            painter.drawPath(arrow);
        }
    }
};

QIcon gearIcon() {
    QPixmap image(32, 32); image.fill(Qt::transparent);
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    for (int i = 0; i < 64; ++i) {
        const double angle = i * 6.28318530718 / 64;
        const double r = (i % 8 >= 2 && i % 8 <= 5) ? 14 : 11;
        const QPointF p(16 + r * std::cos(angle), 16 + r * std::sin(angle));
        if (i == 0) path.moveTo(p); else path.lineTo(p);
    }
    path.closeSubpath(); path.addEllipse(QPointF(16, 16), 5, 5);
    path.setFillRule(Qt::OddEvenFill); painter.fillPath(path, QColor("#c7c7c7"));
    return QIcon(image);
}
void populateDevices(QComboBox* box, CaptureClient* client) {
    QSignalBlocker blocker(box);
    const auto signature = QJsonDocument(QJsonObject{{"devices", client->state.value("devices")}, {"online", client->online}}).toJson(QJsonDocument::Compact);
    const auto active = client->state.value("active").toString();
    if (box->property("captureDevices").toByteArray() != signature) {
      box->setProperty("captureDevices", signature); box->clear();
      for (const auto& item : client->state.value("devices").toArray()) {
        const auto d = item.toObject();
        const bool available = d.value("available").toBool();
        box->addItem(d.value("name").toString() + (available ? "" : " — unavailable"), d.value("id").toString());
        box->setItemData(box->count()-1, d.value("id").toString(), Qt::ToolTipRole);
        if (!available) static_cast<QStandardItemModel*>(box->model())->item(box->count()-1)->setEnabled(false);
      }
      if (box->findData(active) < 0) box->insertItem(0, client->online ? "No capture devices" : "Pi video API unavailable");
    }
    const int selected = box->findData(active);
    if (!box->view()->isVisible()) box->setCurrentIndex(selected >= 0 ? selected : 0);
    box->setEnabled(client->online && !client->busy());
}

class CaptureDialog final : public QDialog {
public:
    explicit CaptureDialog(CaptureClient* client, QWidget* parent) : QDialog(parent), c(client) {
        setWindowTitle("Video Capture Settings"); resize(760, 800); setMinimumSize(540, 480);
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(this);
        devices = new CaptureDeviceBox; layout->addWidget(devices);
        message = new QLabel; message->setWordWrap(true); layout->addWidget(message);
        scroll = new QScrollArea; scroll->setWidgetResizable(true); layout->addWidget(scroll, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        auto* refresh = buttons->addButton("Refresh devices", QDialogButtonBox::ActionRole);
        connect(refresh, &QPushButton::clicked, this, [this] { c->request("refresh"); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        layout->addWidget(buttons);
        connect(devices, &QComboBox::activated, this, [this](int i) {
            const auto id = devices->itemData(i).toString();
            if (!id.isEmpty()) c->request("select", {{"device", id}});
        });
        connect(c, &CaptureClient::changed, this, [this] { updateState(); });
        connect(c, &CaptureClient::failed, this, [this](const QString& error) { message->setText(error); });
        updateState();
    }
private:
    CaptureClient* c;
    QComboBox* devices;
    QLabel* message;
    QScrollArea* scroll;
    QByteArray signature;
    QLabel* status = nullptr;
    QJsonObject selectedFormat;
    QMap<QString, QWidget*> imageWidgets;
    QString renderedDevice;
    QComboBox *pixels = nullptr, *sizes = nullptr, *rates = nullptr, *inputs = nullptr, *standards = nullptr;
    QSpinBox *width = nullptr, *height = nullptr;
    QWidget* contents = nullptr;
    QJsonArray formats;
    QJsonObject devicePayload() const { return {{"device", c->state.value("active")}}; }
    QFormLayout* section(QVBoxLayout* parent, const QString& title) {
        auto* group = new QGroupBox(title); parent->addWidget(group);
        auto* form = new QFormLayout(group); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return form;
    }
    void updateState() {
        populateDevices(devices, c);
        message->setText(!c->online ? "Cannot reach Raspberry Pi video management API." :
                        c->busy() ? "Applying video settings…" : c->state.value("error").toString());
        const auto detail = c->state.value("detail").toObject();
        // Values update in place: recreating the scroll widget on every image
        // adjustment resets scrolling, focus and pending capture-format edits.
        auto capabilities = detail;
        auto controls = capabilities.value("controls").toArray();
        for (int i = 0; i < controls.size(); ++i) {
            auto spec = controls[i].toObject(); spec.remove("value"); controls[i] = spec;
        }
        capabilities["controls"] = controls;
        const auto next = QJsonDocument(QJsonObject{{"active", c->state.value("active")}, {"detail", capabilities}}).toJson(QJsonDocument::Compact);
        if (signature != next) {
            const auto active = c->state.value("active").toString();
            const bool sameDevice = renderedDevice == active;
            const int position = sameDevice ? scroll->verticalScrollBar()->value() : 0;
            signature = next; renderedDevice = active;
            rebuild(detail);
            if (sameDevice) QTimer::singleShot(0, contents, [this, position] {
                contents->layout()->activate(); scroll->verticalScrollBar()->setValue(position);
            });
        }
        for (const auto& item : detail.value("controls").toArray()) {
            const auto spec = item.toObject();
            auto* widget = imageWidgets.value(spec.value("name").toString(), nullptr);
            if (!widget || widget->property("actualValue") == spec.value("value").toVariant()) continue;
            const auto value = spec.value("value");
            widget->setProperty("actualValue", value.toVariant());
            if (auto* combo = qobject_cast<QComboBox*>(widget)) {
                QSignalBlocker block(combo); combo->setCurrentIndex(combo->findData(value.toVariant()));
            } else if (auto* check = qobject_cast<QCheckBox*>(widget)) {
                QSignalBlocker block(check); check->setChecked(value.toInt() != 0);
            } else if (auto* spin = widget->findChild<QSpinBox*>()) {
                QSignalBlocker block(spin); spin->setValue(value.toInt());
                if (auto* slider = widget->findChild<QSlider*>()) {
                    QSignalBlocker sliderBlock(slider);
                    slider->setValue((value.toInt()-spec.value("min").toInt())/qMax(1, spec.value("step").toInt(1)));
                }
            }
        }
        if (contents) contents->setEnabled(c->online && !c->busy() && !c->state.value("active").toString().isEmpty());
        if (status) {
            const auto actual = detail.value("actual").toObject();
            QString bitrate;
            for (const auto& property : c->state.value("encoding").toArray())
                if (property.toObject().value("name").toString() == "bitrate")
                    bitrate = QString::number(property.toObject().value("value").toDouble()/1000000, 'f', 2) + " Mbps (encoder target)";
            const auto measured = c->state.value("bitrate_bps");
            if (measured.isDouble()) bitrate += QString("\nCurrent RTP output: %1 Mbps").arg(measured.toDouble()/1000000, 0, 'f', 2);
            const auto dropped = c->state.value("sink_dropped_frames");
            if (dropped.isDouble()) bitrate += QString("\nTransport sink dropped frames: %1").arg(dropped.toInteger());
            status->setText(QString("Active device: %1\nNegotiated: %2\nCapture: %3 × %4 / %5 FPS\nEncoder: %6\nStream state: %7\n%8\n%9\n%10")
                .arg(c->state.value("active").toString(), c->state.value("negotiated").toString("Not negotiated"),
                     QString::number(actual.value("width").toInt()), QString::number(actual.value("height").toInt()),
                     actual.value("fps").toString(), c->state.value("encoder").toString(),
                     c->state.value("stream_state").toString(), captureNotice(c->state), c->state.value("error").toString(), bitrate));
        }
    }
    QWidget* control(const QJsonObject& spec, std::function<void(QJsonValue)> change, bool imageControl) {
        const auto type = spec.value("type").toString();
        const auto value = spec.value("value");
        QWidget* widget;
        if (!spec.value("menu").toArray().isEmpty()) {
            auto* box = new QComboBox;
            for (const auto& item : spec.value("menu").toArray()) {
                const auto m = item.toObject(); box->addItem(m.value("label").toString(), m.value("value").toVariant());
            }
            box->setCurrentIndex(box->findData(value.toVariant()));
            connect(box, &QComboBox::activated, this, [box, change](int) { change(QJsonValue::fromVariant(box->currentData())); });
            widget = box;
        } else if (type == "bool") {
            auto* box = new QCheckBox; box->setChecked(value.toInt() != 0);
            connect(box, &QCheckBox::clicked, this, [change](bool enabled) { change(int(enabled)); }); widget = box;
        } else if (type == "button") {
            auto* button = new QPushButton("Execute");
            connect(button, &QPushButton::clicked, this, [change] { change(0); }); widget = button;
        } else if ((type == "int" || type == "int64" || type == "bitmask") && spec.contains("min") && spec.contains("max")) {
            const auto min = spec.value("min").toDouble(), max = spec.value("max").toDouble();
            const int step = qMax(1, spec.value("step").toInt(1));
            if (min >= std::numeric_limits<int>::min() && max <= std::numeric_limits<int>::max()) {
                auto* row = new QWidget; auto* line = new QHBoxLayout(row); line->setContentsMargins(0, 0, 0, 0);
                auto* spin = new CaptureNumber<QSpinBox>; spin->setRange(int(min), int(max)); spin->setSingleStep(step); spin->setValue(value.toInt());
                if (imageControl && max-min <= 1000000) {
                    auto* slider = new QSlider(Qt::Horizontal);
                    slider->setRange(0, int((max-min)/step)); slider->setValue((value.toInt()-int(min))/step);
                    connect(slider, &QSlider::valueChanged, spin, [spin, min, step](int v) { spin->setValue(int(min) + v*step); });
                    auto* debounce = new QTimer(slider); debounce->setSingleShot(true); debounce->setInterval(180);
                    connect(debounce, &QTimer::timeout, this, [spin, change] { change(spin->value()); });
                    connect(slider, &QSlider::valueChanged, debounce, [slider, debounce](int) { if (!slider->isSliderDown()) debounce->start(); });
                    connect(slider, &QSlider::sliderReleased, debounce, [debounce] { debounce->start(0); });
                    connect(spin, &QSpinBox::valueChanged, slider, [slider, min, step](int v) { QSignalBlocker b(slider); slider->setValue((v-int(min))/step); });
                    line->addWidget(slider, 1);
                }
                connect(spin, &QSpinBox::editingFinished, this, [spin, change] { change(spin->value()); });
                line->addWidget(spin); widget = row;
            } else if (min >= -9007199254740991.0 && max <= 9007199254740991.0) {
                auto* spin = new CaptureNumber<QDoubleSpinBox>;
                spin->setDecimals(0); spin->setRange(min, max); spin->setSingleStep(step); spin->setValue(value.toDouble());
                connect(spin, &QDoubleSpinBox::editingFinished, this, [spin, change] { change(QJsonValue(qint64(spin->value()))); });
                widget = spin;
            } else {
                // JSON numeric precision is limited to 53 bits. Larger integer64
                // values remain visible rather than being rounded on write.
                auto* line = new QLineEdit(QString::number(value.toDouble(), 'f', 0));
                line->setReadOnly(true); widget = line;
            }
        } else {
            widget = new QLabel(value.isString() ? value.toString() : QString::fromUtf8(QJsonDocument(spec).toJson(QJsonDocument::Compact)));
            static_cast<QLabel*>(widget)->setWordWrap(true);
        }
        widget->setToolTip(QString("%1\nmin %2 · max %3 · step %4 · default %5\n%6")
            .arg(spec.value("name").toString(), spec.value("min").toVariant().toString(), spec.value("max").toVariant().toString(),
                 spec.value("step").toVariant().toString(), spec.value("default").toVariant().toString(), spec.value("flags").toString()));
        if (imageControl && !spec.value("writable").toBool()) widget->setEnabled(false);
        return widget;
    }
    void rebuild(const QJsonObject& detail) {
        if (auto* old = scroll->takeWidget()) old->deleteLater();
        imageWidgets.clear();
        contents = new QWidget; auto* layout = new QVBoxLayout(contents);
        auto* device = section(layout, "Capture Device");
        for (const auto& item : c->state.value("devices").toArray()) {
            const auto d = item.toObject();
            if (d.value("id") != c->state.value("active")) continue;
            device->addRow("Device name", new QLabel(d.value("name").toString()));
            device->addRow("Driver", new QLabel(d.value("driver").toString()));
            device->addRow("VID / PID", new QLabel(d.value("vid").toString() + " / " + d.value("pid").toString()));
            device->addRow("Current status", new QLabel(d.value("available").toBool() ? "USB connected" : "Unavailable"));
        }
        auto* format = section(layout, "Capture Format");
        formats = detail.value("formats").toArray(); selectedFormat = detail.value("actual").toObject();
        pixels = new QComboBox; sizes = new QComboBox; rates = new QComboBox; rates->setEditable(true);
        pixels->setObjectName("capturePixelFormat"); sizes->setObjectName("captureResolution"); rates->setObjectName("captureFps");
        rates->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]+([.][0-9]+)?"), rates));
        for (const auto& f : formats) pixels->addItem(f.toObject().value("pixel").toString());
        pixels->setCurrentText(selectedFormat.value("pixel").toString());
        width = new QSpinBox; height = new QSpinBox;
        width->setObjectName("captureWidth"); height->setObjectName("captureHeight");
        width->setRange(1, 65535); height->setRange(1, 65535);
        auto* dimensions = new QWidget; auto* dimensionRow = new QHBoxLayout(dimensions); dimensionRow->setContentsMargins(0,0,0,0);
        dimensionRow->addWidget(width); dimensionRow->addWidget(new QLabel("×")); dimensionRow->addWidget(height);
        format->addRow("Pixel format", pixels); format->addRow("Resolution", sizes); format->addRow("Width / height", dimensions); format->addRow("FPS", rates);
        auto updateRate = [this] {
            rates->clear();
            const auto s = sizes->currentData().toJsonObject();
            const auto range = s.value("range").toObject();
            width->setRange(range.isEmpty() ? s.value("width").toInt(1) : range.value("min_width").toInt(), range.isEmpty() ? s.value("width").toInt(1) : range.value("max_width").toInt());
            height->setRange(range.isEmpty() ? s.value("height").toInt(1) : range.value("min_height").toInt(), range.isEmpty() ? s.value("height").toInt(1) : range.value("max_height").toInt());
            width->setSingleStep(range.value("step_width").toInt(1)); height->setSingleStep(range.value("step_height").toInt(1));
            width->setValue(selectedFormat.value("width").toInt()); height->setValue(selectedFormat.value("height").toInt());
            for (const auto& r : s.value("rates").toArray()) rates->addItem(r.toString());
            const auto previousRate = selectedFormat.value("fps").toString();
            if (rates->findText(previousRate) >= 0 || rates->count() == 0) rates->setCurrentText(previousRate);
            rates->setToolTip(QString::fromUtf8(QJsonDocument(s.value("interval_range").toObject()).toJson(QJsonDocument::Compact)));
        };
        auto updateSizes = [this, updateRate] {
            QSignalBlocker blocker(sizes); sizes->clear();
            for (const auto& f : formats) if (f.toObject().value("pixel").toString() == pixels->currentText()) {
                for (const auto& item : f.toObject().value("sizes").toArray()) {
                    auto s = item.toObject();
                    sizes->addItem(QString("%1 × %2%3").arg(s.value("width").toInt()).arg(s.value("height").toInt()).arg(s.contains("range") ? " (variable)" : ""), QVariant::fromValue(s));
                    if (s.contains("range") || (s.value("width") == selectedFormat.value("width") && s.value("height") == selectedFormat.value("height"))) sizes->setCurrentIndex(sizes->count()-1);
                }
            }
            updateRate();
        };
        connect(pixels, &QComboBox::currentIndexChanged, this, [updateSizes](int) { updateSizes(); });
        connect(sizes, &QComboBox::currentIndexChanged, this, [updateRate](int) { updateRate(); }); updateSizes();
        auto options = [this, format](const QString& label, const QJsonArray& values, const QString& current) {
            auto* box = new QComboBox;
            box->addItem("Keep current: " + current, QVariant());
            for (const auto& v : values) { const auto m = v.toObject(); box->addItem(m.value("label").toString(), m.value("value").toVariant()); }
            box->setVisible(!values.isEmpty()); if (!values.isEmpty()) format->addRow(label, box);
            else box->setParent(contents);
            return box;
        };
        inputs = options("Physical input", detail.value("inputs").toArray(), selectedFormat.value("input").toString());
        standards = options("PAL / NTSC", detail.value("standards").toArray(), selectedFormat.value("standard").toString());
        auto* apply = new QPushButton("Apply capture format"); apply->setObjectName("applyCaptureFormat"); format->addRow(apply);
        connect(apply, &QPushButton::clicked, this, [this] {
            auto p = devicePayload();
            p["format"] = QJsonObject{{"pixel", pixels->currentText()}, {"width", width->value()}, {"height", height->value()}, {"fps", rates->currentText()}};
            if (inputs->currentData().isValid()) p["input"] = QJsonValue::fromVariant(inputs->currentData());
            if (standards->currentData().isValid()) p["standard"] = QJsonValue::fromVariant(standards->currentData());
            c->request("configure", p);
        });
        auto* image = section(layout, "Image Controls");
        for (const auto& item : detail.value("controls").toArray()) {
            const auto spec = item.toObject(); const auto name = spec.value("name").toString();
            auto label = name; label.replace('_', ' '); if (!label.isEmpty()) label[0] = label[0].toUpper();
            auto* widget = control(spec, [this, name](QJsonValue v) {
                auto p = devicePayload(); p["values"] = QJsonObject{{name, v}}; c->request("controls", p);
            }, true);
            widget->setObjectName("imageControl_" + name); widget->setProperty("actualValue", spec.value("value").toVariant());
            imageWidgets[name] = widget; image->addRow(label, widget);
        }
        auto* reset = new QPushButton("Restore image defaults"); image->addRow(reset);
        reset->setEnabled(!detail.value("controls").toArray().isEmpty());
        connect(reset, &QPushButton::clicked, this, [this] { c->request("reset", devicePayload()); });
        auto* stream = section(layout, "Stream Status"); status = new QLabel; status->setWordWrap(true); status->setTextInteractionFlags(Qt::TextSelectableByMouse); stream->addRow(status);
        // Missing counters are deliberately not replaced with fabricated zeros.
        layout->addStretch(); scroll->setWidget(contents);
    }
};
}

CaptureClient::CaptureClient(QObject* parent) : QObject(parent), m_network(this) {
    m_base = qEnvironmentVariable("FPV_VIDEO_API_URL", "http://192.168.144.20:5603");
    connect(&m_timer, &QTimer::timeout, this, &CaptureClient::poll); m_timer.start(1000);
    QTimer::singleShot(0, this, &CaptureClient::poll);
}
void CaptureClient::poll() {
    if (m_polling) return;
    m_polling = true;
    QNetworkRequest request{QUrl(m_base + "/state")}; request.setTransferTimeout(2500);
    auto* reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_polling = false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &error);
        online = reply->error() == QNetworkReply::NoError && error.error == QJsonParseError::NoError && doc.isObject() && doc.object().contains("devices");
        if (online) {
            state = doc.object();
            const auto result = state.value("operations").toObject().value(m_operation).toObject();
            if (!m_operation.isEmpty() && result.value("done").toBool()) {
                m_operation.clear(); const auto message = result.value("error").toString(); if (!message.isEmpty()) emit failed(message);
            }
        }
        if (!m_operation.isEmpty() && QDateTime::currentMSecsSinceEpoch()-m_started > 30000) {
            m_operation.clear(); emit failed("Video operation timed out; refresh to inspect applied values.");
        }
        reply->deleteLater(); emit changed();
    });
}
void CaptureClient::request(const QString& action, QJsonObject payload) {
    if (busy() || !online) return;
    m_posting = true;
    QNetworkRequest request{QUrl(m_base + "/" + action)}; request.setTransferTimeout(3000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = m_network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_posting = false;
        const auto response = QJsonDocument::fromJson(reply->readAll()).object();
        m_operation = response.value("operation").toString(); m_started = QDateTime::currentMSecsSinceEpoch();
        if (reply->error() != QNetworkReply::NoError || m_operation.isEmpty()) emit failed(response.value("error").toString(reply->errorString()));
        reply->deleteLater(); emit changed(); poll();
    });
}

CaptureBar::CaptureBar(QWidget* parent) : QWidget(parent), m_client(new CaptureClient(this)) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
    auto* row = new QHBoxLayout; row->setSpacing(6);
    m_devices = new CaptureDeviceBox; m_devices->setMinimumContentsLength(14);
    m_devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->addWidget(m_devices, 1);
    auto* settings = new QToolButton; settings->setIcon(gearIcon()); settings->setToolTip("Video Capture Settings"); settings->setAccessibleName("Video Capture Settings");
    row->addWidget(settings);
    m_status = new QLabel("NO SIGNAL"); m_status->setStyleSheet("color:#f3b76b;font-size:10px"); row->addWidget(m_status);
    auto* endpoint = new QLabel("H.264 / RTP / UDP · 192.168.144.3:5602"); endpoint->setObjectName("subtitle");
    endpoint->setAlignment(Qt::AlignRight | Qt::AlignVCenter); layout->addLayout(row); layout->addWidget(endpoint);
    m_notice = new QLabel; m_notice->setWordWrap(true); m_notice->hide(); layout->addWidget(m_notice);
    connect(settings, &QToolButton::clicked, this, &CaptureBar::settings);
    connect(m_devices, &QComboBox::activated, this, [this](int i) {
        const auto id = m_devices->itemData(i).toString(); if (!id.isEmpty()) m_client->request("select", {{"device", id}});
    });
    connect(m_client, &CaptureClient::changed, this, [this] {
        populateDevices(m_devices, m_client);
        const auto error = m_client->state.value("error").toString();
        const auto notice = !m_client->online ? "Pi video management unavailable" : error.isEmpty() ? captureNotice(m_client->state) : error;
        m_devices->setToolTip(notice);
        m_notice->setText(notice); m_notice->setVisible(!notice.isEmpty());
    });
    connect(m_client, &CaptureClient::failed, this, [this](const QString& error) { m_notice->setText(error); m_notice->show(); });
}
void CaptureBar::videoStatus(bool connected, const QString& detail) {
    m_status->setText(connected ? "LIVE" : "NO SIGNAL"); m_status->setToolTip(detail);
    m_status->setStyleSheet(connected ? "color:#57d7b3;font-size:10px" : "color:#f3b76b;font-size:10px");
}
void CaptureBar::settings() {
    if (!m_dialog) {
        m_dialog = new CaptureDialog(m_client, this);
        connect(m_dialog, &QObject::destroyed, this, [this] { m_dialog = nullptr; });
    }
    m_dialog->show(); m_dialog->raise(); m_dialog->activateWindow();
}
}
