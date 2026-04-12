#pragma once
#ifdef _WIN32

#include <functional>
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

class QTimer;

namespace cockscreen::runtime::audio_analysis
{

// Captures audio from the system's default render endpoint via WASAPI loopback.
//
// Usage:
//   auto *cap = new WasapiLoopbackCapture{parentQObject};
//   cap->setAudioDataCallback([](QByteArray data) { ... });
//   if (!cap->start()) { /* cap->errorString() */ }
//
// format() returns the QAudioFormat describing every buffer passed to the callback.
// The capture runs on the Qt event loop via a QTimer; no extra threads are needed.
class WasapiLoopbackCapture final : public QObject
{
public:
    explicit WasapiLoopbackCapture(QObject *parent = nullptr);
    ~WasapiLoopbackCapture() override;

    [[nodiscard]] bool start();
    void stop();

    void setAudioDataCallback(std::function<void(QByteArray)> cb) { callback_ = std::move(cb); }

    [[nodiscard]] QAudioFormat format() const { return format_; }
    [[nodiscard]] QString errorString() const { return error_string_; }

private:
    void poll();
    void release();

    struct Impl;
    Impl *impl_{nullptr};

    QTimer *timer_{nullptr};
    QAudioFormat format_;
    QString error_string_;
    bool com_initialized_{false};
    std::function<void(QByteArray)> callback_;
};

} // namespace cockscreen::runtime::audio_analysis
#endif // _WIN32
