#pragma once

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cockscreen::runtime::v4l2
{

struct IspBuffer
{
    void       *data = nullptr;
    std::size_t size = 0;
};

// Routes raw Bayer frames through the bcm2835-isp hardware ISP.
//
// Buffer strategy: both input and output sides use V4L2_MEMORY_MMAP.
// queue_input() memcpys the unicam frame into a free ISP input buffer so the
// unicam buffer can be released immediately.  The ISP processes the frame and
// the caller dequeues YUYV via dequeue_output()/release_output().
class IspPipeline
{
public:
    ~IspPipeline();

    // Scan /dev/video* for bcm2835-isp input and output nodes.
    static bool find_devices(std::string *input_dev, std::string *output_dev);

    // Open and configure both ISP nodes.
    // input_fourcc:  e.g. V4L2_PIX_FMT_SGBRG10
    // output_fourcc: e.g. V4L2_PIX_FMT_YUYV
    bool open(const std::string &input_dev, const std::string &output_dev,
              int width, int height,
              std::uint32_t input_fourcc, std::uint32_t output_fourcc,
              std::string *error_out = nullptr);

    bool start();
    void stop();
    void close();

    // Copy raw frame data into a free ISP input buffer and submit it.
    // Caller may release the source buffer immediately after this returns true.
    // Returns false if no free input buffer is available.
    bool queue_input(const void *data, std::size_t size);

    // Dequeue one processed frame from the ISP output.
    // Also recycles the corresponding input buffer.
    // Returns nullopt if no frame is ready (non-blocking).
    std::optional<V4l2FrameView> dequeue_output();

    // Re-queue the last dequeued output buffer for reuse.
    void release_output();

    V4l2PixelFormat output_pixel_format() const { return output_pixel_format_; }
    int output_width()  const { return output_width_; }
    int output_height() const { return output_height_; }
    bool is_open()      const { return input_fd_ >= 0 && output_fd_ >= 0; }

    const std::string &error_message() const { return error_; }

private:
    int input_fd_  = -1;
    int output_fd_ = -1;

    int output_width_  = 0;
    int output_height_ = 0;
    int output_stride_ = 0;
    V4l2PixelFormat output_pixel_format_ = V4l2PixelFormat::unsupported;

    std::vector<IspBuffer> input_buffers_;
    std::vector<bool>      input_buffer_free_;   // true = available for queue_input
    int active_input_index_ = -1;                // index currently held by ISP

    std::vector<IspBuffer> output_buffers_;
    int active_output_index_ = -1;

    bool streaming_ = false;
    std::string error_;
};

} // namespace cockscreen::runtime::v4l2
