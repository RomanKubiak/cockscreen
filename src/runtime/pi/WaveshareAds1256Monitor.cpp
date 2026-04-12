#include "../../../include/cockscreen/runtime/pi/WaveshareAds1256Monitor.hpp"

#if defined(__linux__) && defined(__aarch64__)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <sys/ioctl.h>

namespace cockscreen::runtime
{

namespace
{

constexpr unsigned int kChipSelectLine{22};
constexpr unsigned int kResetLine{18};
constexpr unsigned int kPowerDownLine{27};
constexpr unsigned int kDrdyLine{17};

constexpr unsigned int kMuxSelectLine0{5};
constexpr unsigned int kMuxSelectLine1{6};
constexpr unsigned int kMuxSelectLine2{13};
constexpr unsigned int kMuxSelectLine3{26};
constexpr std::size_t kMuxChannelCount{16};

constexpr unsigned int kGateLine0{16};
constexpr unsigned int kGateLine1{19};
constexpr unsigned int kGateLine2{20};
constexpr std::chrono::milliseconds kGatePollPeriod{20};

constexpr std::uint8_t kCommandWakeup{0x00};
constexpr std::uint8_t kCommandRdata{0x01};
constexpr std::uint8_t kCommandSdatac{0x0F};
constexpr std::uint8_t kCommandSync{0xFC};
constexpr std::uint8_t kCommandSelfcal{0xF0};
constexpr std::uint8_t kCommandWreg{0x50};

constexpr std::uint8_t kRegisterStatus{0x00};
constexpr std::uint8_t kRegisterMux{0x01};
constexpr std::uint8_t kRegisterAdcon{0x02};
constexpr std::uint8_t kRegisterDrate{0x03};

constexpr std::uint8_t kDrate100Sps{0x82};
constexpr std::uint32_t kDefaultSpiSpeedHz{1'000'000};

std::string format_voltage_line(std::string_view source, unsigned int channel, double voltage, std::int32_t raw)
{
    std::ostringstream stream;
    stream << "[ads1256] ";
    if (!source.empty())
    {
        stream << source << ' ';
    }

    stream << "AD" << channel << " = " << std::fixed << std::setprecision(6) << voltage << " V (raw " << raw
           << ")";
    return stream.str();
}

std::string format_gate_line(const std::array<unsigned char, 3> &values)
{
    std::ostringstream stream;
    stream << "[ads1256] gates G0=" << (values[0] ? "HIGH" : "LOW") << " G1=" << (values[1] ? "HIGH" : "LOW")
           << " G2=" << (values[2] ? "HIGH" : "LOW");
    return stream.str();
}

std::uint8_t mux_for_channel(unsigned int channel)
{
    return static_cast<std::uint8_t>(((channel & 0x7U) << 4U) | 0x08U);
}

std::optional<int> parse_int_env(const char *name, int fallback, int min_value, int max_value)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return fallback;
    }

    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0')
    {
        return std::nullopt;
    }

    return static_cast<int>(std::clamp<long>(parsed, min_value, max_value));
}

double parse_double_env(const char *name, double fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return fallback;
    }

    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0')
    {
        return fallback;
    }

    return parsed;
}

std::chrono::milliseconds parse_period_env(const char *name, int fallback_ms)
{
    const auto parsed = parse_int_env(name, fallback_ms, 1, 10'000);
    return std::chrono::milliseconds{parsed.value_or(fallback_ms)};
}

} // namespace

struct WaveshareAds1256Monitor::Impl
{
    enum class OutputIndex : std::size_t
    {
        chip_select = 0,
        reset = 1,
        power_down = 2,
    };

    enum class MuxBit : std::size_t
    {
        s0 = 0,
        s1 = 1,
        s2 = 2,
        s3 = 3,
    };

    explicit Impl()
        : channel_{static_cast<unsigned int>(parse_int_env("COCKSCREEN_ADS1256_CHANNEL", 0, 0, 7).value_or(0))},
          mux_channel_count_{static_cast<unsigned int>(
              parse_int_env("COCKSCREEN_ADS1256_MUX_CHANNELS", static_cast<int>(kMuxChannelCount), 1,
                            static_cast<int>(kMuxChannelCount))
                  .value_or(static_cast<int>(kMuxChannelCount)))},
                    gate_poll_period_{parse_period_env("COCKSCREEN_ADS1256_GATE_POLL_MS",
                                                                                         static_cast<int>(kGatePollPeriod.count()))},
          vref_volts_{parse_double_env("COCKSCREEN_ADS1256_VREF_VOLTS", 5.0)},
          sample_period_{parse_period_env("COCKSCREEN_ADS1256_PERIOD_MS", 1000)}
    {
        if (mux_channel_count_ > 1)
        {
            channel_ = 0;
            mux_enabled_ = true;
        }
    }

    ~Impl()
    {
        stop();
        close_all();
    }

    bool start()
    {
        stop_requested_.store(false, std::memory_order_release);

        if (thread_.joinable())
        {
            return false;
        }

        if (!open_gpio_chip() || !open_spi_device())
        {
            return false;
        }

        if (!open_mux_gpio())
        {
            std::cerr << "[ads1256] CD74HC4067 support not active; falling back to AD0 only" << '\n';
        }

        if (!open_gate_gpio())
        {
            std::cerr << "[ads1256] gate input support not active" << '\n';
        }

        if (!initialize_adc())
        {
            return false;
        }

        std::cout << "[ads1256] monitoring AD" << channel_ << " via " << spi_device_path_ << " (VREF="
                  << std::fixed << std::setprecision(2) << vref_volts_ << " V, period="
                  << sample_period_.count() << " ms)" << '\n';
        if (mux_enabled_)
        {
            std::cout << "[ads1256] CD74HC4067 active on AD0 using BCM5/BCM6/BCM13/BCM26, scanning "
                      << mux_channel_count_ << " channels" << '\n';
        }
        if (gate_input_fd_ >= 0)
        {
            std::cout << "[ads1256] gate inputs active on BCM16/BCM19/BCM20" << '\n';
        }

        thread_ = std::thread([this]() { run(); });
        return true;
    }

    void stop()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    bool open_gpio_chip()
    {
        gpio_chip_fd_ = ::open(gpio_chip_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (gpio_chip_fd_ < 0)
        {
            std::cerr << "[ads1256] failed to open " << gpio_chip_path_ << " - " << std::strerror(errno) << '\n';
            return false;
        }

        gpiohandle_request output_request{};
        output_request.lines = 3;
        output_request.lineoffsets[0] = kChipSelectLine;
        output_request.lineoffsets[1] = kResetLine;
        output_request.lineoffsets[2] = kPowerDownLine;
        output_request.flags = GPIOHANDLE_REQUEST_OUTPUT;
        output_request.default_values[0] = 1;
        output_request.default_values[1] = 1;
        output_request.default_values[2] = 1;
        std::strncpy(output_request.consumer_label, "cockscreen-ads1256", sizeof(output_request.consumer_label) - 1U);

        if (::ioctl(gpio_chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &output_request) != 0)
        {
            std::cerr << "[ads1256] failed to request GPIO output lines - " << std::strerror(errno) << '\n';
            return false;
        }

        gpio_output_fd_ = output_request.fd;
        output_values_.values[0] = 1;
        output_values_.values[1] = 1;
        output_values_.values[2] = 1;

        gpiohandle_request input_request{};
        input_request.lines = 1;
        input_request.lineoffsets[0] = kDrdyLine;
        input_request.flags = GPIOHANDLE_REQUEST_INPUT;
        std::strncpy(input_request.consumer_label, "cockscreen-ads1256", sizeof(input_request.consumer_label) - 1U);

        if (::ioctl(gpio_chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &input_request) != 0)
        {
            std::cerr << "[ads1256] failed to request GPIO DRDY line - " << std::strerror(errno) << '\n';
            return false;
        }

        gpio_input_fd_ = input_request.fd;
        return true;
    }

    bool open_mux_gpio()
    {
        if (!mux_enabled_)
        {
            return true;
        }

        gpiohandle_request mux_request{};
        mux_request.lines = 4;
        mux_request.lineoffsets[0] = kMuxSelectLine0;
        mux_request.lineoffsets[1] = kMuxSelectLine1;
        mux_request.lineoffsets[2] = kMuxSelectLine2;
        mux_request.lineoffsets[3] = kMuxSelectLine3;
        mux_request.flags = GPIOHANDLE_REQUEST_OUTPUT;
        mux_request.default_values[0] = 0;
        mux_request.default_values[1] = 0;
        mux_request.default_values[2] = 0;
        mux_request.default_values[3] = 0;
        std::strncpy(mux_request.consumer_label, "cockscreen-4067", sizeof(mux_request.consumer_label) - 1U);

        if (::ioctl(gpio_chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &mux_request) != 0)
        {
            std::cerr << "[ads1256] failed to request CD74HC4067 select lines - " << std::strerror(errno) << '\n';
            mux_enabled_ = false;
            mux_channel_count_ = 1;
            return false;
        }

        mux_output_fd_ = mux_request.fd;
        mux_output_values_.values[0] = 0;
        mux_output_values_.values[1] = 0;
        mux_output_values_.values[2] = 0;
        mux_output_values_.values[3] = 0;
        return true;
    }

    bool open_gate_gpio()
    {
        gpiohandle_request gate_request{};
        gate_request.lines = 3;
        gate_request.lineoffsets[0] = kGateLine0;
        gate_request.lineoffsets[1] = kGateLine1;
        gate_request.lineoffsets[2] = kGateLine2;
        gate_request.flags = GPIOHANDLE_REQUEST_INPUT;
        std::strncpy(gate_request.consumer_label, "cockscreen-gates", sizeof(gate_request.consumer_label) - 1U);

        if (::ioctl(gpio_chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &gate_request) != 0)
        {
            std::cerr << "[ads1256] failed to request gate input lines - " << std::strerror(errno) << '\n';
            return false;
        }

        gate_input_fd_ = gate_request.fd;
        return true;
    }

    bool open_spi_device()
    {
        const std::array<std::string, 2> candidates{spi_device_path_, "/dev/spidev0.1"};
        for (const auto &candidate : candidates)
        {
            spi_fd_ = ::open(candidate.c_str(), O_RDWR | O_CLOEXEC);
            if (spi_fd_ >= 0)
            {
                spi_device_path_ = candidate;
                break;
            }
        }

        if (spi_fd_ < 0)
        {
            std::cerr << "[ads1256] failed to open SPI device - " << std::strerror(errno) << '\n';
            return false;
        }

        std::uint8_t mode = static_cast<std::uint8_t>(SPI_MODE_1 | SPI_NO_CS);
        std::uint8_t bits_per_word = 8;
        std::uint32_t max_speed_hz = kDefaultSpiSpeedHz;

        if (::ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) != 0 ||
            ::ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) != 0 ||
            ::ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &max_speed_hz) != 0)
        {
            std::cerr << "[ads1256] failed to configure SPI device - " << std::strerror(errno) << '\n';
            return false;
        }

        return true;
    }

    bool initialize_adc()
    {
        if (!set_output(OutputIndex::chip_select, true) || !set_output(OutputIndex::power_down, true))
        {
            return false;
        }

        if (!pulse_reset())
        {
            return false;
        }

        if (!send_command(kCommandSdatac))
        {
            return false;
        }

        if (!write_register(kRegisterStatus, 0x00) || !write_register(kRegisterMux, mux_for_channel(channel_)) ||
            !write_register(kRegisterAdcon, 0x00) || !write_register(kRegisterDrate, kDrate100Sps))
        {
            return false;
        }

        if (!send_command(kCommandSync) || !send_command(kCommandWakeup))
        {
            return false;
        }

        if (!send_command(kCommandSelfcal))
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return true;
    }

    bool pulse_reset()
    {
        if (!set_output(OutputIndex::reset, false))
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::microseconds{10});

        if (!set_output(OutputIndex::reset, true))
        {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        return true;
    }

    bool set_output(OutputIndex index, bool value)
    {
        output_values_.values[static_cast<std::size_t>(index)] = static_cast<unsigned char>(value ? 1 : 0);
        if (::ioctl(gpio_output_fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &output_values_) != 0)
        {
            std::cerr << "[ads1256] failed to update GPIO outputs - " << std::strerror(errno) << '\n';
            return false;
        }

        return true;
    }

    bool set_mux_channel(unsigned int channel)
    {
        if (!mux_enabled_)
        {
            return true;
        }

        const unsigned int clamped = std::min(channel, static_cast<unsigned int>(kMuxChannelCount - 1U));
        mux_output_values_.values[static_cast<std::size_t>(MuxBit::s0)] = static_cast<unsigned char>(clamped & 0x1U);
        mux_output_values_.values[static_cast<std::size_t>(MuxBit::s1)] =
            static_cast<unsigned char>((clamped >> 1U) & 0x1U);
        mux_output_values_.values[static_cast<std::size_t>(MuxBit::s2)] =
            static_cast<unsigned char>((clamped >> 2U) & 0x1U);
        mux_output_values_.values[static_cast<std::size_t>(MuxBit::s3)] =
            static_cast<unsigned char>((clamped >> 3U) & 0x1U);

        if (::ioctl(mux_output_fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &mux_output_values_) != 0)
        {
            std::cerr << "[ads1256] failed to set CD74HC4067 channel " << clamped << " - " << std::strerror(errno)
                      << '\n';
            return false;
        }

        std::this_thread::sleep_for(std::chrono::microseconds{50});
        return true;
    }

    bool read_drdy(bool *low)
    {
        if (low == nullptr)
        {
            return false;
        }

        gpiohandle_data data{};
        if (::ioctl(gpio_input_fd_, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) != 0)
        {
            std::cerr << "[ads1256] failed to read DRDY - " << std::strerror(errno) << '\n';
            return false;
        }

        *low = data.values[0] == 0;
        return true;
    }

    bool read_gates(std::array<unsigned char, 3> *values)
    {
        if (values == nullptr || gate_input_fd_ < 0)
        {
            return false;
        }

        gpiohandle_data data{};
        if (::ioctl(gate_input_fd_, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) != 0)
        {
            std::cerr << "[ads1256] failed to read gate inputs - " << std::strerror(errno) << '\n';
            return false;
        }

        (*values)[0] = data.values[0];
        (*values)[1] = data.values[1];
        (*values)[2] = data.values[2];
        return true;
    }

    bool wait_for_drdy_low(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!stop_requested_.load(std::memory_order_acquire))
        {
            bool low = false;
            if (!read_drdy(&low))
            {
                return false;
            }

            if (low)
            {
                return true;
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::cerr << "[ads1256] DRDY timeout waiting for channel " << channel_ << '\n';
                return false;
            }

            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }

        return false;
    }

    bool send_command(std::uint8_t command)
    {
        const std::array<std::uint8_t, 1> bytes{command};
        return spi_write(bytes.data(), bytes.size());
    }

    bool write_register(std::uint8_t register_index, std::uint8_t value)
    {
        const std::array<std::uint8_t, 3> bytes{static_cast<std::uint8_t>(kCommandWreg | (register_index & 0x0FU)),
                                                0x00U,
                                                value};
        return spi_write(bytes.data(), bytes.size());
    }

    bool spi_write(const std::uint8_t *bytes, std::size_t length)
    {
        if (spi_fd_ < 0 || bytes == nullptr || length == 0)
        {
            return false;
        }

        spi_ioc_transfer transfer{};
        transfer.tx_buf = reinterpret_cast<unsigned long>(bytes);
        transfer.len = static_cast<__u32>(length);
        transfer.bits_per_word = 8;
        transfer.speed_hz = kDefaultSpiSpeedHz;

        if (::ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &transfer) < 0)
        {
            std::cerr << "[ads1256] SPI write failed - " << std::strerror(errno) << '\n';
            return false;
        }

        return true;
    }

    bool read_raw(std::int32_t *raw)
    {
        if (raw == nullptr)
        {
            return false;
        }

        if (!wait_for_drdy_low(std::chrono::milliseconds{500}))
        {
            return false;
        }

        const std::array<std::uint8_t, 1> command{kCommandRdata};
        std::array<std::uint8_t, 3> padding{};
        std::array<std::uint8_t, 3> data{};

        spi_ioc_transfer transfers[2]{};
        transfers[0].tx_buf = reinterpret_cast<unsigned long>(command.data());
        transfers[0].len = static_cast<__u32>(command.size());
        transfers[0].bits_per_word = 8;
        transfers[0].speed_hz = kDefaultSpiSpeedHz;
        transfers[0].delay_usecs = 10;
        transfers[1].tx_buf = reinterpret_cast<unsigned long>(padding.data());
        transfers[1].rx_buf = reinterpret_cast<unsigned long>(data.data());
        transfers[1].len = static_cast<__u32>(data.size());
        transfers[1].bits_per_word = 8;
        transfers[1].speed_hz = kDefaultSpiSpeedHz;

        if (::ioctl(spi_fd_, SPI_IOC_MESSAGE(2), transfers) < 0)
        {
            std::cerr << "[ads1256] failed to read conversion data - " << std::strerror(errno) << '\n';
            return false;
        }

        const std::uint32_t combined = (static_cast<std::uint32_t>(data[0]) << 16U) |
                                        (static_cast<std::uint32_t>(data[1]) << 8U) |
                                        static_cast<std::uint32_t>(data[2]);
        std::int32_t signed_value = static_cast<std::int32_t>(combined);
        if ((signed_value & 0x0080'0000) != 0)
        {
            signed_value |= static_cast<std::int32_t>(0xFF00'0000);
        }

        *raw = signed_value;
        return true;
    }

    double raw_to_voltage(std::int32_t raw) const
    {
        constexpr double kFullScale = 8'388'607.0;
        return static_cast<double>(raw) * vref_volts_ / kFullScale;
    }

    bool sample_once()
    {
        if (mux_enabled_ && mux_channel_count_ > 1)
        {
            for (unsigned int mux_channel = 0; mux_channel < mux_channel_count_; ++mux_channel)
            {
                std::int32_t raw = 0;
                if (!set_mux_channel(mux_channel) || !read_raw(&raw))
                {
                    return false;
                }

                const double voltage = raw_to_voltage(raw);
                std::cout << format_voltage_line("mux", mux_channel, voltage, raw) << '\n' << std::flush;
            }

            return true;
        }

        std::int32_t raw = 0;
        if (!read_raw(&raw))
        {
            return false;
        }

        const double voltage = raw_to_voltage(raw);
        std::cout << format_voltage_line("", channel_, voltage, raw) << '\n' << std::flush;
        return true;
    }

    void poll_gate_inputs()
    {
        if (gate_input_fd_ < 0)
        {
            return;
        }

        std::array<unsigned char, 3> values{};
        if (!read_gates(&values))
        {
            return;
        }

        if (!have_last_gate_values_ || values != last_gate_values_)
        {
            last_gate_values_ = values;
            have_last_gate_values_ = true;
            std::cout << format_gate_line(values) << '\n' << std::flush;
        }
    }

    void run()
    {
        auto next_analog_sample = std::chrono::steady_clock::now();
        auto next_gate_poll = next_analog_sample;

        while (!stop_requested_.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();

            if (gate_input_fd_ >= 0 && now >= next_gate_poll)
            {
                poll_gate_inputs();
                next_gate_poll = now + gate_poll_period_;
            }

            if (now >= next_analog_sample)
            {
                if (!sample_once())
                {
                    stop_requested_.store(true, std::memory_order_release);
                    break;
                }

                next_analog_sample = now + sample_period_;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    void close_all()
    {
        if (mux_output_fd_ >= 0)
        {
            ::close(mux_output_fd_);
            mux_output_fd_ = -1;
        }

        if (gate_input_fd_ >= 0)
        {
            ::close(gate_input_fd_);
            gate_input_fd_ = -1;
        }

        if (gpio_input_fd_ >= 0)
        {
            ::close(gpio_input_fd_);
            gpio_input_fd_ = -1;
        }

        if (gpio_output_fd_ >= 0)
        {
            ::close(gpio_output_fd_);
            gpio_output_fd_ = -1;
        }

        if (gpio_chip_fd_ >= 0)
        {
            ::close(gpio_chip_fd_);
            gpio_chip_fd_ = -1;
        }

        if (spi_fd_ >= 0)
        {
            ::close(spi_fd_);
            spi_fd_ = -1;
        }
    }

    int gpio_chip_fd_{-1};
    int gpio_output_fd_{-1};
    int gpio_input_fd_{-1};
    int mux_output_fd_{-1};
    int gate_input_fd_{-1};
    int spi_fd_{-1};
    std::string gpio_chip_path_{"/dev/gpiochip0"};
    std::string spi_device_path_{"/dev/spidev0.0"};
    gpiohandle_data output_values_{};
    gpiohandle_data mux_output_values_{};
    std::array<unsigned char, 3> last_gate_values_{};
    unsigned int channel_{0};
    unsigned int mux_channel_count_{1};
    std::chrono::milliseconds gate_poll_period_{20};
    double vref_volts_{5.0};
    std::chrono::milliseconds sample_period_{1000};
    bool mux_enabled_{false};
    bool have_last_gate_values_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
};

WaveshareAds1256Monitor::WaveshareAds1256Monitor() : impl_{std::make_unique<Impl>()} {}

WaveshareAds1256Monitor::~WaveshareAds1256Monitor()
{
    stop();
}

bool WaveshareAds1256Monitor::start()
{
    return impl_ != nullptr && impl_->start();
}

void WaveshareAds1256Monitor::stop()
{
    if (impl_ != nullptr)
    {
        impl_->stop();
    }
}

} // namespace cockscreen::runtime

#endif