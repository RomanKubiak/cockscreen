#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

// Enumerate the frame sizes that the CSI sensor subdev actually supports by
// walking the media controller graph from the given video node to its sensor
// entity and calling VIDIOC_SUBDEV_ENUM_MBUS_CODE + VIDIOC_SUBDEV_ENUM_FRAME_SIZE.
// Returns fixed-size modes as "WxH" strings, sorted smallest-first.
// Returns an empty vector on any failure so the caller can fall back gracefully.
static std::vector<std::string> enumerate_mc_sensor_modes(const std::string &video_path)
{
    struct stat video_stat{};
    if (::stat(video_path.c_str(), &video_stat) != 0)
        return {};
    const unsigned int target_major = major(video_stat.st_rdev);
    const unsigned int target_minor = minor(video_stat.st_rdev);

    int media_fd = -1;
    __u32 capture_entity_id = 0;
    for (int n = 0; n < 16 && media_fd < 0; ++n)
    {
        const std::string mpath = "/dev/media" + std::to_string(n);
        const int fd = ::open(mpath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            break;
        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0)
        {
            if (entity.v4l.major == target_major && entity.v4l.minor == target_minor)
            {
                capture_entity_id = entity.id;
                media_fd = fd;
                break;
            }
            entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
        }
        if (media_fd < 0)
            ::close(fd);
    }
    if (media_fd < 0)
        return {};

    __u32 sensor_entity_id = 0;
    __u16 sensor_pad_index = 0;
    {
        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (::ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0 && sensor_entity_id == 0)
        {
            const __u32 eid = entity.id;
            if (eid != capture_entity_id && entity.links > 0)
            {
                std::vector<media_pad_desc> pads(entity.pads);
                std::vector<media_link_desc> link_descs(entity.links);
                media_links_enum links_req{};
                links_req.entity   = eid;
                links_req.pads     = pads.empty() ? nullptr : pads.data();
                links_req.links    = link_descs.data();
                if (::ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links_req) == 0)
                {
                    for (const auto &link : link_descs)
                    {
                        if (link.sink.entity == capture_entity_id)
                        {
                            sensor_entity_id = link.source.entity;
                            sensor_pad_index = link.source.index;
                            break;
                        }
                    }
                }
            }
            entity.id = eid | MEDIA_ENT_ID_FLAG_NEXT;
        }
    }
    if (sensor_entity_id == 0)
    {
        ::close(media_fd);
        return {};
    }

    media_entity_desc sensor_ent{};
    sensor_ent.id = sensor_entity_id;
    if (::ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &sensor_ent) != 0)
    {
        ::close(media_fd);
        return {};
    }
    ::close(media_fd);

    std::string subdev_path;
    for (int n = 0; n < 16; ++n)
    {
        const std::string spath = "/dev/v4l-subdev" + std::to_string(n);
        struct stat sst{};
        if (::stat(spath.c_str(), &sst) != 0)
            break;
        if (major(sst.st_rdev) == sensor_ent.v4l.major &&
            minor(sst.st_rdev) == sensor_ent.v4l.minor)
        {
            subdev_path = spath;
            break;
        }
    }
    if (subdev_path.empty())
        return {};

    const int subdev_fd = ::open(subdev_path.c_str(), O_RDWR | O_NONBLOCK);
    if (subdev_fd < 0)
        return {};

    // Collect all mbus codes supported by the sensor pad.
    std::vector<__u32> mbus_codes;
    for (__u32 idx = 0; ; ++idx)
    {
        v4l2_subdev_mbus_code_enum mce{};
        mce.pad   = sensor_pad_index;
        mce.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        mce.index = idx;
        if (::ioctl(subdev_fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mce) != 0)
            break;
        mbus_codes.push_back(mce.code);
    }

    // Enumerate discrete frame sizes across all mbus codes; deduplicate by WxH.
    std::set<std::pair<int, int>> seen;
    for (const auto code : mbus_codes)
    {
        for (__u32 idx = 0; ; ++idx)
        {
            v4l2_subdev_frame_size_enum fse{};
            fse.pad   = sensor_pad_index;
            fse.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            fse.index = idx;
            fse.code  = code;
            if (::ioctl(subdev_fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fse) != 0)
                break;
            // Only take fixed (non-stepwise) sizes — stepwise would be the
            // same misleading "any resolution" range we are trying to replace.
            if (fse.min_width == fse.max_width && fse.min_height == fse.max_height)
                seen.insert({static_cast<int>(fse.min_width), static_cast<int>(fse.min_height)});
        }
    }
    ::close(subdev_fd);

    std::vector<std::string> modes;
    modes.reserve(seen.size());
    for (const auto &[w, h] : seen)
        modes.push_back(std::to_string(w) + "x" + std::to_string(h));
    return modes;
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
        dev.modes = dev.requires_media_controller
                        ? enumerate_mc_sensor_modes(path)
                        : runtime::V4l2Capture::enumerate_supported_modes(path);
        result.push_back(std::move(dev));
    }

    std::sort(result.begin(), result.end(),
              [](const RpiCameraDevice &a, const RpiCameraDevice &b) { return a.path < b.path; });
    return result;
}

#endif // !_WIN32
} // namespace cockscreen::app