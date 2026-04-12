#ifdef _WIN32

// <initguid.h> must come before any header that uses DEFINE_GUID so that the
// GUIDs (CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator, etc.) are emitted
// as definitions rather than extern declarations in this translation unit.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "cockscreen/runtime/audioanalysis/WasapiLoopback.hpp"

#include <QTimer>

#include <cstring>

namespace cockscreen::runtime::audio_analysis
{

// {00000003-0000-0010-8000-00AA00389B71}
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT — defined manually to avoid pulling in <ksmedia.h>.
static const GUID kSubFormat_Float{
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// ---- Pimpl ----------------------------------------------------------------

struct WasapiLoopbackCapture::Impl
{
    IMMDeviceEnumerator *enumerator{nullptr};
    IMMDevice *device{nullptr};
    IAudioClient *audio_client{nullptr};
    IAudioCaptureClient *capture_client{nullptr};
    UINT32 buffer_frames{0};
};

// ---- Construction / destruction -------------------------------------------

WasapiLoopbackCapture::WasapiLoopbackCapture(QObject *parent)
    : QObject{parent}, impl_{new Impl{}}
{
    // S_OK  — we initialised COM; we must call CoUninitialize on destruction.
    // S_FALSE — already initialised on this thread (Qt did it); must NOT uninit.
    // RPC_E_CHANGED_MODE — different model but still usable; must NOT uninit.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = (hr == S_OK);
}

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
    delete impl_;
    if (com_initialized_)
    {
        CoUninitialize();
    }
}

// ---- Public API -----------------------------------------------------------

bool WasapiLoopbackCapture::start()
{
    stop();

    // 1. Device enumerator
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator,
                                  reinterpret_cast<void **>(&impl_->enumerator));
    if (FAILED(hr) || impl_->enumerator == nullptr)
    {
        error_string_ = QStringLiteral("WASAPI: failed to create device enumerator (hr=0x%1)")
                            .arg(static_cast<unsigned long>(static_cast<ULONG>(hr)), 8, 16, QChar{'0'});
        return false;
    }

    // 2. Default *render* endpoint — loopback taps the render stream
    hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    if (FAILED(hr) || impl_->device == nullptr)
    {
        error_string_ = QStringLiteral("WASAPI: no default render endpoint for loopback");
        release();
        return false;
    }

    // 3. Audio client
    hr = impl_->device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(&impl_->audio_client));
    if (FAILED(hr) || impl_->audio_client == nullptr)
    {
        error_string_ = QStringLiteral("WASAPI: failed to activate IAudioClient");
        release();
        return false;
    }

    // 4. Mix format — describes the engine's shared-mode PCM layout
    WAVEFORMATEX *mix_fmt = nullptr;
    hr = impl_->audio_client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || mix_fmt == nullptr)
    {
        error_string_ = QStringLiteral("WASAPI: GetMixFormat failed");
        release();
        return false;
    }

    format_.setSampleRate(static_cast<int>(mix_fmt->nSamplesPerSec));
    format_.setChannelCount(static_cast<int>(mix_fmt->nChannels));

    const bool tag_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    const bool ext_float =
        (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) &&
        IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix_fmt)->SubFormat,
                    kSubFormat_Float);

    if (tag_float || ext_float)
    {
        format_.setSampleFormat(QAudioFormat::Float);
    }
    else if (mix_fmt->wBitsPerSample == 16)
    {
        format_.setSampleFormat(QAudioFormat::Int16);
    }
    else
    {
        format_.setSampleFormat(QAudioFormat::Int32);
    }

    // 5. Initialise for shared-mode loopback with a 200 ms engine buffer
    constexpr REFERENCE_TIME kBufferDuration = 200LL * 10000LL; // 100-ns units
    hr = impl_->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          kBufferDuration,
                                          0,
                                          mix_fmt,
                                          nullptr);
    CoTaskMemFree(mix_fmt);

    if (FAILED(hr))
    {
        error_string_ = QStringLiteral("WASAPI: IAudioClient::Initialize failed (hr=0x%1)")
                            .arg(static_cast<unsigned long>(static_cast<ULONG>(hr)), 8, 16, QChar{'0'});
        release();
        return false;
    }

    // 6. Capture client
    hr = impl_->audio_client->GetService(IID_IAudioCaptureClient,
                                          reinterpret_cast<void **>(&impl_->capture_client));
    if (FAILED(hr) || impl_->capture_client == nullptr)
    {
        error_string_ = QStringLiteral("WASAPI: failed to get IAudioCaptureClient");
        release();
        return false;
    }

    impl_->audio_client->GetBufferSize(&impl_->buffer_frames);

    // 7. Start the stream
    hr = impl_->audio_client->Start();
    if (FAILED(hr))
    {
        error_string_ = QStringLiteral("WASAPI: IAudioClient::Start failed");
        release();
        return false;
    }

    // 8. Poll the capture client every 10 ms on the Qt event loop
    timer_ = new QTimer{this};
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(10);
    QObject::connect(timer_, &QTimer::timeout, this, &WasapiLoopbackCapture::poll);
    timer_->start();
    return true;
}

void WasapiLoopbackCapture::stop()
{
    if (timer_ != nullptr)
    {
        timer_->stop();
        delete timer_;
        timer_ = nullptr;
    }

    if (impl_->audio_client != nullptr)
    {
        impl_->audio_client->Stop();
    }

    release();
}

// ---- Private helpers -------------------------------------------------------

void WasapiLoopbackCapture::poll()
{
    if (impl_->capture_client == nullptr)
    {
        return;
    }

    for (;;)
    {
        UINT32 packet_size = 0;
        if (FAILED(impl_->capture_client->GetNextPacketSize(&packet_size)) || packet_size == 0)
        {
            break;
        }

        BYTE *data_ptr = nullptr;
        UINT32 frames_available = 0;
        DWORD flags = 0;
        const HRESULT hr =
            impl_->capture_client->GetBuffer(&data_ptr, &frames_available, &flags, nullptr, nullptr);

        if (FAILED(hr))
        {
            break;
        }

        if (frames_available > 0)
        {
            const int bytes_per_frame = format_.channelCount() * format_.bytesPerSample();
            const int byte_count = static_cast<int>(frames_available) * bytes_per_frame;

            QByteArray chunk(byte_count, '\0');
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data_ptr != nullptr)
            {
                std::memcpy(chunk.data(), data_ptr, static_cast<std::size_t>(byte_count));
            }

            impl_->capture_client->ReleaseBuffer(frames_available);
            if (callback_)
            {
                callback_(std::move(chunk));
            }
        }
        else
        {
            impl_->capture_client->ReleaseBuffer(0);
        }
    }
}

void WasapiLoopbackCapture::release()
{
    if (impl_->capture_client != nullptr)
    {
        impl_->capture_client->Release();
        impl_->capture_client = nullptr;
    }
    if (impl_->audio_client != nullptr)
    {
        impl_->audio_client->Release();
        impl_->audio_client = nullptr;
    }
    if (impl_->device != nullptr)
    {
        impl_->device->Release();
        impl_->device = nullptr;
    }
    if (impl_->enumerator != nullptr)
    {
        impl_->enumerator->Release();
        impl_->enumerator = nullptr;
    }
}

} // namespace cockscreen::runtime::audio_analysis
#endif // _WIN32
