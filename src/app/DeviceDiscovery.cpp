#include "cockscreen/app/CliSupport.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace cockscreen::app
{

namespace
{

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

} // namespace

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

} // namespace cockscreen::app