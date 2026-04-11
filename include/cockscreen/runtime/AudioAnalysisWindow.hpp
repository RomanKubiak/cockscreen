#pragma once

#include <array>
#include <chrono>
#include <complex>
#include <cstddef>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QScreen>
#include <QProcess>
#include <QWidget>

#include <pffft/pffft.hpp>

#include "Application.hpp"

namespace cockscreen::runtime
{

class AudioAnalysisWindow final : public QWidget
{
  public:
    explicit AudioAnalysisWindow(const ApplicationSettings &settings, QWidget *parent = nullptr);

    [[nodiscard]] float left_level_db() const;
    [[nodiscard]] float right_level_db() const;
    [[nodiscard]] float overall_level_db() const;
    [[nodiscard]] float rms_level() const;
    [[nodiscard]] float peak_level() const;
    [[nodiscard]] const std::array<float, core::kAudioFftBandCount> &fft_bands() const;
    [[nodiscard]] const std::array<float, core::kAudioWaveformSampleCount> &waveform_samples() const;
    [[nodiscard]] QString status_message() const;

    void place_on_screen(QScreen *screen);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    static constexpr float kSilenceDb{-120.0F};
    static constexpr int kFftSize{1024};
    static constexpr int kMeterWidth{180};
    static constexpr int kMeterHeight{420};

    void start_audio_capture();
    void process_audio_chunk();
    void update_levels(const QByteArray &data);
    void update_fft_analysis(float mono_sample);
    void refresh_waveform_samples();
    void set_status_message(QString message);
    void start_monitor_capture();
    static float rms_to_db(double rms);
    static int bytes_per_sample(const QAudioFormat &format);

    ApplicationSettings settings_;
    QString device_label_;
    QString status_message_;
    QAudioDevice audio_device_;
    QAudioFormat audio_format_;
    QAudioSource *audio_source_{nullptr};
    QIODevice *audio_io_{nullptr};
    QProcess audio_process_;
    bool using_external_capture_{false};
    std::array<float, 2> channel_levels_db_{kSilenceDb, kSilenceDb};
    float overall_level_db_{kSilenceDb};
    float rms_level_{0.0F};
    float peak_level_{0.0F};
    std::array<float, core::kAudioFftBandCount> fft_band_levels_{};
    std::array<float, core::kAudioWaveformSampleCount> waveform_samples_{};
    std::array<float, kFftSize> fft_sample_buffer_{};
    std::array<float, kFftSize> fft_window_{};
    std::size_t fft_sample_count_{0};
    pffft::Fft<float> fft_{kFftSize};
    pffft::AlignedVector<float> fft_input_{fft_.valueVector()};
    pffft::AlignedVector<std::complex<float>> fft_spectrum_{fft_.spectrumVector()};
    std::chrono::steady_clock::time_point last_profile_report_{};
};

} // namespace cockscreen::runtime