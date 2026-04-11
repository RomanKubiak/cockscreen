#pragma once

#include <string>
#include <vector>

namespace cockscreen::runtime::midi
{

struct PortRecord
{
    int client{-1};
    int port{-1};
    std::string client_name;
    std::string port_name;
};

std::vector<PortRecord> read_ports();
std::string make_label(const PortRecord &record);
std::string channel_label(int channel);

} // namespace cockscreen::runtime::midi