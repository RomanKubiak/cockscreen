#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#endif

namespace cockscreen::app
{

namespace
{
#ifndef _WIN32

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
    std::ifstream file{path};
    if (!file.is_open())
    {
        return std::nullopt;
    }

    std::string value;
    std::getline(file, value);
    return value;
}

bool contains_case_insensitive(std::string_view text, std::string_view needle)
{
    if (needle.empty() || text.size() < needle.size())
    {
        return false;
    }

    for (std::size_t offset = 0; offset + needle.size() <= text.size(); ++offset)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            const auto left = static_cast<unsigned char>(text[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right))
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return true;
        }
    }

    return false;
}

bool card_has_capture_pcm(const std::filesystem::path &card_dir)
{
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator{card_dir, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_directory())
        {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name.rfind("pcm", 0) == 0 && !name.empty() && name.back() == 'c')
        {
            return true;
        }
    }

    return false;
}

#endif // !_WIN32
} // namespace

#ifdef _WIN32

std::optional<std::string> detect_default_audio_device()
{
    IMMDeviceEnumerator *enumerator = nullptr;
    const HRESULT hr_enum = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                              IID_IMMDeviceEnumerator,
                                              reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr_enum) || enumerator == nullptr)
    {
        return std::nullopt;
    }

    IMMDevice *device = nullptr;
    const HRESULT hr_dev = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr_dev) || device == nullptr)
    {
        return std::nullopt;
    }

    IPropertyStore *props = nullptr;
    device->OpenPropertyStore(STGM_READ, &props);
    device->Release();
    if (props == nullptr)
    {
        return std::nullopt;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    const HRESULT hr_prop = props->GetValue(PKEY_Device_FriendlyName, &var);
    props->Release();

    std::optional<std::string> result;
    if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR && var.pwszVal != nullptr)
    {
        const int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)
        {
            std::string name(static_cast<std::size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, name.data(), len, nullptr, nullptr);
            result = std::move(name);
        }
    }
    PropVariantClear(&var);
    return result;
}

std::vector<std::string> detect_midi_devices()
{
    std::vector<std::string> result;
    const UINT num_devs = midiInGetNumDevs();
    for (UINT i = 0; i < num_devs; ++i)
    {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) { continue; }
        const int wlen   = static_cast<int>(wcslen(caps.szPname));
        const int needed = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, wlen, nullptr, 0, nullptr, nullptr);
        if (needed <= 0) { continue; }
        std::string name(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, caps.szPname, wlen, name.data(), needed, nullptr, nullptr);
        result.push_back(std::move(name));
    }
    return result;
}

std::optional<std::string> detect_default_midi_device()
{
    const auto devices = detect_midi_devices();
    if (!devices.empty())
    {
        return devices.front();
    }
    return std::nullopt;
}

#else // !_WIN32

std::optional<std::string> detect_default_audio_device()
{
    const std::filesystem::path asound_root{"/proc/asound"};
    if (!std::filesystem::exists(asound_root))
    {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> card_dirs;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator{asound_root, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_directory())
        {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) == 0)
        {
            card_dirs.push_back(entry.path());
        }
    }

    std::sort(card_dirs.begin(), card_dirs.end());

    for (const auto &card_dir : card_dirs)
    {
        const auto id = read_text_file(card_dir / "id");
        const auto name = read_text_file(card_dir / "name");
        const auto id_text = id.value_or("");
        const auto name_text = name.value_or("");

        if (contains_case_insensitive(id_text, "hdmi") || contains_case_insensitive(name_text, "hdmi") ||
            contains_case_insensitive(id_text, "vc4hdmi") || contains_case_insensitive(name_text, "vc4-hdmi"))
        {
            continue;
        }

        if (!card_has_capture_pcm(card_dir))
        {
            continue;
        }

        if (!id_text.empty())
        {
            return id_text;
        }

        if (!name_text.empty())
        {
            return name_text;
        }
    }

    return std::nullopt;
}

std::vector<std::string> detect_midi_devices()
{
    std::vector<std::string> result;
    const std::filesystem::path seq_clients{"/proc/asound/seq/clients"};
    std::ifstream file{seq_clients};
    if (!file.is_open())
    {
        return result;
    }

    std::string line;
    std::string current_client;
    while (std::getline(file, line))
    {
        if (line.rfind("Client ", 0) == 0)
        {
            const auto first_quote = line.find('"');
            const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
            if (first_quote != std::string::npos && second_quote != std::string::npos)
            {
                current_client = line.substr(first_quote + 1, second_quote - first_quote - 1);
                if (current_client == "System" || current_client == "Midi Through" ||
                    contains_case_insensitive(current_client, "pipewire"))
                {
                    current_client.clear();
                }
            }
            else
            {
                current_client.clear();
            }
            continue;
        }

        if (current_client.empty() || line.rfind("  Port ", 0) != 0)
        {
            continue;
        }

        const auto first_quote = line.find('"');
        const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
        if (first_quote == std::string::npos || second_quote == std::string::npos)
        {
            continue;
        }

        const auto port_name = line.substr(first_quote + 1, second_quote - first_quote - 1);
        if (port_name.empty())
        {
            continue;
        }

        result.push_back(current_client + " / " + port_name);
        current_client.clear();
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<std::string> detect_default_midi_device()
{
    const auto midi_devices = detect_midi_devices();
    if (midi_devices.empty())
    {
        return std::nullopt;
    }

    return midi_devices.front();
}

std::vector<RpiCameraDevice> detect_rpi_cameras()
{
    static constexpr std::string_view rpi_driver_keywords[] = {
        "unicam", "bm2835", "mmal", "rpivid", "imx219", "imx477", "imx708",
        "ov5647", "ov9281", "ov64a40", "se327m12",
    };

    std::vector<RpiCameraDevice> result;

    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator{"/dev", ec})
    {
        if (ec)
        {
            break;
        }

        const auto filename = entry.path().filename().string();
        if (filename.rfind("video", 0) != 0)
        {
            continue;
        }

        const std::string path = entry.path().string();
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
        {
            continue;
        }

        v4l2_capability cap{};
        const bool ok = (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
        ::close(fd);

        if (!ok || (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
        {
            continue;
        }

        const std::string driver{reinterpret_cast<const char *>(cap.driver)};
        const std::string card{reinterpret_cast<const char *>(cap.card)};

        bool is_rpi = false;
        for (const auto &kw : rpi_driver_keywords)
        {
            if (contains_case_insensitive(driver, kw) || contains_case_insensitive(card, kw))
            {
                is_rpi = true;
                break;
            }
        }

        if (!is_rpi)
        {
            continue;
        }

        RpiCameraDevice dev;
        dev.path = path;
        dev.driver = driver;
        dev.card = card;
        dev.requires_media_controller = (cap.capabilities & V4L2_CAP_IO_MC) != 0U;
        dev.modes = runtime::V4l2Capture::enumerate_supported_modes(path);
        result.push_back(std::move(dev));
    }

    std::sort(result.begin(), result.end(),
              [](const RpiCameraDevice &a, const RpiCameraDevice &b) { return a.path < b.path; });
    return result;
}

#endif // !_WIN32
} // namespace cockscreen::app