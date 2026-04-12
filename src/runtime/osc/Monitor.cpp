#include "cockscreen/runtime/OscInputMonitor.hpp"

#include <QString>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
namespace { inline bool socket_invalid(socket_t s) { return s == INVALID_SOCKET; } }
namespace { inline void socket_close(socket_t s) { ::closesocket(s); } }
namespace { inline std::string last_socket_error() { return "WSAError " + std::to_string(WSAGetLastError()); } }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
namespace { inline bool socket_invalid(socket_t s) { return s < 0; } }
namespace { inline void socket_close(socket_t s) { ::close(s); } }
namespace { inline std::string last_socket_error() { return std::strerror(errno); } }
#endif

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
#ifdef _WIN32
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    open_socket();
}

OscInputMonitor::~OscInputMonitor()
{
    close_socket();
#ifdef _WIN32
    WSACleanup();
#endif
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

    socket_fd_ = static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_invalid(static_cast<socket_t>(socket_fd_)))
    {
        status_message_ = std::string{"socket() failed: "} + last_socket_error();
        return false;
    }

#ifdef _WIN32
    u_long nb_mode = 1;
    ::ioctlsocket(static_cast<socket_t>(socket_fd_), FIONBIO, &nb_mode);
#else
    const int flags = ::fcntl(static_cast<socket_t>(socket_fd_), F_GETFL, 0);
    if (flags >= 0)
    {
        ::fcntl(static_cast<socket_t>(socket_fd_), F_SETFL, flags | O_NONBLOCK);
    }
#endif

    const int opt = 1;
    ::setsockopt(static_cast<socket_t>(socket_fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&opt), sizeof(opt));

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
            socket_close(static_cast<socket_t>(socket_fd_));
            socket_fd_ = static_cast<std::uintptr_t>(kInvalidSocket);
            status_message_ = "invalid address: " + host;
            return false;
        }
    }

    if (::bind(static_cast<socket_t>(socket_fd_),
               reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        socket_close(static_cast<socket_t>(socket_fd_));
        socket_fd_ = static_cast<std::uintptr_t>(kInvalidSocket);
        status_message_ = std::string{"bind() failed on "} + endpoint_ + ": " + last_socket_error();
        return false;
    }

    active_ = true;
    status_message_ = endpoint_;
    return true;
}

void OscInputMonitor::close_socket()
{
    if (!socket_invalid(static_cast<socket_t>(socket_fd_)))
    {
        socket_close(static_cast<socket_t>(socket_fd_));
        socket_fd_ = static_cast<std::uintptr_t>(kInvalidSocket);
    }
    active_ = false;
}

void OscInputMonitor::poll()
{
    if (!active_ || socket_invalid(static_cast<socket_t>(socket_fd_)))
    {
        return;
    }

    static constexpr int kMaxPacketSize = 1024;
    char buf[kMaxPacketSize];

    while (true)
    {
        const int n = static_cast<int>(::recv(static_cast<socket_t>(socket_fd_), buf, sizeof(buf) - 1, 0));
        if (n <= 0)
        {
            break;
        }
        process_packet(buf, n);
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
        ++message_count_;
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
        ++message_count_;
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

unsigned long OscInputMonitor::message_count() const
{
    return message_count_;
}

int OscInputMonitor::address_count() const
{
    return static_cast<int>(values_.size());
}

} // namespace cockscreen::runtime
