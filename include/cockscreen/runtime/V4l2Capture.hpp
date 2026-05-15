#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cockscreen::runtime
{

enum class V4l2PixelFormat
{
    unsupported,
    yuyv,
    uyvy,
    rgb24,
    bgr24,
    bayer_bggr8,   // SBGGR8  — B G / G R  (pixel (0,0) is Blue)
    bayer_gbrg8,   // SGBRG8  — G B / R G  (OV5647 8-bit mode)
    bayer_grbg8,   // SGRBG8  — G R / B G
    bayer_rggb8,   // SRGGB8  — R G / G B  (IMX219, IMX477)
    bayer_bggr10,  // SBGGR10 — same pattern, 16-bit word per pixel (10-bit in low bits)
    bayer_gbrg10,  // SGBRG10 — G B / R G  (OV5647 native 10-bit output)
    bayer_grbg10,  // SGRBG10
    bayer_rggb10,  // SRGGB10 — R G / G B  (IMX219 native)
};

struct V4l2FrameView
{
    const std::uint8_t *data{nullptr};
    std::size_t size{0};
    int width{0};
    int height{0};
    int stride{0};
  int buffer_index{-1};
  int dmabuf_fd{-1};
    V4l2PixelFormat pixel_format{V4l2PixelFormat::unsupported};
};

class V4l2Capture
{
  public:
    V4l2Capture() = default;
    ~V4l2Capture();

    V4l2Capture(const V4l2Capture &) = delete;
    V4l2Capture &operator=(const V4l2Capture &) = delete;

    bool open(std::string_view device_path, int requested_width, int requested_height, bool prefer_rgb_capture = false);
    bool start();
    std::optional<V4l2FrameView> dequeue();
    void release();

    [[nodiscard]] static std::vector<std::string> enumerate_supported_modes(std::string_view device_path);

    [[nodiscard]] const std::string &error_message() const;
    [[nodiscard]] const std::string &format_label() const;
    [[nodiscard]] bool dmabuf_export_supported() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] V4l2PixelFormat pixel_format() const;

  private:
    struct Buffer
    {
        void *data{nullptr};
        std::size_t length{0};
        int dmabuf_fd{-1};
    };

    int fd_{-1};
    std::string device_path_;
    std::vector<Buffer> buffers_;
    int active_buffer_index_{-1};
    int width_{0};
    int height_{0};
    int stride_{0};
    V4l2PixelFormat pixel_format_{V4l2PixelFormat::unsupported};
    std::string format_label_{"unknown"};
    std::string error_message_;
    bool streaming_{false};
    bool dmabuf_export_supported_{false};
    bool prefer_rgb_capture_{false};
    bool is_mc_device_{false};

    bool configure_format(int requested_width, int requested_height, std::uint32_t mc_mbus_code_hint = 0);
    bool request_buffers();
    bool queue_all_buffers();
    void stop_stream();
    void close_device();
};

} // namespace cockscreen::runtime
