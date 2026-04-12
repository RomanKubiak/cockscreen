#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QRawFont>

#include <algorithm>

namespace cockscreen::runtime::shader_window
{

namespace
{

QString strip_precision_qualifiers(QString source)
{
    QString result;
    const auto lines = source.split('\n');
    result.reserve(source.size());

    for (const auto &line : lines)
    {
        if (line.trimmed().startsWith(QStringLiteral("precision ")))
        {
            continue;
        }

        result += line;
        result += QLatin1Char('\n');
    }

    if (!result.isEmpty())
    {
        result.chop(1);
    }

    return result;
}

QString note_label_for_midi_note(int note_number)
{
    static constexpr const char *kPitchClasses[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int normalized = std::clamp(note_number, 0, 127);
    const int pitch_class = normalized % 12;
    const int octave = normalized / 12 - 1;
    return QStringLiteral("%1%2").arg(QString::fromUtf8(kPitchClasses[pitch_class])).arg(octave);
}

} // namespace

const char *vertex_shader_source()
{
    return R"(
    precision mediump float;
    attribute vec2 a_position;
    attribute vec2 a_texcoord;
    varying vec2 v_texcoord;
    uniform vec2 u_viewport_size;
    uniform vec2 u_video_size;
    uniform float u_status_bar_height;
    void main()
    {
        vec2 usable = vec2(u_viewport_size.x, max(u_viewport_size.y - u_status_bar_height, 1.0));
        vec2 origin = floor((usable - u_video_size) * 0.5);
        vec2 pixel = origin + a_position * u_video_size;
        vec2 ndc = vec2((pixel.x / u_viewport_size.x) * 2.0 - 1.0,
                        1.0 - (pixel.y / u_viewport_size.y) * 2.0);
        gl_Position = vec4(ndc, 0.0, 1.0);
        v_texcoord = vec2(a_texcoord.x, 1.0 - a_texcoord.y);
    }
)";
}

const char *fullscreen_vertex_shader_source()
{
    return R"(
    attribute vec2 a_position;
    attribute vec2 a_texcoord;
    varying vec2 v_texcoord;
    void main()
    {
        gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);
        v_texcoord = a_texcoord;
    }
)";
}

const char *passthrough_fragment_shader_source()
{
    return R"(
    precision mediump float;
    varying vec2 v_texcoord;
    uniform sampler2D u_texture;
    uniform float u_opacity;
    void main()
    {
        vec4 color = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(color.rgb, color.a * u_opacity);
    }
)";
}

QString shader_source_for_current_context(QString source)
{
    const auto *context = QOpenGLContext::currentContext();
    if (context != nullptr && !context->isOpenGLES())
    {
        return strip_precision_qualifiers(std::move(source));
    }

    return source;
}

void set_midi_uniforms(QOpenGLShaderProgram *program, const core::ControlFrame &frame)
{
    if (program == nullptr)
    {
        return;
    }

    program->setUniformValue("u_midi_primary", frame.midi_primary);
    program->setUniformValue("u_midi_secondary", frame.midi_secondary);
    program->setUniformValueArray("u_midi_notes", frame.midi_notes.data(), static_cast<int>(frame.midi_notes.size()), 1);
    program->setUniformValueArray("u_midi_velocities", frame.midi_velocities.data(),
                                  static_cast<int>(frame.midi_velocities.size()), 1);
    program->setUniformValueArray("u_midi_ages", frame.midi_ages.data(), static_cast<int>(frame.midi_ages.size()), 1);
    program->setUniformValueArray("u_midi_channels", frame.midi_channels.data(),
                                  static_cast<int>(frame.midi_channels.size()), 1);
}

void set_audio_uniforms(QOpenGLShaderProgram *program, const core::ControlFrame &frame)
{
    if (program == nullptr)
    {
        return;
    }

    program->setUniformValue("u_audio_level", frame.audio_level);
    program->setUniformValue("u_audio_rms", frame.audio_rms);
    program->setUniformValue("u_audio_peak", frame.audio_peak);
    program->setUniformValueArray("u_audio_fft", frame.audio_fft_bands.data(),
                                  static_cast<int>(frame.audio_fft_bands.size()), 1);
    program->setUniformValueArray("u_audio_waveform", frame.audio_waveform.data(),
                                  static_cast<int>(frame.audio_waveform.size()), 1);
}

QString read_text_file_qstring(const std::filesystem::path &path)
{
    const auto text = read_text_file(path);
    if (!text.has_value())
    {
        return {};
    }

    return QString::fromStdString(*text);
}

std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &relative_path)
{
    if (relative_path.is_absolute())
    {
        return std::filesystem::exists(relative_path) ? std::optional{relative_path} : std::nullopt;
    }

    auto current = std::filesystem::current_path();
    while (true)
    {
        const auto candidate = current / relative_path;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }

        if (!current.has_parent_path() || current == current.parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return std::nullopt;
}

QColor scene_clear_color(const SceneColor &color)
{
    return QColor::fromRgbF(color.red, color.green, color.blue, color.alpha);
}

std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene, const ApplicationSettings &settings)
{
    if (const auto requested = parse_capture_mode_dimensions(scene.video_input.format); requested.has_value())
    {
        return *requested;
    }

    return {settings.width, settings.height};
}

QRectF video_display_rect(const SceneInput &video_input, const QSize &viewport_size)
{
    const float scale = std::max(video_input.scale, 0.01F);
    const float width = static_cast<float>(viewport_size.width()) * scale;
    const float height = static_cast<float>(viewport_size.height()) * scale;
    const float x = std::clamp(video_input.position_x, 0.0F, 1.0F) * (static_cast<float>(viewport_size.width()) - width);
    const float y = std::clamp(video_input.position_y, 0.0F, 1.0F) * (static_cast<float>(viewport_size.height()) - height);
    return QRectF{x, y, width, height};
}

std::optional<std::filesystem::path> resolve_scene_resource_path(const std::filesystem::path &resources_directory,
                                                                 const std::string &resource_file)
{
    if (resource_file.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path requested{resource_file};
    if (requested.is_absolute())
    {
        return std::filesystem::exists(requested) ? std::optional{requested} : std::nullopt;
    }

    const auto candidate = resources_directory / requested;
    return std::filesystem::exists(candidate) ? std::optional{candidate} : std::nullopt;
}

QString note_font_family_for_scene(const SceneDefinition &scene)
{
    const auto font_path = resolve_scene_resource_path(scene.resources_directory, scene.note_font_file);
    if (!font_path.has_value())
    {
        return QStringLiteral("Sans Serif");
    }

    const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(font_path->string()));
    if (font_id < 0)
    {
        return QStringLiteral("Sans Serif");
    }

    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty())
    {
        return QStringLiteral("Sans Serif");
    }

    return families.front();
}

bool image_has_opaque_pixels(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
    {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(row[x]) > 0)
            {
                return true;
            }
        }
    }

    return false;
}

float image_opaque_coverage(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
    {
        return 0.0F;
    }

    int opaque_pixels = 0;
    const int total_pixels = image.width() * image.height();

    for (int y = 0; y < image.height(); ++y)
    {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(row[x]) > 0)
            {
                ++opaque_pixels;
            }
        }
    }

    return static_cast<float>(opaque_pixels) / static_cast<float>(std::max(total_pixels, 1));
}

QImage build_note_label_atlas_image(const QString &font_family)
{
    QImage atlas{kNoteAtlasColumns * kNoteAtlasCellSize, kNoteAtlasRows * kNoteAtlasCellSize, QImage::Format_RGBA8888};
    atlas.fill(Qt::transparent);

    QPainter painter{&atlas};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font{font_family};
    font.setFixedPitch(true);
    font.setBold(false);
    font.setWeight(QFont::Normal);
    font.setStyleHint(QFont::Monospace, QFont::PreferMatch);
    font.setHintingPreference(QFont::PreferFullHinting);

    const QString longest_label = QStringLiteral("C#-1");
    const float target_width = static_cast<float>(kNoteAtlasCellSize) * 0.84F;
    const float target_height = static_cast<float>(kNoteAtlasCellSize) * 0.58F;
    int pixel_size = static_cast<int>(kNoteAtlasCellSize * 0.50F);
    for (; pixel_size >= 8; --pixel_size)
    {
        font.setPixelSize(pixel_size);
        const QFontMetricsF metrics{font};
        if (metrics.horizontalAdvance(longest_label) <= target_width && metrics.height() <= target_height)
        {
            break;
        }
    }
    font.setPixelSize(std::max(pixel_size, 8));
    painter.setFont(font);
    painter.setPen(Qt::white);

    for (int note = 0; note < 128; ++note)
    {
        const int column = note % kNoteAtlasColumns;
        const int row = note / kNoteAtlasColumns;
        const QRect cell{column * kNoteAtlasCellSize, row * kNoteAtlasCellSize, kNoteAtlasCellSize, kNoteAtlasCellSize};
        painter.drawText(cell, Qt::AlignCenter, note_label_for_midi_note(note));
    }

    return atlas;
}

QImage vertically_flipped_image(const QImage &image)
{
    return image.mirrored(false, true);
}

QImage build_icon_atlas_image(const std::filesystem::path &font_path)
{
    const int atlas_w = kIconAtlasColumns * kIconAtlasCellSize;
    const int atlas_h = kIconAtlasRows * kIconAtlasCellSize;
    QImage atlas{atlas_w, atlas_h, QImage::Format_RGBA8888};
    atlas.fill(Qt::transparent);

    const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(font_path.string()));
    if (font_id < 0)
    {
        return atlas;
    }

    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty())
    {
        return atlas;
    }

    QFont font{families.front()};
    font.setPixelSize(static_cast<int>(kIconAtlasCellSize * 0.72F));
    font.setHintingPreference(QFont::PreferFullHinting);

    // Dynamically discover valid icon codepoints supported by this font
    const QRawFont raw_font = QRawFont::fromFont(font);
    std::vector<char32_t> valid_cp;
    valid_cp.reserve(static_cast<std::size_t>(kIconAtlasColumns * kIconAtlasRows));

    const int needed = kIconAtlasColumns * kIconAtlasRows;
    // Scan the Private Use Area ranges used by icon fonts
    for (uint32_t range_start : {0xE000u, 0xF000u, 0xF500u, 0xF800u})
    {
        for (uint32_t cp = range_start; cp < range_start + 0x500u && static_cast<int>(valid_cp.size()) < needed; ++cp)
        {
            if (raw_font.supportsCharacter(cp))
            {
                valid_cp.push_back(static_cast<char32_t>(cp));
            }
        }
        if (static_cast<int>(valid_cp.size()) >= needed)
        {
            break;
        }
    }

    if (valid_cp.empty())
    {
        return atlas;
    }

    // Pad to full grid with repeats of found codepoints
    const std::size_t found = valid_cp.size();
    while (static_cast<int>(valid_cp.size()) < needed)
    {
        valid_cp.push_back(valid_cp[valid_cp.size() % found]);
    }

    QPainter painter{&atlas};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(Qt::white);

    for (int i = 0; i < needed; ++i)
    {
        const int col = i % kIconAtlasColumns;
        const int row = i / kIconAtlasColumns;
        const QRect cell{col * kIconAtlasCellSize, row * kIconAtlasCellSize, kIconAtlasCellSize, kIconAtlasCellSize};
        painter.drawText(cell, Qt::AlignCenter, QString::fromUcs4(&valid_cp[static_cast<std::size_t>(i)], 1));
    }

    return atlas;
}

} // namespace cockscreen::runtime::shader_window