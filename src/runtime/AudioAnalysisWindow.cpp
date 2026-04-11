#include "../../include/cockscreen/runtime/AudioAnalysisWindow.hpp"
#include "../../include/cockscreen/runtime/RuntimeHelpers.hpp"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QIODevice>
#include <QLinearGradient>
#include <QRect>
#include <QPaintEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace cockscreen::runtime
{

namespace
{

constexpr float kKReferenceDbfs = -14.0F;
constexpr float kKDisplayBottomDb = -20.0F;
constexpr float kKDisplayTopDb = 3.0F;
constexpr float kPi = 3.14159265358979323846F;

float sample_to_float(quint8 value)
{
    return (static_cast<float>(value) - 128.0F) / 128.0F;
}

float sample_to_float(qint16 value)
{
    return static_cast<float>(value) / 32768.0F;
}

float sample_to_float(qint32 value)
{
    return static_cast<float>(value) / 2147483648.0F;
}

float sample_to_float(float value)
{
    return value;
}

} // namespace

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
        fft_window_[index] = 0.5F - 0.5F * std::cos(2.0F * kPi * phase);
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
    const float display_bottom_db = kKReferenceDbfs + kKDisplayBottomDb;
    const float display_top_db = kKReferenceDbfs + kKDisplayTopDb;
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
        draw_tick(kKReferenceDbfs, 92);
        draw_tick(display_top_db, 72);
    };

    const auto draw_channel = [&](const QRect &meter_rect, float level_dbfs) {
        draw_scale(meter_rect);

        const float k_value = level_dbfs - kKReferenceDbfs;
        const float normalized = std::clamp((k_value - kKDisplayBottomDb) / (kKDisplayTopDb - kKDisplayBottomDb), 0.0F, 1.0F);
        const int fill_height = static_cast<int>(meter_rect.height() * normalized);
        const int fill_top = meter_rect.bottom() - fill_height + 1;
        const int fill_alpha = 48 + static_cast<int>(normalized * 207.0F);
        const int peak_y = meter_rect.bottom() - static_cast<int>(std::clamp((k_value - kKDisplayBottomDb) /
                                                                              (kKDisplayTopDb - kKDisplayBottomDb),
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

void AudioAnalysisWindow::start_audio_capture()
{
    if (settings_.audio_device == "@default_output_monitor@" || settings_.audio_device == "@DEFAULT_OUTPUT_MONITOR@" ||
        settings_.audio_device == "@DEFAULT_MONITOR@")
    {
        start_monitor_capture();
        return;
    }

    const auto selected_audio_device = select_audio_input(settings_, &device_label_);
    audio_device_ = selected_audio_device.value_or(QAudioDevice{});
    if (audio_device_.isNull())
    {
        set_status_message(QStringLiteral("No audio input device found"));
        return;
    }

    audio_format_ = audio_device_.preferredFormat();
    if (audio_format_.channelCount() <= 0)
    {
        audio_format_.setChannelCount(1);
    }

    audio_source_ = new QAudioSource{audio_device_, audio_format_, this};
    audio_io_ = audio_source_->start();
    if (audio_io_ == nullptr)
    {
        set_status_message(QStringLiteral("Audio analysis could not start"));
        return;
    }

    QObject::connect(audio_io_, &QIODevice::readyRead, this, &AudioAnalysisWindow::process_audio_chunk);
    set_status_message(QStringLiteral("Input: %1").arg(device_label_.isEmpty() ? audio_device_.description() : device_label_));
}

void AudioAnalysisWindow::start_monitor_capture()
{
    using_external_capture_ = true;
    audio_format_.setChannelCount(2);
    audio_format_.setSampleRate(48000);
    audio_format_.setSampleFormat(QAudioFormat::Int16);

    audio_process_.setProgram(QStringLiteral("parec"));
    audio_process_.setArguments({QStringLiteral("-r"), QStringLiteral("-d"), QStringLiteral("@DEFAULT_MONITOR@"),
                                 QStringLiteral("--raw"), QStringLiteral("--format=s16"),
                                 QStringLiteral("--channels=2"), QStringLiteral("--rate=48000"),
                                 QStringLiteral("-")});

    QObject::connect(&audio_process_, &QProcess::readyReadStandardOutput, this, &AudioAnalysisWindow::process_audio_chunk);
    QObject::connect(&audio_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        set_status_message(QStringLiteral("Audio monitor capture could not start"));
    });

    audio_process_.start();
    if (!audio_process_.waitForStarted())
    {
        using_external_capture_ = false;
        set_status_message(QStringLiteral("Audio monitor capture could not start"));
        return;
    }

    audio_io_ = &audio_process_;
    set_status_message(QStringLiteral("Input: default output monitor"));
}

void AudioAnalysisWindow::process_audio_chunk()
{
    if (audio_io_ == nullptr)
    {
        return;
    }

    const QByteArray data = using_external_capture_ ? audio_process_.readAllStandardOutput() : audio_io_->readAll();
    if (data.isEmpty())
    {
        return;
    }

    update_levels(data);
    update();

    const auto now = std::chrono::steady_clock::now();
    if (last_profile_report_ == std::chrono::steady_clock::time_point{} || now - last_profile_report_ > std::chrono::seconds{1})
    {
        last_profile_report_ = now;
        std::cout << "Audio analysis: L=" << channel_levels_db_[0] << " dB, R=" << channel_levels_db_[1]
                  << " dB, overall=" << overall_level_db_ << " dB" << '\n';
    }
}

void AudioAnalysisWindow::update_levels(const QByteArray &data)
{
    const int sample_bytes = bytes_per_sample(audio_format_);
    const int channel_count = std::max(audio_format_.channelCount(), 1);
    if (sample_bytes <= 0 || channel_count <= 0)
    {
        return;
    }

    const int frame_bytes = sample_bytes * channel_count;
    const int frame_count = data.size() / frame_bytes;
    if (frame_count <= 0)
    {
        return;
    }

    std::array<double, 2> sum_squares{0.0, 0.0};
    int samples_per_channel = 0;

    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    for (int frame = 0; frame < frame_count; ++frame)
    {
        double mono_value_sum = 0.0;
        int mono_value_count = 0;
        for (int channel = 0; channel < std::min(channel_count, 2); ++channel)
        {
            const int offset = frame * frame_bytes + channel * sample_bytes;
            double value = 0.0;

            switch (audio_format_.sampleFormat())
            {
            case QAudioFormat::UInt8:
            {
                const auto sample = bytes[offset];
                value = sample_to_float(sample);
                break;
            }
            case QAudioFormat::Int16:
            {
                qint16 sample = 0;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = sample_to_float(sample);
                break;
            }
            case QAudioFormat::Int32:
            {
                qint32 sample = 0;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = sample_to_float(sample);
                break;
            }
            case QAudioFormat::Float:
            {
                float sample = 0.0F;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = sample_to_float(sample);
                break;
            }
            default:
                return;
            }

            sum_squares[channel] += value * value;
            mono_value_sum += value;
            ++mono_value_count;
        }

        if (mono_value_count > 0)
        {
            update_fft_analysis(static_cast<float>(mono_value_sum / static_cast<double>(mono_value_count)));
        }

        ++samples_per_channel;
    }

    for (int channel = 0; channel < 2; ++channel)
    {
        const double rms = samples_per_channel > 0 ? std::sqrt(sum_squares[channel] / samples_per_channel) : 0.0;
        const float level_db = rms_to_db(rms);
        channel_levels_db_[channel] = channel_levels_db_[channel] * 0.85F + level_db * 0.15F;
    }

    if (channel_count == 1)
    {
        channel_levels_db_[1] = channel_levels_db_[0];
    }

    overall_level_db_ = std::max(channel_levels_db_[0], channel_levels_db_[1]);
}

void AudioAnalysisWindow::update_fft_analysis(float mono_sample)
{
    if (!fft_.isValid())
    {
        return;
    }

    if (fft_sample_count_ < fft_sample_buffer_.size())
    {
        fft_sample_buffer_[fft_sample_count_++] = mono_sample;
    }

    if (fft_sample_count_ < fft_sample_buffer_.size())
    {
        return;
    }

    for (std::size_t index = 0; index < fft_sample_buffer_.size(); ++index)
    {
        fft_input_[index] = fft_sample_buffer_[index] * fft_window_[index];
    }

    fft_.forward(fft_input_, fft_spectrum_);

    constexpr float kBandCurve = 2.4F;
    const int spectrum_bins = static_cast<int>(fft_spectrum_.size());
    const int positive_bins = std::max(spectrum_bins - 1, 1);

    for (std::size_t band = 0; band < fft_band_levels_.size(); ++band)
    {
        const float start_ratio = static_cast<float>(band) / static_cast<float>(fft_band_levels_.size());
        const float end_ratio = static_cast<float>(band + 1) / static_cast<float>(fft_band_levels_.size());

        int start_bin = static_cast<int>(std::pow(start_ratio, kBandCurve) * positive_bins);
        int end_bin = static_cast<int>(std::pow(end_ratio, kBandCurve) * positive_bins);
        start_bin = std::clamp(start_bin, 0, spectrum_bins - 1);
        end_bin = std::clamp(end_bin, start_bin + 1, spectrum_bins);

        float sum = 0.0F;
        int bin_count = 0;
        for (int bin = start_bin; bin < end_bin; ++bin)
        {
            const float magnitude = std::abs(fft_spectrum_[static_cast<std::size_t>(bin)]) / static_cast<float>(kFftSize);
            sum += std::sqrt(std::max(magnitude, 0.0F));
            ++bin_count;
        }

        const float normalized = bin_count > 0 ? std::clamp((sum / static_cast<float>(bin_count)) * 1.8F, 0.0F, 1.0F) : 0.0F;
        fft_band_levels_[band] = fft_band_levels_[band] * 0.82F + normalized * 0.18F;
    }

    fft_sample_count_ = fft_sample_buffer_.size() / 2;
    std::move(fft_sample_buffer_.begin() + static_cast<std::ptrdiff_t>(fft_sample_buffer_.size() / 2),
              fft_sample_buffer_.end(),
              fft_sample_buffer_.begin());
}

void AudioAnalysisWindow::set_status_message(QString message)
{
    status_message_ = std::move(message);
    update();
}

float AudioAnalysisWindow::rms_to_db(double rms)
{
    if (rms <= std::numeric_limits<double>::epsilon())
    {
        return kSilenceDb;
    }

    return static_cast<float>(20.0 * std::log10(rms));
}

int AudioAnalysisWindow::bytes_per_sample(const QAudioFormat &format)
{
    switch (format.sampleFormat())
    {
    case QAudioFormat::UInt8:
        return 1;
    case QAudioFormat::Int16:
        return 2;
    case QAudioFormat::Int32:
    case QAudioFormat::Float:
        return 4;
    default:
        return 0;
    }
}

} // namespace cockscreen::runtime