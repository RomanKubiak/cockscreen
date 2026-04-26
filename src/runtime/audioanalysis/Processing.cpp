#include "cockscreen/runtime/AudioAnalysisWindow.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/audioanalysis/Support.hpp"
#ifdef _WIN32
#include "cockscreen/runtime/audioanalysis/WasapiLoopback.hpp"
#endif

#include <QByteArray>
#include <QIODevice>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace cockscreen::runtime
{

namespace
{

#ifndef _WIN32

struct PulseSourceCaptureFormat
{
    int channel_count{2};
    int sample_rate{48000};
};

struct ExternalMonitorCaptureCommand
{
    QString program;
    QStringList arguments;
    QString backend_name;
};

ExternalMonitorCaptureCommand build_external_monitor_capture_command(const QString &source_name, int channel_count,
                                                                    int sample_rate)
{
    if (!QStandardPaths::findExecutable(QStringLiteral("pw-record")).isEmpty())
    {
        return ExternalMonitorCaptureCommand{QStringLiteral("pw-record"),
                                             QStringList{QStringLiteral("--raw"),
                                                         QStringLiteral("--format"),
                                                         QStringLiteral("s16"),
                                                         QStringLiteral("--channels"),
                                                         QString::number(channel_count),
                                                         QStringLiteral("--rate"),
                                                         QString::number(sample_rate),
                                                         QStringLiteral("--target"),
                                                         source_name,
                                                         QStringLiteral("-")},
                                             QStringLiteral("pw-record")};
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("pw-cat")).isEmpty())
    {
        return ExternalMonitorCaptureCommand{QStringLiteral("pw-cat"),
                                             QStringList{QStringLiteral("--record"),
                                                         QStringLiteral("--raw"),
                                                         QStringLiteral("--format"),
                                                         QStringLiteral("s16"),
                                                         QStringLiteral("--channels"),
                                                         QString::number(channel_count),
                                                         QStringLiteral("--rate"),
                                                         QString::number(sample_rate),
                                                         QStringLiteral("--target"),
                                                         source_name,
                                                         QStringLiteral("-")},
                                             QStringLiteral("pw-cat")};
    }

    return ExternalMonitorCaptureCommand{QStringLiteral("parec"),
                                         QStringList{QStringLiteral("-r"),
                                                     QStringLiteral("-d"),
                                                     source_name,
                                                     QStringLiteral("--raw"),
                                                     QStringLiteral("--format=s16"),
                                                     QStringLiteral("--channels=%1").arg(channel_count),
                                                     QStringLiteral("--rate=%1").arg(sample_rate),
                                                     QStringLiteral("-")},
                                         QStringLiteral("parec")};
}

std::optional<PulseSourceCaptureFormat> detect_pulse_source_capture_format(const QString &source_name)
{
    if (source_name.isEmpty())
    {
        return std::nullopt;
    }

    QProcess sources_process;
    sources_process.start(QStringLiteral("pactl"),
                          QStringList{QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")});
    if (!sources_process.waitForFinished(1000) || sources_process.exitStatus() != QProcess::NormalExit ||
        sources_process.exitCode() != 0)
    {
        return std::nullopt;
    }

    const QString sources_output = QString::fromUtf8(sources_process.readAllStandardOutput());
    for (const auto &line : sources_output.split('\n', Qt::SkipEmptyParts))
    {
        const auto fields = line.simplified().split(' ', Qt::SkipEmptyParts);
        if (fields.size() < 6 || fields[1] != source_name)
        {
            continue;
        }

        const QString channels_field = fields[4];
        const QString rate_field = fields[5];
        if (!channels_field.endsWith(QStringLiteral("ch")) || !rate_field.endsWith(QStringLiteral("Hz")))
        {
            continue;
        }

        bool channel_count_ok = false;
        bool sample_rate_ok = false;
        const int channel_count = channels_field.left(channels_field.size() - 2).toInt(&channel_count_ok);
        const int sample_rate = rate_field.left(rate_field.size() - 2).toInt(&sample_rate_ok);
        if (!channel_count_ok || !sample_rate_ok || channel_count <= 0 || sample_rate <= 0)
        {
            continue;
        }

        return PulseSourceCaptureFormat{channel_count, sample_rate};
    }

    return std::nullopt;
}

#endif // !_WIN32

} // namespace

void AudioAnalysisWindow::start_audio_capture()
{
    if (is_default_output_monitor_token(settings_.audio_device))
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
    set_status_message(
        QStringLiteral("Opened input: %1").arg(device_label_.isEmpty() ? audio_device_.description() : device_label_));
}

void AudioAnalysisWindow::start_monitor_capture()
{
#ifdef _WIN32
    wasapi_loopback_ = new audio_analysis::WasapiLoopbackCapture{this};
    wasapi_loopback_->setAudioDataCallback([this](QByteArray data) {
        process_audio_buffer(std::move(data));
    });
    if (!wasapi_loopback_->start())
    {
        set_status_message(wasapi_loopback_->errorString());
        delete wasapi_loopback_;
        wasapi_loopback_ = nullptr;
        return;
    }

    audio_format_ = wasapi_loopback_->format();
    set_status_message(QStringLiteral("Opened monitor: default output monitor (WASAPI loopback)"));
#else
    const auto monitor_source_name = default_output_monitor_source_name();
    if (!monitor_source_name.has_value())
    {
        set_status_message(QStringLiteral("Default output monitor could not be resolved"));
        return;
    }

    const auto monitor_source_format = detect_pulse_source_capture_format(*monitor_source_name);
    const int monitor_channel_count = monitor_source_format.has_value() ? monitor_source_format->channel_count : 2;
    const int monitor_sample_rate = monitor_source_format.has_value() ? monitor_source_format->sample_rate : 48000;

    using_external_capture_ = true;
    audio_format_.setChannelCount(monitor_channel_count);
    audio_format_.setSampleRate(monitor_sample_rate);
    audio_format_.setSampleFormat(QAudioFormat::Int16);

    const auto capture_command = build_external_monitor_capture_command(*monitor_source_name, monitor_channel_count,
                                                                        monitor_sample_rate);
    audio_process_.setProgram(capture_command.program);
    audio_process_.setArguments(capture_command.arguments);

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
    set_status_message(QStringLiteral("Opened monitor: %1 (%2 ch @ %3 Hz via %4)")
                           .arg(*monitor_source_name)
                           .arg(monitor_channel_count)
                           .arg(monitor_sample_rate)
                           .arg(capture_command.backend_name));
#endif
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

}

void AudioAnalysisWindow::process_audio_buffer(const QByteArray &data)
{
    if (data.isEmpty())
    {
        return;
    }

    update_levels(data);
    update();
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
    std::array<float, 2> channel_chunk_peaks{0.0F, 0.0F};
    float chunk_peak = 0.0F;
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
                value = audio_analysis::sample_to_float(bytes[offset]);
                break;
            case QAudioFormat::Int16:
            {
                qint16 sample = 0;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = audio_analysis::sample_to_float(sample);
                break;
            }
            case QAudioFormat::Int32:
            {
                qint32 sample = 0;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = audio_analysis::sample_to_float(sample);
                break;
            }
            case QAudioFormat::Float:
            {
                float sample = 0.0F;
                std::memcpy(&sample, bytes + offset, sizeof(sample));
                value = audio_analysis::sample_to_float(sample);
                break;
            }
            default:
                return;
            }

            sum_squares[channel] += value * value;
            const float absolute_value = static_cast<float>(std::abs(value));
            channel_chunk_peaks[channel] = std::max(channel_chunk_peaks[channel], absolute_value);
            chunk_peak = std::max(chunk_peak, absolute_value);
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
        channel_peak_levels_[channel] = std::max(channel_chunk_peaks[channel], channel_peak_levels_[channel] * 0.92F);
    }

    if (channel_count == 1)
    {
        channel_levels_db_[1] = channel_levels_db_[0];
        channel_peak_levels_[1] = channel_peak_levels_[0];
    }

    overall_level_db_ = std::max(channel_levels_db_[0], channel_levels_db_[1]);

    const double left_rms = samples_per_channel > 0 ? std::sqrt(sum_squares[0] / samples_per_channel) : 0.0;
    const double right_rms = samples_per_channel > 0 ? std::sqrt(sum_squares[1] / samples_per_channel) : left_rms;
    const float combined_rms = static_cast<float>(std::clamp(std::max(left_rms, right_rms), 0.0, 1.0));
    rms_level_ = rms_level_ * 0.85F + combined_rms * 0.15F;
    peak_level_ = std::max(chunk_peak, peak_level_ * 0.92F);
    refresh_waveform_samples();
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

void AudioAnalysisWindow::refresh_waveform_samples()
{
    waveform_samples_.fill(0.0F);

    const std::size_t available_samples = std::min(fft_sample_count_, fft_sample_buffer_.size());
    if (available_samples == 0)
    {
        return;
    }

    for (std::size_t index = 0; index < waveform_samples_.size(); ++index)
    {
        const float ratio = waveform_samples_.size() > 1
                                ? static_cast<float>(index) / static_cast<float>(waveform_samples_.size() - 1)
                                : 0.0F;
        const std::size_t source_index = std::min(static_cast<std::size_t>(ratio * static_cast<float>(available_samples - 1)),
                                                  available_samples - 1);
        waveform_samples_[index] = std::clamp(fft_sample_buffer_[source_index], -1.0F, 1.0F);
    }
}

} // namespace cockscreen::runtime