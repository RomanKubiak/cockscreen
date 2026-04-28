#include "cockscreen/runtime/LoopbackPipeline.hpp"

#ifndef _WIN32

#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThread>

#include <chrono>
#include <iostream>
#include <utility>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace cockscreen::runtime
{

// ---------------------------------------------------------------------------
// GStreamer pipeline strings
// ---------------------------------------------------------------------------

// On Raspberry Pi the VideoCore hardware codecs are exposed as v4l2 elements;
// elsewhere (x86_64) we fall back to software x264enc / avdec_h264.
// We probe at runtime by checking whether the Pi-specific encoder element
// exists; if it does, we use hardware paths.
//
// Sender (V4L2 device source):
//   v4l2src → videoconvert → [hw|sw encode] → rtph264pay → udpsink
//
// Sender (file source):
//   filesrc → decodebin → videoconvert → [hw|sw encode] → rtph264pay → udpsink
//
// Receiver:
//   udpsrc → rtph264depay → h264parse → [hw|sw decode] → videoconvert → v4l2sink

static bool gst_element_exists(const QString &element_name)
{
    QProcess probe;
    probe.start(QStringLiteral("gst-inspect-1.0"),
                QStringList{QStringLiteral("--exists"), element_name});
    probe.waitForFinished(3000);
    return probe.exitCode() == 0;
}

static bool prefer_hardware_codecs()
{
    static const bool hw = gst_element_exists(QStringLiteral("v4l2h264enc"));
    return hw;
}

// ---------------------------------------------------------------------------

QString LoopbackPipeline::build_sender_pipeline_device(const std::string &device, int width, int height,
                                                        const LoopbackParams &params)
{
    const QString caps = QStringLiteral("video/x-raw,width=%1,height=%2").arg(width).arg(height);

    QString encode;
    if (prefer_hardware_codecs())
    {
        encode = QStringLiteral("v4l2h264enc extra-controls=\"encode,h264_level=11,h264_profile=1,video_bitrate=2000000\" "
                                "! video/x-h264,level=(string)3");
    }
    else
    {
        encode = QStringLiteral("x264enc tune=zerolatency bitrate=2000 speed-preset=ultrafast");
    }

    return QStringLiteral("gst-launch-1.0 "
                          "v4l2src device=%1 "
                          "! %2 "
                          "! videoconvert "
                          "! %3 "
                          "! rtph264pay config-interval=1 pt=96 "
                          "! udpsink host=127.0.0.1 port=%4")
        .arg(QString::fromStdString(device))
        .arg(caps)
        .arg(encode)
        .arg(params.udp_port);
}

QString LoopbackPipeline::build_sender_pipeline_file(const std::string &file,
                                                      const LoopbackParams &params)
{
    QString encode;
    if (prefer_hardware_codecs())
    {
        encode = QStringLiteral("v4l2h264enc extra-controls=\"encode,h264_level=11,h264_profile=1,video_bitrate=2000000\" "
                                "! video/x-h264,level=(string)3");
    }
    else
    {
        encode = QStringLiteral("x264enc tune=zerolatency bitrate=2000 speed-preset=ultrafast");
    }

    // decodebin handles any container/codec the system GStreamer supports.
    // "! queue" buffers between the async demuxer and the sync encoder.
    return QStringLiteral("gst-launch-1.0 "
                          "filesrc location=%1 "
                          "! decodebin "
                          "! queue max-size-buffers=4 leaky=downstream "
                          "! videoconvert "
                          "! %2 "
                          "! rtph264pay config-interval=1 pt=96 "
                          "! udpsink host=127.0.0.1 port=%3")
        .arg(QString::fromStdString(file))
        .arg(encode)
        .arg(params.udp_port);
}

QString LoopbackPipeline::build_receiver_pipeline(const LoopbackParams &params)
{
    QString decode;
    if (prefer_hardware_codecs())
    {
        decode = QStringLiteral("v4l2h264dec");
    }
    else
    {
        decode = QStringLiteral("avdec_h264");
    }

    return QStringLiteral("gst-launch-1.0 "
                          "udpsrc port=%1 "
                          "caps=\"application/x-rtp,payload=96,encoding-name=H264,clock-rate=90000\" "
                          "! rtph264depay "
                          "! h264parse "
                          "! %2 "
                          "! videoconvert "
                          "! video/x-raw,format=RGB "
                          "! v4l2sink device=%3 sync=false")
        .arg(params.udp_port)
        .arg(decode)
        .arg(QString::fromStdString(params.loopback_device));
}

// ---------------------------------------------------------------------------
// tc-netem helpers (requires CAP_NET_ADMIN or root)
// ---------------------------------------------------------------------------

// Build the netem options string, e.g. "loss 5% corrupt 10% delay 20ms".
static QString build_netem_opts(const LoopbackParams &params)
{
    QString opts;
    if (params.loss_percent > 0.0F)
    {
        opts += QStringLiteral(" loss %1%").arg(static_cast<double>(params.loss_percent), 0, 'f', 2);
    }
    if (params.corrupt_percent > 0.0F)
    {
        opts += QStringLiteral(" corrupt %1%").arg(static_cast<double>(params.corrupt_percent), 0, 'f', 2);
    }
    if (params.delay_ms > 0)
    {
        opts += QStringLiteral(" delay %1ms").arg(params.delay_ms);
    }
    if (params.reorder_percent > 0.0F && params.delay_ms > 0)
    {
        // Reorder requires a base delay in netem.
        opts += QStringLiteral(" reorder %1%").arg(static_cast<double>(params.reorder_percent), 0, 'f', 2);
    }
    return opts.trimmed();
}

bool LoopbackPipeline::run_tc_command(const QStringList &args, QString *error_message)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QStringLiteral("tc"), args);
    if (!proc.waitForFinished(5000))
    {
        if (error_message != nullptr)
            *error_message = QStringLiteral("tc command timed out");
        return false;
    }

    if (proc.exitCode() != 0)
    {
        if (error_message != nullptr)
        {
            const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
            *error_message = QStringLiteral("tc-netem setup failed; CAP_NET_ADMIN or root is required");
            if (!out.isEmpty())
                *error_message += QStringLiteral(": ") + out;
        }
        return false;
    }

    return true;
}

bool LoopbackPipeline::install_netem(const LoopbackParams &params)
{
    const QString netem_opts = build_netem_opts(params);
    if (netem_opts.isEmpty())
    {
        return true; // Nothing to configure.
    }

    // Topology:
    //   lo root: prio (4 bands, all normal by default)
    //     band 1:4 → netem (handles our UDP port)
    //   tc filter routes dport <udp_port> → 1:4

    // 0. Remove any leftover qdisc from a previous run that was hard-killed.
    //    This makes install_netem() idempotent; ignore failure (no existing qdisc is fine).
    run_tc_command(QStringList{
        QStringLiteral("qdisc"), QStringLiteral("del"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("root"),
    }, nullptr);

    // 1. Root prio qdisc (4 bands; default priomap sends everything to band 0).
    QString error_message;
    if (!run_tc_command(QStringList{
        QStringLiteral("qdisc"), QStringLiteral("add"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("root"), QStringLiteral("handle"), QStringLiteral("1:"),
        QStringLiteral("prio"),
        QStringLiteral("bands"), QStringLiteral("4"),
        QStringLiteral("priomap"),
        QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"),
        QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"),
        QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"),
        QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"),
    }, &error_message))
    {
        status_message_ = error_message;
        return false;
    }

    // 2. netem on band 4 (1:4).
    QStringList netem_args{
        QStringLiteral("qdisc"), QStringLiteral("add"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("parent"), QStringLiteral("1:4"),
        QStringLiteral("handle"), QStringLiteral("40:"),
        QStringLiteral("netem"),
    };
    for (const QString &opt : netem_opts.split(QLatin1Char(' '), Qt::SkipEmptyParts))
    {
        netem_args << opt;
    }
    if (!run_tc_command(netem_args, &error_message))
    {
        status_message_ = error_message;
        return false;
    }

    // 3. u32 filter: UDP destination port → band 1:4.
    if (!run_tc_command(QStringList{
        QStringLiteral("filter"), QStringLiteral("add"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("protocol"), QStringLiteral("ip"),
        QStringLiteral("parent"), QStringLiteral("1:"),
        QStringLiteral("prio"), QStringLiteral("1"),
        QStringLiteral("u32"),
        QStringLiteral("match"), QStringLiteral("ip"),
        QStringLiteral("dport"), QString::number(params.udp_port), QStringLiteral("0xffff"),
        QStringLiteral("flowid"), QStringLiteral("1:4"),
    }, &error_message))
    {
        status_message_ = error_message;
        return false;
    }

    netem_installed_ = true;
    return true;
}

void LoopbackPipeline::remove_netem()
{
    if (!netem_installed_)
    {
        return;
    }

    // Remove the filter first, then the child qdisc, then the root.
    run_tc_command(QStringList{
        QStringLiteral("filter"), QStringLiteral("del"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("protocol"), QStringLiteral("ip"),
        QStringLiteral("parent"), QStringLiteral("1:"),
        QStringLiteral("prio"), QStringLiteral("1"),
    });
    run_tc_command(QStringList{
        QStringLiteral("qdisc"), QStringLiteral("del"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("parent"), QStringLiteral("1:4"),
        QStringLiteral("handle"), QStringLiteral("40:"),
    });
    run_tc_command(QStringList{
        QStringLiteral("qdisc"), QStringLiteral("del"),
        QStringLiteral("dev"), QStringLiteral("lo"),
        QStringLiteral("root"), QStringLiteral("handle"), QStringLiteral("1:"),
    });

    netem_installed_ = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

LoopbackPipeline::~LoopbackPipeline()
{
    stop();
}

LoopbackPipeline::LoopbackPipeline(LoopbackPipeline &&other) noexcept
    : sender_{std::exchange(other.sender_, nullptr)},
      receiver_{std::exchange(other.receiver_, nullptr)},
      params_{std::move(other.params_)},
      netem_installed_{std::exchange(other.netem_installed_, false)},
      status_message_{std::move(other.status_message_)}
{
}

LoopbackPipeline &LoopbackPipeline::operator=(LoopbackPipeline &&other) noexcept
{
    if (this != &other)
    {
        stop();
        sender_ = std::exchange(other.sender_, nullptr);
        receiver_ = std::exchange(other.receiver_, nullptr);
        params_ = std::move(other.params_);
        netem_installed_ = std::exchange(other.netem_installed_, false);
        status_message_ = std::move(other.status_message_);
    }
    return *this;
}

// Clear any stale format left on the v4l2loopback device by a previous run.
static void reset_device_format(const std::string &device)
{
    const int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        std::cerr << "[LoopbackPipeline] reset_device_format: open failed on "
                  << device << ": " << strerror(errno)
                  << " — device may be held by another process\n";
        return;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    // Setting width=0/height=0/pixelformat=0 clears the format in v4l2loopback
    // when no reader currently has STREAMON active.
    ::ioctl(fd, VIDIOC_S_FMT, &fmt);
    ::close(fd);

    std::cerr << "[LoopbackPipeline] cleared stale format on " << device << "\n";
}

bool LoopbackPipeline::start_for_device(const std::string &source_device, int width, int height,
                                         const LoopbackParams &params)
{
    stop();
    params_ = params;

    reset_device_format(params.loopback_device);

    // Start the receiver first so udpsrc is listening before the sender fires.
    const QString recv_cmd = build_receiver_pipeline(params);
    receiver_ = new QProcess;
    receiver_->start(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), recv_cmd});
    if (!receiver_->waitForStarted(3000))
    {
        status_message_ = QStringLiteral("LoopbackPipeline: receiver failed to start: ") +
                          receiver_->errorString();
        delete receiver_;
        receiver_ = nullptr;
        return false;
    }

    // Install tc-netem before the sender starts sending packets.
    if (!install_netem(params))
    {
        stop();
        return false;
    }

    // Small delay so the receiver is fully initialised.
    QThread::msleep(200);

    const QString send_cmd = build_sender_pipeline_device(source_device, width, height, params);
    sender_ = new QProcess;
    sender_->start(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), send_cmd});
    if (!sender_->waitForStarted(3000))
    {
        status_message_ = QStringLiteral("LoopbackPipeline: sender failed to start: ") +
                          sender_->errorString();
        stop();
        return false;
    }

    status_message_.clear();
    return true;
}

bool LoopbackPipeline::start_for_file(const std::string &source_file, const LoopbackParams &params)
{
    stop();
    params_ = params;

    reset_device_format(params.loopback_device);

    // Receiver: run in its own session (setsid) so that when the sender's
    // gst-launch-1.0 exits via kill(0,SIGTERM) on EOS it doesn't propagate
    // SIGTERM to the receiver process group.
    const QString recv_cmd = QStringLiteral("setsid ") + build_receiver_pipeline(params);
    std::cerr << "[LoopbackPipeline] receiver cmd: " << recv_cmd.toStdString() << "\n";
    receiver_ = new QProcess;
    receiver_->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(receiver_, &QProcess::readyReadStandardOutput, receiver_, [this]() {
        const QByteArray out = receiver_->readAllStandardOutput();
        if (!out.trimmed().isEmpty())
            std::cerr << "[gst-receiver] " << out.toStdString();
    });
    QObject::connect(receiver_, &QProcess::finished, receiver_, [this](int code, QProcess::ExitStatus) {
        std::cerr << "[LoopbackPipeline] receiver exited with code " << code << "\n";
        if (!stopping_ && receiver_ != nullptr)
        {
            std::cerr << "[LoopbackPipeline] restarting receiver\n";
            receiver_->start(QStringLiteral("/bin/sh"),
                             QStringList{QStringLiteral("-c"),
                                         QStringLiteral("setsid ") + build_receiver_pipeline(params_)});
        }
    });
    receiver_->start(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), recv_cmd});
    if (!receiver_->waitForStarted(3000))
    {
        status_message_ = QStringLiteral("LoopbackPipeline: receiver failed to start: ") +
                          receiver_->errorString();
        delete receiver_;
        receiver_ = nullptr;
        return false;
    }

    if (!install_netem(params))
    {
        stop();
        return false;
    }
    QThread::msleep(200);

    // Sender: also in its own session. Stored as sender_cmd_ so connect_sender_restart()
    // can restart it automatically when gst-launch-1.0 exits on EOS.
    sender_cmd_ = QStringLiteral("setsid ") + build_sender_pipeline_file(source_file, params);
    std::cerr << "[LoopbackPipeline] sender cmd: " << sender_cmd_.toStdString() << "\n";
    start_sender_process();
    if (sender_ == nullptr || !sender_->waitForStarted(3000))
    {
        status_message_ = QStringLiteral("LoopbackPipeline: sender failed to start");
        stop();
        return false;
    }

    status_message_.clear();
    return true;
}

void LoopbackPipeline::start_sender_process()
{
    if (sender_ != nullptr)
    {
        sender_->disconnect();
        sender_->deleteLater();
    }

    sender_ = new QProcess;
    sender_->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(sender_, &QProcess::readyReadStandardOutput, sender_, [this]() {
        const QByteArray out = sender_->readAllStandardOutput();
        if (!out.trimmed().isEmpty())
            std::cerr << "[gst-sender] " << out.toStdString();
    });
    QObject::connect(sender_, &QProcess::finished, sender_, [this](int code, QProcess::ExitStatus) {
        std::cerr << "[LoopbackPipeline] sender exited with code " << code;
        if (stopping_)
        {
            std::cerr << " (stopped)\n";
            return;
        }
        // gst-launch-1.0 exits via kill(0,SIGTERM) on EOS — restart to loop.
        std::cerr << " — restarting for loop\n";
        start_sender_process();
        if (sender_ != nullptr)
            sender_->start(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), sender_cmd_});
    });
    sender_->start(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), sender_cmd_});
}

void LoopbackPipeline::stop()
{
    stopping_ = true;
    remove_netem();

    if (sender_ != nullptr)
    {
        sender_->terminate();
        if (!sender_->waitForFinished(2000))
        {
            sender_->kill();
        }
        delete sender_;
        sender_ = nullptr;
    }

    if (receiver_ != nullptr)
    {
        receiver_->terminate();
        if (!receiver_->waitForFinished(2000))
        {
            receiver_->kill();
        }
        delete receiver_;
        receiver_ = nullptr;
    }

    stopping_ = false;
}

std::string LoopbackPipeline::output_device() const
{
    return params_.loopback_device;
}

QString LoopbackPipeline::status_message() const
{
    return status_message_;
}

bool LoopbackPipeline::is_running() const
{
    return sender_ != nullptr && receiver_ != nullptr;
}

bool LoopbackPipeline::check_prerequisites(const LoopbackParams &params, QString *error_message)
{
    // Verify that the v4l2loopback kernel module is loaded.
    // Use readAll() — /proc files report size 0 so atEnd()-based loops never run.
    QFile modules_file(QStringLiteral("/proc/modules"));
    if (modules_file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QString content = QString::fromUtf8(modules_file.readAll());
        modules_file.close();
        // Match "v4l2loopback " (with trailing space) to avoid partial matches.
        if (!content.contains(QStringLiteral("v4l2loopback ")))
        {
            if (error_message != nullptr)
            {
                *error_message = QStringLiteral(
                    "v4l2loopback kernel module is not loaded.\n"
                    "Load it with:\n"
                    "  sudo modprobe v4l2loopback devices=1 video_nr=10 card_label=cockscreen-lb exclusive_caps=1");
            }
            return false;
        }
    }

    // Verify the output device file exists.
    const QString device_path = QString::fromStdString(params.loopback_device);
    if (!QFile::exists(device_path))
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral(
                "Loopback output device '%1' does not exist.\n"
                "Check that v4l2loopback was loaded with the correct video_nr.")
                    .arg(device_path);
        }
        return false;
    }

    // Try to open the device as output to detect exclusive_caps conflicts.
    // v4l2loopback with exclusive_caps=1 returns EBUSY from open() if another
    // process already holds the device as an output writer.
    {
        const int test_fd = ::open(params.loopback_device.c_str(), O_RDWR | O_NONBLOCK);
        if (test_fd < 0)
        {
            if (errno == EBUSY)
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral(
                        "Loopback device '%1' is held exclusively by another process.\n"
                        "Find what has it open:\n"
                        "  sudo fuser %1\n"
                        "Release it:\n"
                        "  sudo fuser -k %1").arg(device_path);
                }
                return false;
            }
            // Other errors (permissions etc.) — let the pipeline fail with its own message.
        }
        else
        {
            ::close(test_fd);
        }
    }

    return true;
}

bool LoopbackPipeline::wait_for_device_ready(int timeout_ms)
{
    // Poll the v4l2loopback device until VIDIOC_G_FMT reports RGB24, which is
    // the exact format our receiver pipeline writes. Checking for any non-zero
    // format is insufficient — a previous run may have left a stale entry.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    const std::string device = params_.loopback_device;

    while (std::chrono::steady_clock::now() < deadline)
    {
        // Flush any pending GStreamer output inline — the Qt event loop is
        // blocked here so queued signals won't fire until we return.
        if (receiver_ != nullptr)
        {
            const QByteArray recv_out = receiver_->readAllStandardOutput();
            if (!recv_out.trimmed().isEmpty())
                std::cerr << "[gst-receiver] " << recv_out.toStdString();

            if (receiver_->state() != QProcess::Running)
            {
                status_message_ = QStringLiteral(
                    "Loopback receiver process exited unexpectedly (code %1) "
                    "before the device became ready")
                    .arg(receiver_->exitCode());
                std::cerr << "[LoopbackPipeline] " << status_message_.toStdString() << "\n";
                return false;
            }
        }
        if (sender_ != nullptr)
        {
            const QByteArray send_out = sender_->readAllStandardOutput();
            if (!send_out.trimmed().isEmpty())
                std::cerr << "[gst-sender] " << send_out.toStdString();
        }

        const int fd = ::open(device.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0)
        {
            v4l2_format fmt{};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            const bool got_format = (::ioctl(fd, VIDIOC_G_FMT, &fmt) == 0);
            ::close(fd);

            char cc[5] = {};
            std::memcpy(cc, &fmt.fmt.pix.pixelformat, 4);
            std::cerr << "[LoopbackPipeline] poll: fourcc=" << (got_format ? cc : "?")
                      << " " << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height << "\n";

            if (got_format && fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24 &&
                fmt.fmt.pix.width > 0 && fmt.fmt.pix.height > 0)
            {
                std::cerr << "[LoopbackPipeline] device ready: "
                          << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height
                          << " fourcc=RGB24"
                          << " stride=" << fmt.fmt.pix.bytesperline
                          << "\n";
                return true;
            }
        }
        QThread::msleep(150);
    }

    status_message_ = QStringLiteral(
        "Timed out waiting for loopback device '%1' to become ready. "
        "GStreamer may not have written its first frame in time.")
                          .arg(QString::fromStdString(device));
    return false;
}

} // namespace cockscreen::runtime

#endif // !_WIN32
