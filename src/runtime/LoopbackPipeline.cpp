#include "cockscreen/runtime/LoopbackPipeline.hpp"

#ifndef _WIN32

#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThread>

#include <utility>

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

    return QStringLiteral("gst-launch-1.0 -q "
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
    return QStringLiteral("gst-launch-1.0 -q "
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

    return QStringLiteral("gst-launch-1.0 -q "
                          "udpsrc port=%1 "
                          "caps=\"application/x-rtp,payload=96,encoding-name=H264,clock-rate=90000\" "
                          "! rtph264depay "
                          "! h264parse "
                          "! %2 "
                          "! videoconvert "
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
    proc.start(QStringLiteral("tc"), args);
    if (!proc.waitForFinished(5000))
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("tc command timed out");
        }
        return false;
    }

    if (proc.exitCode() != 0)
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("tc-netem setup failed; CAP_NET_ADMIN or root is required");
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

bool LoopbackPipeline::start_for_device(const std::string &source_device, int width, int height,
                                         const LoopbackParams &params)
{
    stop();
    params_ = params;

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

    if (!install_netem(params))
    {
        stop();
        return false;
    }
    QThread::msleep(200);

    const QString send_cmd = build_sender_pipeline_file(source_file, params);
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

void LoopbackPipeline::stop()
{
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

} // namespace cockscreen::runtime

#endif // !_WIN32
