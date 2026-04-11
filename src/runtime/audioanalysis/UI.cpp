#include "cockscreen/runtime/AudioAnalysisWindow.hpp"

#include "cockscreen/runtime/audioanalysis/Support.hpp"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QRect>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace cockscreen::runtime
{

AudioAnalysisWindow::AudioAnalysisWindow(const ApplicationSettings &settings, QWidget *parent)
    : QWidget{parent}, settings_{settings}
{
    setWindowTitle(QString());
    if (parent == nullptr)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    }
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setMinimumSize(kMeterWidth, kMeterHeight);
    resize(kMeterWidth, kMeterHeight);

    for (int index = 0; index < kFftSize; ++index)
    {
        const float phase = static_cast<float>(index) / static_cast<float>(kFftSize - 1);
        fft_window_[index] = 0.5F - 0.5F * std::cos(2.0F * audio_analysis::kPi * phase);
    }

    start_audio_capture();
}

float AudioAnalysisWindow::left_level_db() const
{
    return channel_levels_db_[0];
}

float AudioAnalysisWindow::right_level_db() const
{
    return channel_levels_db_[1];
}

float AudioAnalysisWindow::overall_level_db() const
{
    return overall_level_db_;
}

const std::array<float, core::kAudioFftBandCount> &AudioAnalysisWindow::fft_bands() const
{
    return fft_band_levels_;
}

QString AudioAnalysisWindow::status_message() const
{
    return status_message_;
}

void AudioAnalysisWindow::place_on_screen(QScreen *screen)
{
    if (parentWidget() != nullptr)
    {
        const QWidget *host = parentWidget();
        const int margin = 24;
        move(host->width() - width() - margin, margin);
        show();
        raise();
        return;
    }

    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }

    if (screen == nullptr)
    {
        show();
        return;
    }

    const QRect available = screen->availableGeometry();
    const int margin = 24;
    move(available.x() + available.width() - width() - margin, available.y() + margin);
    show();
}

void AudioAnalysisWindow::paintEvent(QPaintEvent *)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor{0, 0, 0, 0});

    const QRect content = rect().adjusted(14, 14, -14, -14);
    const int column_gap = 12;
    const int column_width = std::max(24, (content.width() - column_gap) / 2);
    const int meter_height = content.height();
    const float display_bottom_db = audio_analysis::kReferenceDbfs + audio_analysis::kDisplayBottomDb;
    const float display_top_db = audio_analysis::kReferenceDbfs + audio_analysis::kDisplayTopDb;
    const float display_range_db = std::max(display_top_db - display_bottom_db, 1.0F);

    const auto draw_scale = [&](const QRect &meter_rect) {
        painter.setPen(QColor{255, 255, 255, 28});
        painter.drawRect(meter_rect.adjusted(0, 0, -1, -1));

        const auto draw_tick = [&](float dbfs, int alpha) {
            const float ratio = std::clamp((dbfs - display_bottom_db) / display_range_db, 0.0F, 1.0F);
            const int y = meter_rect.bottom() - static_cast<int>(ratio * meter_rect.height());
            painter.setPen(QColor{255, 255, 255, alpha});
            painter.drawLine(meter_rect.left(), y, meter_rect.right(), y);
        };

        draw_tick(display_bottom_db, 32);
        draw_tick(audio_analysis::kReferenceDbfs, 92);
        draw_tick(display_top_db, 72);
    };

    const auto draw_channel = [&](const QRect &meter_rect, float level_dbfs) {
        draw_scale(meter_rect);

        const float k_value = level_dbfs - audio_analysis::kReferenceDbfs;
        const float normalized = std::clamp((k_value - audio_analysis::kDisplayBottomDb) /
                                                (audio_analysis::kDisplayTopDb - audio_analysis::kDisplayBottomDb),
                                            0.0F, 1.0F);
        const int fill_height = static_cast<int>(meter_rect.height() * normalized);
        const int fill_top = meter_rect.bottom() - fill_height + 1;
        const int fill_alpha = 48 + static_cast<int>(normalized * 207.0F);
        const int peak_y = meter_rect.bottom() - static_cast<int>(std::clamp((k_value - audio_analysis::kDisplayBottomDb) /
                                                                                  (audio_analysis::kDisplayTopDb - audio_analysis::kDisplayBottomDb),
                                                                              0.0F, 1.0F) * meter_rect.height());

        QLinearGradient fill_gradient(static_cast<qreal>(meter_rect.left()), static_cast<qreal>(meter_rect.bottom()),
                                      static_cast<qreal>(meter_rect.left()), static_cast<qreal>(meter_rect.top()));
        fill_gradient.setColorAt(0.0, QColor{36, 210, 92, fill_alpha});
        fill_gradient.setColorAt(0.70, QColor{210, 208, 38, fill_alpha});
        fill_gradient.setColorAt(1.0, QColor{255, 72, 48, fill_alpha});

        painter.fillRect(QRect{meter_rect.left() + 1, fill_top, meter_rect.width() - 2, fill_height}, fill_gradient);
        painter.fillRect(QRect{meter_rect.left() + 1, meter_rect.top() + 1, meter_rect.width() - 2,
                               meter_rect.height() - fill_height - 1},
                         QColor{0, 0, 0, 30});

        painter.setPen(QColor{255, 255, 255, 200});
        painter.drawLine(meter_rect.left() + 1, peak_y, meter_rect.right() - 1, peak_y);
    };

    const QRect left_meter{content.left(), content.top(), column_width, meter_height};
    const QRect right_meter{content.left() + column_width + column_gap, content.top(), column_width, meter_height};

    draw_channel(left_meter, channel_levels_db_[0]);
    draw_channel(right_meter, channel_levels_db_[1]);
}

void AudioAnalysisWindow::set_status_message(QString message)
{
    status_message_ = std::move(message);
    update();
}

} // namespace cockscreen::runtime