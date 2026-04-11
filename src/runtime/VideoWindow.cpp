#include "../../include/cockscreen/runtime/VideoWindow.hpp"

#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QVideoFrame>

#include <chrono>
#include <utility>

namespace cockscreen::runtime
{

VideoWindow::VideoWindow(const ApplicationSettings &settings, QCameraDevice video_device, QString video_label,
                                                 QString format_label, QString shader_label, bool show_status_overlay, QWidget *parent)
    : QWidget{parent}, settings_{settings}, video_label_{std::move(video_label)}, shader_label_{std::move(shader_label)},
    camera_format_label_{std::move(format_label)}, show_status_overlay_{show_status_overlay}
{
    setWindowTitle(QString::fromStdString(settings_.window_title));
    resize(settings_.width, settings_.height);
    setMinimumSize(900, 540);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setCursor(Qt::BlankCursor);

    capture_session_.setVideoSink(&video_sink_);
    QObject::connect(&video_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        handle_frame(frame);
    });

    if (!video_device.isNull())
    {
        camera_ = new QCamera{video_device, this};
        if (const auto selected_format = select_camera_format(video_device, settings_.width, settings_.height);
            selected_format.has_value())
        {
            camera_->setCameraFormat(*selected_format);
            camera_format_label_ = camera_format_label(*selected_format);
        }
        else
        {
            camera_format_label_ = QStringLiteral("unknown");
        }
        capture_session_.setCamera(camera_);
    }

    if (camera_ != nullptr)
    {
        camera_->start();
        if (!camera_->isActive())
        {
            status_message_ = QStringLiteral("Video capture could not start");
        }
    }
    else
    {
        status_message_ = QStringLiteral("No video capture device was found");
    }
}

void VideoWindow::set_frame(const core::ControlFrame &frame)
{
    frame_ = frame;
    update();
}

void VideoWindow::paintEvent(QPaintEvent *)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor{2, 6, 14});

    if (!latest_frame_.isNull())
    {
        const QRect target = rect();
        const QSize image_size = latest_frame_.size();
        const QPoint top_left{(target.width() - image_size.width()) / 2, (target.height() - image_size.height()) / 2};
        painter.drawImage(top_left, latest_frame_);
    }
    else
    {
        painter.setPen(QColor{235, 241, 255});
        QFont message_font = painter.font();
        message_font.setPointSizeF(16.0);
        message_font.setBold(true);
        painter.setFont(message_font);
        painter.drawText(rect(), Qt::AlignCenter, status_message_.isEmpty() ? QStringLiteral("Waiting for video input...")
                                                                           : status_message_);
    }

    if (show_status_overlay_)
    {
        const QRect status_bar{0, height() - kStatusBarHeight, width(), kStatusBarHeight};
        painter.fillRect(status_bar, Qt::black);

        QFont status_font{"Sans Serif", 10};
        status_font.setBold(true);
        painter.setFont(status_font);
        painter.setPen(Qt::white);

        const QString status_line = QStringLiteral("FPS %1 | Gain %2 | Shader %3")
                                        .arg(processing_fps_, 0, 'f', 1)
                                        .arg(frame_.gain, 0, 'f', 2)
                                        .arg(shader_label_);
        painter.drawText(status_bar.adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, status_line);
    }
}

void VideoWindow::handle_frame(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return;
    }

    const QImage image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    latest_frame_ = image;

    const auto now = std::chrono::steady_clock::now();
    if (last_frame_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_frame_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            if (processing_fps_ <= 0.0)
            {
                processing_fps_ = instant_fps;
            }
            else
            {
                processing_fps_ = processing_fps_ * 0.85 + instant_fps * 0.15;
            }
        }
    }

    last_frame_time_ = now;
    status_message_.clear();
    update();
}

} // namespace cockscreen::runtime