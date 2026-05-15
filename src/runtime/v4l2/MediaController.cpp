#include "cockscreen/runtime/v4l2/Support.hpp"

#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace cockscreen::runtime::v4l2
{

bool mc_setup_pipeline(std::string_view video_path, int width, int height, std::string *error_out,
                       std::uint32_t *out_mbus_code)
{
    struct stat video_stat{};
    if (::stat(std::string(video_path).c_str(), &video_stat) != 0)
    {
        if (error_out) *error_out = "Cannot stat " + std::string(video_path);
        return false;
    }
    const unsigned int target_major = major(video_stat.st_rdev);
    const unsigned int target_minor = minor(video_stat.st_rdev);

    // Find the media device that owns this video node.
    int media_fd = -1;
    __u32 capture_entity_id = 0;

    for (int n = 0; n < 16 && media_fd < 0; ++n)
    {
        const std::string mpath = "/dev/media" + std::to_string(n);
        const int fd = ::open(mpath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            break;

        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0)
        {
            if (entity.v4l.major == target_major && entity.v4l.minor == target_minor)
            {
                capture_entity_id = entity.id;
                media_fd = fd;
                break;
            }
            entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
        }

        if (media_fd < 0)
            ::close(fd);
    }

    if (media_fd < 0)
    {
        if (error_out) *error_out = "No media device found for " + std::string(video_path);
        return false;
    }

    // In kernel 6.x, entity.links counts only outgoing (source) links, so the
    // capture (sink) entity reports links=0. Enumerate all entities and check
    // their links for a connection whose sink is our capture entity.
    __u32 sensor_entity_id = 0;
    __u16 sensor_pad_index = 0;
    __u16 capture_pad_index = 0;

    {
        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0 && sensor_entity_id == 0)
        {
            const __u32 eid = entity.id;
            if (eid != capture_entity_id && entity.links > 0)
            {
                std::vector<media_pad_desc> pads(entity.pads);
                std::vector<media_link_desc> link_descs(entity.links);
                media_links_enum links_req{};
                links_req.entity = eid;
                links_req.pads = pads.empty() ? nullptr : pads.data();
                links_req.links = link_descs.data();

                if (ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links_req) == 0)
                {
                    for (const auto &link : link_descs)
                    {
                        if (link.sink.entity == capture_entity_id)
                        {
                            sensor_entity_id = link.source.entity;
                            sensor_pad_index = link.source.index;
                            capture_pad_index = link.sink.index;
                            break;
                        }
                    }
                }
            }
            entity.id = eid | MEDIA_ENT_ID_FLAG_NEXT;
        }
    }

    if (sensor_entity_id == 0)
    {
        ::close(media_fd);
        if (error_out) *error_out = "No source entity linked to capture device";
        return false;
    }

    // Enable the link (may already be enabled/immutable; ignore failure).
    media_link_desc enable_link{};
    enable_link.source.entity = sensor_entity_id;
    enable_link.source.index = sensor_pad_index;
    enable_link.sink.entity = capture_entity_id;
    enable_link.sink.index = capture_pad_index;
    enable_link.flags = MEDIA_LNK_FL_ENABLED;
    ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &enable_link);

    // Get sensor entity details to find its /dev/v4l-subdev path.
    media_entity_desc sensor_ent{};
    sensor_ent.id = sensor_entity_id;
    if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &sensor_ent) != 0)
    {
        ::close(media_fd);
        if (error_out) *error_out = "Failed to get sensor entity info";
        return false;
    }
    ::close(media_fd);

    // Locate the subdev character device by matching major:minor.
    std::string subdev_path;
    for (int n = 0; n < 16; ++n)
    {
        const std::string spath = "/dev/v4l-subdev" + std::to_string(n);
        struct stat sst{};
        if (::stat(spath.c_str(), &sst) != 0)
            break;
        if (major(sst.st_rdev) == sensor_ent.v4l.major &&
            minor(sst.st_rdev) == sensor_ent.v4l.minor)
        {
            subdev_path = spath;
            break;
        }
    }

    if (subdev_path.empty())
    {
        if (error_out) *error_out = "Cannot find v4l-subdev for sensor entity";
        return false;
    }

    // Set the sensor subdev format. Try 8-bit Bayer first, fall back to 10-bit.
    const int subdev_fd = ::open(subdev_path.c_str(), O_RDWR | O_NONBLOCK);
    if (subdev_fd < 0)
    {
        if (error_out) *error_out = "Cannot open sensor subdev " + subdev_path;
        return false;
    }

    const std::uint32_t mbus_codes[] = {
        MEDIA_BUS_FMT_SBGGR8_1X8,
        MEDIA_BUS_FMT_SRGGB8_1X8,
        MEDIA_BUS_FMT_SGRBG8_1X8,
        MEDIA_BUS_FMT_SGBRG8_1X8,
        MEDIA_BUS_FMT_SBGGR10_1X10,
        MEDIA_BUS_FMT_SRGGB10_1X10,
        MEDIA_BUS_FMT_SGRBG10_1X10,
        MEDIA_BUS_FMT_SGBRG10_1X10,
        MEDIA_BUS_FMT_SBGGR12_1X12,
        MEDIA_BUS_FMT_SRGGB12_1X12,
        MEDIA_BUS_FMT_SGRBG12_1X12,
        MEDIA_BUS_FMT_SGBRG12_1X12,
    };

    bool format_set = false;
    std::uint32_t negotiated_code = 0;
    for (const auto code : mbus_codes)
    {
        v4l2_subdev_format fmt{};
        fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        fmt.pad = sensor_pad_index;
        fmt.format.width = static_cast<__u32>(width);
        fmt.format.height = static_cast<__u32>(height);
        fmt.format.code = code;
        fmt.format.field = V4L2_FIELD_NONE;
        fmt.format.colorspace = V4L2_COLORSPACE_RAW;
        if (ioctl(subdev_fd, VIDIOC_SUBDEV_S_FMT, &fmt) == 0)
        {
            negotiated_code = fmt.format.code;  // read back what the sensor accepted
            format_set = true;
            break;
        }
    }
    ::close(subdev_fd);

    if (!format_set)
    {
        if (error_out) *error_out = "Failed to set sensor subdev format";
        return false;
    }

    if (out_mbus_code != nullptr)
        *out_mbus_code = negotiated_code;

    return true;
}

} // namespace cockscreen::runtime::v4l2
