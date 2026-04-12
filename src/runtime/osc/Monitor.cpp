#include "cockscreen/runtime/OscInputMonitor.hpp"

#include <QString>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace cockscreen::runtime
{

namespace
{

bool parse_endpoint(const std::string &endpoint, std::string *host, uint16_t *port)
{
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }

    *host = endpoint.substr(0, colon);

    try
    {
        const int p = std::stoi(endpoint.substr(colon + 1));
        if (p <= 0 || p > 65535)
        {
            return false;
        }
        *port = static_cast<uint16_t>(p);
    }
    catch (...)
    {
        return false;
    }

    return true;
}

} // namespace

OscInputMonitor::OscInputMonitor(std::string endpoint, const std::vector<OscMapping> *scene_osc_mappings)
    : scene_osc_mappings_{scene_osc_mappings}, endpoint_{std::move(endpoint)}
{
    open_socket();
}

OscInputMonitor::~OscInputMonitor()
{
    close_socket();
}

bool OscInputMonitor::is_active() const
{
    return active_;
}

QString OscInputMonitor::status_message() const
{
    return QString::fromStdString(status_message_);
}

QString OscInputMonitor::activity_message() const
{
    return QString::fromStdString(activity_message_);
}

bool OscInputMonitor::open_socket()
{
    std::string host;
    uint16_t port = 0;
    if (!parse_endpoint(endpoint_, &host, &port))
    {
        status_message_ = "invalid endpoint: " + endpoint_;
        return false;
    }

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0)
    {
        status_message_ = std::string{"socket() failed: "} + std::strerror(errno);
        return false;
    }

    const int flags = ::fcntl(socket_fd_, F_GETFL, 0);
    if (flags >= 0)
    {
        ::fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    const int opt = 1;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host.empty() || host == "0.0.0.0" || host == "*")
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    else
    {
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            ::close(socket_fd_);
            socket_fd_ = -1;
            status_message_ = "invalid address: " + host;
            return false;
        }
    }

    if (::bind(socket_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(socket_fd_);
        socket_fd_ = -1;
        status_message_ = std::string{"bind() failed on "} + endpoint_ + ": " + std::strerror(errno);
        return false;
    }

    active_ = true;
    status_message_ = endpoint_;
    return true;
}

void OscInputMonitor::close_socket()
{
    if (socket_fd_ >= 0)
    {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    active_ = false;
}

void OscInputMonitor::poll()
{
    if (!active_ || socket_fd_ < 0)
    {
        return;
    }

    static constexpr int kMaxPacketSize = 1024;
    char buf[kMaxPacketSize];

    while (true)
    {
        const ssize_t n = ::recv(socket_fd_, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
        {
            break;
        }
        process_packet(buf, static_cast<int>(n));
    }
}

void OscInputMonitor::process_packet(const char *data, int size)
{
    // Minimum viable OSC packet is 8 bytes
    if (size < 8)
    {
        return;
    }

    // Address string: null-terminated, must start with '/'
    const auto addr_len = static_cast<int>(::strnlen(data, static_cast<std::size_t>(size)));
    if (addr_len == 0 || addr_len >= size || data[0] != '/')
    {
        return;
    }

    const std::string address(data, static_cast<std::string::size_type>(addr_len));

    // Pad to 4-byte boundary (addr_len + 1 for null terminator, then round up)
    const int addr_padded = (addr_len + 4) & ~3;
    if (addr_padded >= size)
    {
        return;
    }

    // Type tag string: starts with ','
    const char *type_tag = data + addr_padded;
    const auto tag_available = static_cast<std::size_t>(size - addr_padded);
    const auto tag_len = static_cast<int>(::strnlen(type_tag, tag_available));
    if (tag_len < 2 || type_tag[0] != ',')
    {
        return;
    }

    const int tag_padded = (tag_len + 4) & ~3;
    const int data_offset = addr_padded + tag_padded;

    const char arg_type = type_tag[1];

    if (arg_type == 'f')
    {
        if (data_offset + 4 > size)
        {
            return;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, data + data_offset, 4);
        raw = ntohl(raw);
        float value = 0.0F;
        std::memcpy(&value, &raw, 4);
        values_[address] = value;
        activity_message_ = address + " f=" + std::to_string(value).substr(0, 6);
    }
    else if (arg_type == 'i')
    {
        if (data_offset + 4 > size)
        {
            return;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, data + data_offset, 4);
        const auto ival = static_cast<int32_t>(ntohl(raw));
        values_[address] = static_cast<float>(ival);
        activity_message_ = address + " i=" + std::to_string(ival);
    }
}

void OscInputMonitor::populate_frame(core::ControlFrame *frame) const
{
    if (frame == nullptr)
    {
        return;
    }
    frame->osc_values = values_;
}

} // namespace cockscreen::runtime
