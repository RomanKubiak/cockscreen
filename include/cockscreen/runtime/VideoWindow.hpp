#pragma once

#include <chrono>

#include <QtMultimedia/QCamera>
#include <QImage>
#include <QMediaCaptureSession>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>

#include "Application.hpp"
#include "RuntimeHelpers.hpp"

namespace cockscreen::runtime
{

class StatusOverlay;

class VideoWindow final : public QWidget
{
  public:
    explicit VideoWindow(const ApplicationSettings &settings, QCameraDevice video_device, QString video_label,
                         QString format_label, QString shader_label, bool show_status_overlay,
                         QWidget *parent = nullptr);

    [[nodiscard]] QString status_message() const;
    [[nodiscard]] double processing_fps() const;

    void set_status_overlay_text(QString text);

    void set_frame(const core::ControlFrame &frame);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    static constexpr int kStatusBarHeight{64};

    void handle_frame(const QVideoFrame &frame);

    ApplicationSettings settings_;
    QString video_label_;
    QString shader_label_;
    core::ControlFrame frame_;
    QCamera *camera_{nullptr};
    QMediaCaptureSession capture_session_;
    QVideoSink video_sink_;
    QImage latest_frame_;
    QString camera_format_label_{QStringLiteral("unknown")};
    QString status_message_;
    bool show_status_overlay_{true};
    std::chrono::steady_clock::time_point last_frame_time_{};
    double processing_fps_{0.0};
    QString status_overlay_text_;
    StatusOverlay *status_overlay_{nullptr};
};

} // namespace cockscreen::runtime