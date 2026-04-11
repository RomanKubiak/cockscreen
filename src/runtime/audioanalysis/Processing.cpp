#include "cockscreen/runtime/AudioAnalysisWindow.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/audioanalysis/Support.hpp"

#include <QByteArray>
#include <QIODevice>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace cockscreen::runtime
{

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
            chunk_peak = std::max(chunk_peak, static_cast<float>(std::abs(value)));
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