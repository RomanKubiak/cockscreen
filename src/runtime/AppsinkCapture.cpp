#include "cockscreen/runtime/AppsinkCapture.hpp"

#ifndef _WIN32

#include <QVideoFrame>
#include <QImage>
#include <QRunnable>
#include <QThreadPool>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cstring>
#include <iostream>
#include <functional>

namespace cockscreen::runtime
{

namespace
{

struct AppsinkContext
{
    QVideoSink *sink{nullptr};
    GstElement *pipeline{nullptr};
    GstElement *appsink{nullptr};
    GMainLoop  *loop{nullptr};
    std::atomic<bool> *running{nullptr};
    QString *status_message{nullptr};

    static GstFlowReturn on_new_sample(GstElement *appsink_elem, gpointer user_data)
    {
        auto *ctx = static_cast<AppsinkContext *>(user_data);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink_elem));
        if (sample == nullptr)
        {
            return GST_FLOW_OK;
        }

        GstBuffer *buf  = gst_sample_get_buffer(sample);
        GstCaps   *caps = gst_sample_get_caps(sample);

        GstVideoInfo vinfo;
        if (caps != nullptr && gst_video_info_from_caps(&vinfo, caps))
        {
            const int width  = static_cast<int>(GST_VIDEO_INFO_WIDTH(&vinfo));
            const int height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&vinfo));
            const int stride = static_cast<int>(GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0));

            GstMapInfo map;
            if (width > 0 && height > 0 && gst_buffer_map(buf, &map, GST_MAP_READ))
            {
                // BGRx → QImage::Format_RGB32  (0xffRRGGBB)
                QImage img{width, height, QImage::Format_RGB32};
                for (int row = 0; row < height; ++row)
                {
                    const quint8 *src = map.data + row * stride;
                    auto *dst = reinterpret_cast<quint32 *>(img.scanLine(row));
                    for (int col = 0; col < width; ++col, src += 4)
                    {
                        dst[col] = 0xff000000u
                                   | (static_cast<quint32>(src[2]) << 16)
                                   | (static_cast<quint32>(src[1]) << 8)
                                   | static_cast<quint32>(src[0]);
                    }
                }
                gst_buffer_unmap(buf, &map);

                if (ctx->sink != nullptr)
                {
                    ctx->sink->setVideoFrame(QVideoFrame{img});
                }
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    static gboolean bus_callback(GstBus * /*bus*/, GstMessage *msg, gpointer user_data)
    {
        auto *ctx = static_cast<AppsinkContext *>(user_data);
        switch (GST_MESSAGE_TYPE(msg))
        {
        case GST_MESSAGE_ERROR:
        {
            GError *err = nullptr;
            gchar  *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            if (ctx->status_message != nullptr)
            {
                *ctx->status_message = QStringLiteral("AppsinkCapture error: %1")
                                           .arg(err ? QString::fromUtf8(err->message) : QStringLiteral("unknown"));
            }
            std::cerr << "[appsink] "
                      << (err ? err->message : "unknown GStreamer error") << '\n';
            if (err) { g_error_free(err); }
            if (dbg) { g_free(dbg); }
            if (ctx->loop != nullptr) { g_main_loop_quit(ctx->loop); }
            break;
        }
        case GST_MESSAGE_EOS:
            if (ctx->loop != nullptr) { g_main_loop_quit(ctx->loop); }
            break;
        default:
            break;
        }
        return TRUE;
    }
};

// Plain POSIX thread function so we have no QObject/MOC dependency.
struct ThreadArg
{
    int udp_port;
    bool use_h264;
    QVideoSink *sink;
    // Shared state written by the thread, read by AppsinkCapture.
    std::atomic<bool> *running;
    QString *status_message;
    // Set by the thread so AppsinkCapture::stop() can quit the loop.
    GMainLoop **loop_out;
    std::mutex *mutex;
};

void *capture_thread(void *arg_ptr)
{
    auto *arg = static_cast<ThreadArg *>(arg_ptr);

    gst_init(nullptr, nullptr);

    std::string pipeline_str;
    if (arg->use_h264)
    {
        pipeline_str =
            "udpsrc port=" + std::to_string(arg->udp_port) +
            " caps=\"application/x-rtp,media=video,clock-rate=90000,"
            "encoding-name=H264,payload=96\" ! "
            "rtpjitterbuffer latency=60 ! "
            "rtph264depay ! h264parse ! avdec_h264 ! "
            "videoconvert ! video/x-raw,format=BGRx ! "
            "appsink name=sink sync=false max-buffers=2 drop=true emit-signals=true";
    }
    else
    {
        // MJPEG path (x86_64 without v4l2h264enc).
        pipeline_str =
            "udpsrc port=" + std::to_string(arg->udp_port) +
            " caps=\"application/x-rtp,media=video,clock-rate=90000,"
            "encoding-name=JPEG,payload=26\" ! "
            "rtpjitterbuffer latency=60 ! "
            "rtpjpegdepay ! jpegdec ! "
            "videoconvert ! video/x-raw,format=BGRx ! "
            "appsink name=sink sync=false max-buffers=2 drop=true emit-signals=true";
    }

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (pipeline == nullptr || error != nullptr)
    {
        *arg->status_message = QStringLiteral("AppsinkCapture: gst_parse_launch failed: %1")
                                    .arg(error ? QString::fromUtf8(error->message) : QStringLiteral("?"));
        if (error) { g_error_free(error); }
        arg->running->store(false);
        delete arg;
        return nullptr;
    }

    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    auto *ctx = new AppsinkContext;
    ctx->sink           = arg->sink;
    ctx->pipeline       = pipeline;
    ctx->appsink        = appsink;
    ctx->running        = arg->running;
    ctx->status_message = arg->status_message;

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    ctx->loop = loop;
    {
        std::lock_guard<std::mutex> lk{*arg->mutex};
        *arg->loop_out = loop;
    }

    if (appsink != nullptr)
    {
        g_signal_connect(appsink, "new-sample", G_CALLBACK(AppsinkContext::on_new_sample), ctx);
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, AppsinkContext::bus_callback, ctx);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    *arg->status_message =
        QStringLiteral("AppsinkCapture: running on UDP port %1").arg(arg->udp_port);
    arg->running->store(true);
    delete arg;

    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (appsink != nullptr) { gst_object_unref(appsink); }
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    ctx->running->store(false);
    delete ctx;

    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------

AppsinkCapture::~AppsinkCapture()
{
    stop();
}

bool AppsinkCapture::start(int udp_port, QVideoSink *sink, bool use_h264)
{
    stop();

    loop_         = nullptr;
    loop_mutex_   = std::make_unique<std::mutex>();

    auto *arg          = new ThreadArg;
    arg->udp_port      = udp_port;
    arg->use_h264      = use_h264;
    arg->sink          = sink;
    arg->running       = &running_;
    arg->status_message = &status_message_;
    arg->loop_out      = &loop_;
    arg->mutex         = loop_mutex_.get();

    pthread_create(&thread_, nullptr, capture_thread, arg);
    thread_started_ = true;

    // Wait briefly for the pipeline to initialise.
    for (int i = 0; i < 20; ++i)
    {
        if (running_.load()) { break; }
        QThread::msleep(10);
    }

    return true;
}

void AppsinkCapture::stop()
{
    if (!thread_started_) { return; }

    {
        std::lock_guard<std::mutex> lk{*loop_mutex_};
        if (loop_ != nullptr)
        {
            g_main_loop_quit(loop_);
        }
    }

    pthread_join(thread_, nullptr);
    thread_started_ = false;
    loop_ = nullptr;
}

bool AppsinkCapture::is_running() const
{
    return running_.load();
}

const QString &AppsinkCapture::status_message() const
{
    return status_message_;
}

} // namespace cockscreen::runtime

#endif // _WIN32
