#pragma once

#ifndef _WIN32

#include "Scene.hpp"

#include <QString>

class QProcess;

namespace cockscreen::runtime
{

// ---------------------------------------------------------------------------
// LoopbackPipeline — Step 2 (iproute2 / tc-netem variant).
//
// Manages a pair of GStreamer subprocesses that relay a video source through
// a local RTP-over-UDP loop so that Linux tc-netem can corrupt packets before
// they are decoded, producing authentic codec-level artefacts (macroblocking,
// P-frame smear, freeze) in the output.
//
// Topology:
//
//   [source: V4L2 device or file]
//          |
//   gst: encode → RTP → udpsink 127.0.0.1:<udp_port>
//          |
//   [loopback interface / tc-netem rule]
//          |
//   gst: udpsrc:<udp_port> → RTP depay → H.264 decode → v4l2sink <loopback_device>
//          |
//   [app reads /dev/videoN as a normal V4L2 camera]
//
// Prerequisites (not managed by this class):
//   - v4l2loopback module loaded:
//       modprobe v4l2loopback devices=1 video_nr=10 card_label="cockscreen-lb"
//   - gst-launch-1.0 available (gstreamer1.0-tools package)
//   - gstreamer1.0-plugins-good, -bad, -ugly (for x264enc / avdec_h264 / rtpXXX)
//   - On Pi: gstreamer1.0-plugins-bad for v4l2h264enc / v4l2h264dec (HW encode/decode)
//   - CAP_NET_ADMIN capability (or root) for tc commands
//
// Usage:
//   LoopbackPipeline pipeline;
//   if (pipeline.start_for_device("/dev/video0", 640, 480, params)) {
//       // use pipeline.output_device() as the V4L2 source for the window
//   }
// ---------------------------------------------------------------------------
class LoopbackPipeline
{
  public:
    LoopbackPipeline() = default;
    ~LoopbackPipeline();

    // Non-copyable.
    LoopbackPipeline(const LoopbackPipeline &) = delete;
    LoopbackPipeline &operator=(const LoopbackPipeline &) = delete;

    // Movable.
    LoopbackPipeline(LoopbackPipeline &&) noexcept;
    LoopbackPipeline &operator=(LoopbackPipeline &&) noexcept;

    // Start the relay for a live V4L2 capture device.
    // Returns true if both GStreamer processes started; the caller should wait
    // ~500 ms before opening the output device.
    [[nodiscard]] bool start_for_device(const std::string &source_device, int width, int height,
                                        const LoopbackParams &params);

    // Start the relay for a video file (decoded → re-encoded → loopback).
    // The file is played in a loop by GStreamer.
    [[nodiscard]] bool start_for_file(const std::string &source_file, const LoopbackParams &params);

    // Start only the sender side (encode → RTP → UDP + tc-netem).
    // No v4l2loopback receiver is started; the caller is expected to receive
    // frames via AppsinkCapture.  Requires CAP_NET_ADMIN for tc-netem.
    [[nodiscard]] bool start_sender_only_for_file(const std::string &source_file,
                                                   const LoopbackParams &params);

    // Returns true when the sender pipeline uses H.264, false when MJPEG.
    // Determined at build time by GStreamer element availability.
    [[nodiscard]] static bool uses_h264();

    // Stop both GStreamer processes and remove the tc-netem rule.
    void stop();

    // Returns params.loopback_device (the v4l2loopback device the app should read).
    [[nodiscard]] std::string output_device() const;

    // Human-readable status / error text (empty when all is well).
    [[nodiscard]] QString status_message() const;

    [[nodiscard]] bool is_running() const;

    // Check that v4l2loopback is loaded and the output device file exists.
    // Call this before start_for_device / start_for_file to get a clear
    // error message instead of a silent GStreamer failure.
    [[nodiscard]] static bool check_prerequisites(const LoopbackParams &params,
                                                  QString *error_message = nullptr);

    // Block until the v4l2loopback output device has a negotiated format,
    // which only happens once GStreamer's receiver has written its first frame.
    // Returns false (and sets status_message()) on timeout.
    [[nodiscard]] bool wait_for_device_ready(int timeout_ms = 5000);

  private:
    bool install_netem(const LoopbackParams &params);
    void remove_netem();
    static bool run_tc_command(const QStringList &args, QString *error_message = nullptr);
    static QString build_sender_pipeline_device(const std::string &device, int width, int height,
                                                const LoopbackParams &params);
    static QString build_sender_pipeline_file(const std::string &file, const LoopbackParams &params);
    static QString build_receiver_pipeline(const LoopbackParams &params);

    QProcess *sender_{nullptr};
    QProcess *receiver_{nullptr};
    LoopbackParams params_;
    bool netem_installed_{false};
    bool stopping_{false};
    QString sender_cmd_;
    QString status_message_;

    void connect_sender_restart();
    void start_sender_process();
};

} // namespace cockscreen::runtime

#endif // !_WIN32
