#include "cockscreen/runtime/midi/PortDiscovery.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cockscreen::runtime::midi
{

std::vector<PortRecord> read_ports()
{
    std::vector<PortRecord> result;
    const std::filesystem::path seq_clients{"/proc/asound/seq/clients"};
    std::ifstream file{seq_clients};
    if (!file.is_open())
    {
        return result;
    }

    std::string line;
    PortRecord current{};
    bool have_client = false;
    while (std::getline(file, line))
    {
        if (line.rfind("Client ", 0) == 0)
        {
            current = {};
            have_client = false;

            std::istringstream parser{line};
            std::string client_word;
            std::string client_number_text;
            if (!(parser >> client_word >> client_number_text) || client_word != "Client")
            {
                continue;
            }

            try
            {
                current.client = std::stoi(client_number_text);
            }
            catch (...)
            {
                continue;
            }

            const auto first_quote = line.find('"');
            const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
            if (first_quote != std::string::npos && second_quote != std::string::npos)
            {
                current.client_name = line.substr(first_quote + 1, second_quote - first_quote - 1);
                have_client = true;
            }
            continue;
        }

        if (!have_client || line.rfind("  Port ", 0) != 0)
        {
            continue;
        }

        std::istringstream parser{line};
        std::string port_word;
        std::string port_number_text;
        if (!(parser >> port_word >> port_number_text) || port_word != "Port")
        {
            continue;
        }

        try
        {
            current.port = std::stoi(port_number_text);
        }
        catch (...)
        {
            continue;
        }

        const auto first_quote = line.find('"');
        const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
        if (first_quote != std::string::npos && second_quote != std::string::npos)
        {
            current.port_name = line.substr(first_quote + 1, second_quote - first_quote - 1);
        }

        if (current.client >= 0 && current.port >= 0 && !current.client_name.empty() && !current.port_name.empty())
        {
            result.push_back(current);
        }
    }

    return result;
}

std::string make_label(const PortRecord &record)
{
    return record.client_name + " / " + record.port_name;
}

std::string channel_label(int channel)
{
    return std::to_string(channel + 1) + " (raw " + std::to_string(channel) + ")";
}

} // namespace cockscreen::runtime::midi