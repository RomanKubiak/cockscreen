#include "cockscreen/runtime/web/SceneControlServer.hpp"

#include "cockscreen/app/CliSupport.hpp"
#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/application/Support.hpp"
#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QMediaDevices>
#include <QTcpSocket>
#include <QUrl>

#include <algorithm>
#include <map>

namespace cockscreen::runtime
{

namespace
{

bool has_suffix(const std::string &value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_scene_preset_file(const std::filesystem::path &path)
{
    if (!path.has_filename())
    {
        return false;
    }

    const auto name = path.filename().string();
    return has_suffix(name, ".scene.json") || has_suffix(name, ".scene.jsonc");
}

std::string scene_label_for_path(const std::filesystem::path &path)
{
    auto name = path.filename().string();
    if (has_suffix(name, ".scene.jsonc"))
    {
        name.resize(name.size() - std::string{".scene.jsonc"}.size());
    }
    else if (has_suffix(name, ".scene.json"))
    {
        name.resize(name.size() - std::string{".scene.json"}.size());
    }
    return name;
}

std::filesystem::path preset_root_directory(const std::filesystem::path &scene_file)
{
    if (scene_file.empty())
    {
        return {};
    }

    for (auto current = scene_file.parent_path(); !current.empty(); current = current.parent_path())
    {
        if (current.filename() == "scenes")
        {
            return current;
        }

        if (!current.has_parent_path() || current == current.parent_path())
        {
            break;
        }
    }

    return scene_file.parent_path();
}

std::string relative_path_string(const std::filesystem::path &path, const std::filesystem::path &root)
{
    if (path.empty() || root.empty())
    {
        return {};
    }

    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return error ? std::string{} : relative.generic_string();
}

QString preset_group_label(const std::string &group_path)
{
    return group_path.empty() ? QStringLiteral("root") : QString::fromStdString(group_path);
}

QJsonObject preset_groups_to_json(const std::filesystem::path &root, const std::filesystem::path &scene_file,
                                  bool active_scene_read_only)
{
    QJsonObject result;
    result.insert(QStringLiteral("root"), QString::fromStdString(root.generic_string()));
    result.insert(QStringLiteral("activeSceneReadOnly"), active_scene_read_only);
    result.insert(QStringLiteral("saveMode"),
                  active_scene_read_only ? QStringLiteral("new-preset-only") : QStringLiteral("in-place-or-new"));

    if (root.empty() || !std::filesystem::exists(root))
    {
        result.insert(QStringLiteral("activeScenePath"), QStringLiteral(""));
        result.insert(QStringLiteral("activeGroupPath"), QStringLiteral(""));
        result.insert(QStringLiteral("groups"), QJsonArray{});
        return result;
    }

    std::map<std::string, std::vector<std::filesystem::path>> grouped_presets;
    std::error_code error;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_regular_file() || !is_scene_preset_file(entry.path()))
        {
            continue;
        }

        std::error_code relative_error;
        const auto relative = std::filesystem::relative(entry.path(), root, relative_error);
        if (relative_error)
        {
            continue;
        }

        grouped_presets[relative.parent_path().generic_string()].push_back(relative);
    }

    QJsonArray groups;
    for (auto &[group_path, presets] : grouped_presets)
    {
        std::sort(presets.begin(), presets.end());
        QJsonArray scenes;
        for (const auto &preset : presets)
        {
            scenes.push_back(QJsonObject{{QStringLiteral("path"), QString::fromStdString(preset.generic_string())},
                                         {QStringLiteral("label"), QString::fromStdString(scene_label_for_path(preset))}});
        }

        groups.push_back(QJsonObject{{QStringLiteral("path"), QString::fromStdString(group_path)},
                                     {QStringLiteral("label"), preset_group_label(group_path)},
                                     {QStringLiteral("scenes"), scenes}});
    }

    const auto active_scene_path = relative_path_string(scene_file, root);
    const std::filesystem::path active_scene_relative_path{active_scene_path};
    result.insert(QStringLiteral("activeScenePath"), QString::fromStdString(active_scene_path));
    result.insert(QStringLiteral("activeGroupPath"), QString::fromStdString(active_scene_relative_path.parent_path().generic_string()));
    result.insert(QStringLiteral("groups"), groups);
    return result;
}

bool validate_scene_shader_files(const SceneDefinition &scene, const std::filesystem::path &shader_directory,
                                 QString *error_message)
{
    const auto validate_layer = [&](const char *layer_name, const SceneLayer &layer) -> bool {
        for (const auto &shader : layer.shaders)
        {
            if (shader.empty())
            {
                continue;
            }

            std::filesystem::path shader_path{shader};
            if (!shader_path.is_absolute())
            {
                shader_path = shader_directory / shader_path;
            }

            if (!application_support::resolve_relative_path(shader_path).has_value())
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Missing scene shader [%1]: %2")
                                         .arg(QString::fromUtf8(layer_name), QString::fromStdString(shader));
                }
                return false;
            }
        }

        return true;
    };

    return validate_layer("video", scene.video_layer) && validate_layer("playback", scene.playback_layer) &&
           validate_layer("screen", scene.screen_layer);
}

QString placement_to_string(BackgroundImagePlacement placement)
{
    switch (placement)
    {
        case BackgroundImagePlacement::Center:
            return QStringLiteral("center");
        case BackgroundImagePlacement::Stretched:
            return QStringLiteral("stretched");
        case BackgroundImagePlacement::ProportionalStretch:
            return QStringLiteral("proportional-stretch");
        case BackgroundImagePlacement::Tiled:
            return QStringLiteral("tiled");
    }

    return QStringLiteral("center");
}

QJsonArray layer_order_to_json(const std::vector<std::string> &layer_order)
{
    QJsonArray result;
    for (const auto &layer_name : layer_order)
    {
        result.push_back(QString::fromStdString(layer_name));
    }
    return result;
}

QJsonObject playback_input_to_json(const SceneInput &input)
{
    return QJsonObject{{QStringLiteral("enabled"), input.enabled},
                       {QStringLiteral("file"), QString::fromStdString(input.file)},
                       {QStringLiteral("startMs"), static_cast<double>(input.start_ms)},
                       {QStringLiteral("loopStartMs"), static_cast<double>(input.loop_start_ms)},
                       {QStringLiteral("loopEndMs"),
                        input.loop_end_ms.has_value() ? QJsonValue{static_cast<double>(*input.loop_end_ms)}
                                                      : QJsonValue{QJsonValue::Null}},
                       {QStringLiteral("loopRepeat"), input.loop_repeat},
                       {QStringLiteral("playbackRate"), input.playback_rate},
                       {QStringLiteral("playbackRateLooping"), input.playback_rate_looping}};
}

QJsonObject pink_key_to_json(const PinkKeySettings &settings)
{
    return QJsonObject{{QStringLiteral("audioAlgorithm"), settings.audio_algorithm},
                       {QStringLiteral("audioReactivity"), settings.audio_reactivity},
                       {QStringLiteral("midiReactivity"), settings.midi_reactivity}};
}

BackgroundImagePlacement placement_from_string(QString value)
{
    value = value.trimmed().toLower();
    if (value == QStringLiteral("stretched"))
    {
        return BackgroundImagePlacement::Stretched;
    }
    if (value == QStringLiteral("proportional-stretch") || value == QStringLiteral("proportional_stretch") ||
        value == QStringLiteral("centered"))
    {
        return value == QStringLiteral("centered") ? BackgroundImagePlacement::Center
                                                    : BackgroundImagePlacement::ProportionalStretch;
    }
    if (value == QStringLiteral("tiled"))
    {
        return BackgroundImagePlacement::Tiled;
    }

    return BackgroundImagePlacement::Center;
}

QJsonObject layer_to_json(const SceneLayer &layer)
{
    QJsonArray shaders;
    for (const auto &shader : layer.shaders)
    {
        shaders.push_back(QString::fromStdString(shader));
    }

    return QJsonObject{{QStringLiteral("enabled"), layer.enabled}, {QStringLiteral("shaders"), shaders}};
}

QStringList json_array_to_string_list(const QJsonValue &value)
{
    QStringList result;
    if (!value.isArray())
    {
        return result;
    }

    for (const auto &entry : value.toArray())
    {
        if (entry.isString())
        {
            result.push_back(entry.toString());
        }
    }
    return result;
}

QStringList collect_media_labels(const QList<QCameraDevice> &devices)
{
    QStringList labels;
    for (const auto &device : devices)
    {
        labels.push_back(device.description());
    }
    labels.removeDuplicates();
    labels.sort();
    return labels;
}

QStringList collect_audio_labels(const QList<QAudioDevice> &devices)
{
    QStringList labels;
    for (const auto &device : devices)
    {
        labels.push_back(device.description());
    }
    labels.removeDuplicates();
    labels.sort();
    return labels;
}

QStringList collect_midi_labels()
{
    QStringList labels;
    for (const auto &device : app::detect_midi_devices())
    {
        labels.push_back(QString::fromStdString(device));
    }
    labels.removeDuplicates();
    labels.sort();
    return labels;
}

QString filesystem_path_to_url_value(const std::filesystem::path &path)
{
    return QString::fromStdString(path.generic_string());
}

QStringList collect_relative_files(const std::filesystem::path &root, std::initializer_list<const char *> extensions)
{
    QStringList result;
    if (root.empty() || !std::filesystem::exists(root))
    {
        return result;
    }

    std::error_code error;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_regular_file())
        {
            continue;
        }

        const auto extension = entry.path().extension().string();
        const bool matches = std::any_of(extensions.begin(), extensions.end(), [&](const char *candidate) {
            return extension == candidate;
        });
        if (!matches)
        {
            continue;
        }

        std::error_code relative_error;
        const auto relative = std::filesystem::relative(entry.path(), root, relative_error);
        result.push_back(QString::fromStdString((relative_error ? entry.path().filename() : relative).generic_string()));
    }

    result.removeDuplicates();
    result.sort();
    return result;
}

std::optional<SceneControlServer::HttpRequest> parse_http_request(const QByteArray &buffer)
{
    const int header_end = buffer.indexOf("\r\n\r\n");
    if (header_end < 0)
    {
        return std::nullopt;
    }

    const QList<QByteArray> lines = buffer.left(header_end).split('\n');
    if (lines.isEmpty())
    {
        return std::nullopt;
    }

    const QList<QByteArray> request_line = lines.front().trimmed().split(' ');
    if (request_line.size() < 2)
    {
        return std::nullopt;
    }

    int content_length = 0;
    for (int index = 1; index < lines.size(); ++index)
    {
        const QByteArray line = lines[index].trimmed();
        if (line.startsWith("Content-Length:"))
        {
            content_length = line.mid(static_cast<int>(sizeof("Content-Length:")) - 1).trimmed().toInt();
        }
    }

    const int body_offset = header_end + 4;
    if (buffer.size() < body_offset + content_length)
    {
        return std::nullopt;
    }

    return SceneControlServer::HttpRequest{QString::fromUtf8(request_line[0]),
                                           QString::fromUtf8(request_line[1]),
                                           buffer.mid(body_offset, content_length)};
}

} // namespace

SceneControlServer::SceneControlServer(SceneDefinition *scene, ShaderVideoWindow *window, std::filesystem::path scene_file,
                                                                             std::filesystem::path resources_directory, std::filesystem::path shader_directory,
                                                                             SceneControlDeviceInfo device_info, bool active_scene_read_only, QObject *parent)
    : QObject{parent}, scene_{scene}, window_{window}, scene_file_{std::move(scene_file)},
            initial_scene_file_{scene_file_},
      resources_directory_{std::move(resources_directory)}, shader_directory_{std::move(shader_directory)},
    default_shader_directory_{shader_directory_},
                        device_info_{std::move(device_info)}, initial_scene_read_only_{active_scene_read_only}, server_{this}
{
}

bool SceneControlServer::start(const QHostAddress &address, quint16 port)
{
    QObject::connect(&server_, &QTcpServer::newConnection, this, [this]() { handle_new_connection(); });
    return server_.listen(address, port);
}

quint16 SceneControlServer::port() const
{
    return server_.serverPort();
}

void SceneControlServer::handle_new_connection()
{
    while (QTcpSocket *socket = server_.nextPendingConnection())
    {
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handle_socket_ready_read(socket); });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void SceneControlServer::handle_socket_ready_read(QTcpSocket *socket)
{
    if (socket == nullptr)
    {
        return;
    }

    QByteArray buffer = socket->property("requestBuffer").toByteArray();
    buffer += socket->readAll();
    socket->setProperty("requestBuffer", buffer);

    const auto request = parse_http_request(buffer);
    if (!request.has_value())
    {
        return;
    }

    handle_request(socket, *request);
}

void SceneControlServer::handle_request(QTcpSocket *socket, const HttpRequest &request)
{
    const QUrl url{request.path};
    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();

    if (request.method == QStringLiteral("GET") && path == QStringLiteral("/"))
    {
        send_response(socket, 200, QByteArray{"text/html; charset=utf-8"}, build_index_html());
        return;
    }

    if (request.method == QStringLiteral("GET") && path == QStringLiteral("/api/state"))
    {
        send_response(socket, 200, QByteArray{"application/json; charset=utf-8"},
                      QJsonDocument{build_state_object()}.toJson(QJsonDocument::Compact));
        return;
    }

    if (request.method == QStringLiteral("POST") && path == QStringLiteral("/api/apply"))
    {
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(request.body, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        {
            send_response(socket, 400, QByteArray{"application/json; charset=utf-8"},
                          QJsonDocument{QJsonObject{{QStringLiteral("ok"), false},
                                                    {QStringLiteral("error"), QStringLiteral("Invalid JSON payload")}}}
                              .toJson(QJsonDocument::Compact));
            return;
        }

        QString error_message;
        const bool applied = apply_update_from_json(document.object(), &error_message);
        send_response(socket, applied ? 200 : 400, QByteArray{"application/json; charset=utf-8"},
                      QJsonDocument{QJsonObject{{QStringLiteral("ok"), applied},
                                                {QStringLiteral("error"), error_message},
                                                {QStringLiteral("state"), build_state_object()}}}
                          .toJson(QJsonDocument::Compact));
        return;
    }

    send_response(socket, 404, QByteArray{"text/plain; charset=utf-8"}, QByteArray{"Not found"});
}

void SceneControlServer::send_response(QTcpSocket *socket, int status_code, QByteArray content_type, QByteArray body) const
{
    if (socket == nullptr)
    {
        return;
    }

    QByteArray status_text{"OK"};
    if (status_code == 400)
    {
        status_text = "Bad Request";
    }
    else if (status_code == 404)
    {
        status_text = "Not Found";
    }

    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;

    socket->write(response);
    socket->disconnectFromHost();
}

QByteArray SceneControlServer::build_index_html() const
{
    return QByteArrayLiteral(R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>cockscreen control</title>
  <style>
        :root {
            color-scheme: dark;
            --bg: #111;
            --panel: #181818;
            --panel-border: #333;
            --field: #222;
            --field-border: #444;
            --text: #eee;
            --muted: #aaa;
            --accent: #d86f3d;
            --accent-strong: #f08c56;
            --page-gap: 24px;
            --section-gap: 16px;
            --touch-height: 46px;
            --radius: 12px;
        }
        html { -webkit-text-size-adjust: 100%; }
        body { font-family: sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: env(safe-area-inset-top, 0) env(safe-area-inset-right, 0) env(safe-area-inset-bottom, 0) env(safe-area-inset-left, 0); }
        body, button, input, select, textarea { font-size: 16px; }
        h1, h2 { margin: 0 0 8px; }
        p { margin-top: 0; }
        section { border: 1px solid var(--panel-border); padding: 16px; margin-bottom: var(--section-gap); background: var(--panel); border-radius: var(--radius); }
    label { display: block; margin: 8px 0 4px; }
        select, input, button, textarea { width: 100%; box-sizing: border-box; background: var(--field); color: var(--text); border: 1px solid var(--field-border); padding: 10px 12px; border-radius: 10px; }
        input, select, button { min-height: var(--touch-height); }
        button { background: linear-gradient(180deg, var(--accent-strong), var(--accent)); border-color: var(--accent-strong); font-weight: 600; }
        button[type="button"] { background: var(--field); border-color: var(--field-border); }
    select[multiple] { min-height: 140px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
    .row { display: grid; grid-template-columns: 1fr 120px; gap: 12px; align-items: end; }
        .page { max-width: 1180px; margin: 0 auto; padding: var(--page-gap); }
        .page-header { margin-bottom: 18px; }
        .shader-editor { display: grid; grid-template-columns: minmax(0, 1fr) 112px minmax(0, 1fr); gap: 12px; align-items: start; }
        .shader-editor-controls { display: grid; gap: 8px; }
        .shader-editor-panel { min-width: 0; }
        .shader-editor select { min-height: 220px; }
        .shader-editor .muted { margin-top: 6px; }
        .layer-toggle { display: flex; align-items: center; gap: 10px; min-height: var(--touch-height); margin-bottom: 8px; }
        .layer-toggle input { width: 22px; height: 22px; min-height: 22px; flex: 0 0 auto; }
        .status-grid strong { display: block; margin-bottom: 4px; }
        .muted { color: var(--muted); font-size: 0.92rem; }
        pre { white-space: pre-wrap; background: #0f0f0f; padding: 12px; border: 1px solid var(--panel-border); border-radius: 10px; overflow-x: auto; }
        ul { margin: 8px 0 0 18px; padding: 0; }
        @media (max-width: 900px) {
            :root { --page-gap: 16px; }
            .row { grid-template-columns: 1fr; }
            .shader-editor { grid-template-columns: 1fr; }
            .shader-editor-controls { grid-template-columns: repeat(2, minmax(0, 1fr)); }
            .shader-editor select { min-height: 180px; }
        }
        @media (max-width: 640px) {
            :root { --page-gap: 12px; --section-gap: 12px; }
            .page { padding: var(--page-gap); }
            section { padding: 14px; }
            .grid { grid-template-columns: 1fr; gap: 12px; }
            .shader-editor-controls { grid-template-columns: 1fr 1fr; }
            .shader-editor select { min-height: 156px; }
            .muted { font-size: 0.95rem; }
        }
  </style>
</head>
<body>
    <div class="page">
        <div class="page-header">
            <h1>cockscreen control</h1>
            <p class="muted">Live updates for shader layers, playback transport, and background settings. Device lists are exposed here, but device reopening is read-only in this first version.</p>
        </div>
        <div id="app">Loading…</div>
    </div>
  <script>
    function escapeHtml(value) {
      return String(value ?? '').replace(/[&<>\"]/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[ch]));
    }
    function buildOptions(values, selected) {
      const set = new Set(selected || []);
      return values.map(value => `<option value="${escapeHtml(value)}" ${set.has(value) ? 'selected' : ''}>${escapeHtml(value)}</option>`).join('');
    }
        function buildPresetOptions(groups, selected) {
            return (groups || []).map(group => {
                const scenes = Array.isArray(group.scenes) ? group.scenes : [];
                const options = buildLabeledOptions(scenes.map(scene => ({ value: scene.path, label: scene.label })), selected);
                return `<optgroup label="${escapeHtml(group.label || 'root')}">${options}</optgroup>`;
            }).join('');
        }
        function buildLabeledOptions(options, selected) {
            const selectedValue = String(selected ?? '');
            return options.map(option => {
                const value = String(option.value ?? '');
                const label = String(option.label ?? value);
                return `<option value="${escapeHtml(value)}" ${value === selectedValue ? 'selected' : ''}>${escapeHtml(label)}</option>`;
            }).join('');
        }
    function deviceList(values) {
      return values.map(value => `<li>${escapeHtml(value)}</li>`).join('');
    }
        function numericValue(id, fallback) {
            const value = document.getElementById(id).value.trim();
            if (value === '') {
                return fallback;
            }

            const parsed = Number(value);
            return Number.isFinite(parsed) ? parsed : fallback;
        }
        function nullableNumericValue(id) {
            const value = document.getElementById(id).value.trim();
            if (value === '') {
                return null;
            }

            const parsed = Number(value);
            return Number.isFinite(parsed) ? parsed : null;
        }
        function orderedValues(id) {
            return Array.from(document.getElementById(id).options).map(option => option.value);
        }
        function buildShaderEditor(prefix, layer, availableShaders) {
            const selected = Array.isArray(layer.shaders) ? layer.shaders : [];
            const selectedSet = new Set(selected);
            const available = availableShaders.filter(shader => !selectedSet.has(shader));
            return `
                <div>
                    <label class="layer-toggle"><input id="${prefix}Enabled" type="checkbox" ${layer.enabled ? 'checked' : ''}> <span>${escapeHtml(prefix.charAt(0).toUpperCase() + prefix.slice(1))} layer enabled</span></label>
                    <div class="shader-editor">
                        <div class="shader-editor-panel">
                            <label for="${prefix}ShaderPool">Available shaders</label>
                            <select id="${prefix}ShaderPool" multiple>${buildOptions(available, [])}</select>
                            <div class="muted">Tap one or more shaders, then use Add or Remove. Move up and Move down change the render order.</div>
                        </div>
                        <div class="shader-editor-controls">
                            <button type="button" data-action="add" data-layer="${prefix}">Add →</button>
                            <button type="button" data-action="remove" data-layer="${prefix}">← Remove</button>
                            <button type="button" data-action="up" data-layer="${prefix}">Move up</button>
                            <button type="button" data-action="down" data-layer="${prefix}">Move down</button>
                        </div>
                        <div class="shader-editor-panel">
                            <label for="${prefix}Shaders">Active shader chain</label>
                            <select id="${prefix}Shaders" multiple>${buildOptions(selected, [])}</select>
                            <div class="muted">Top to bottom matches render order.</div>
                        </div>
                    </div>
                </div>`;
        }
        function moveSelectedOptions(sourceId, targetId) {
            const source = document.getElementById(sourceId);
            const target = document.getElementById(targetId);
            const selected = Array.from(source.selectedOptions);
            for (const option of selected) {
                target.appendChild(option);
            }
        }
        function moveSelectedUp(selectId) {
            const select = document.getElementById(selectId);
            const options = Array.from(select.options);
            for (let index = 1; index < options.length; index += 1) {
                const option = options[index];
                if (option.selected && !options[index - 1].selected) {
                    select.insertBefore(option, options[index - 1]);
                    options[index - 1] = option;
                    options[index] = select.options[index];
                }
            }
        }
        function moveSelectedDown(selectId) {
            const select = document.getElementById(selectId);
            const options = Array.from(select.options);
            for (let index = options.length - 2; index >= 0; index -= 1) {
                const option = options[index];
                if (option.selected && !options[index + 1].selected) {
                    const nextSibling = options[index + 1].nextSibling;
                    select.insertBefore(option, nextSibling);
                    options[index + 1] = option;
                    options[index] = select.options[index];
                }
            }
        }
        function wireShaderEditorButtons() {
            for (const button of document.querySelectorAll('[data-action][data-layer]')) {
                button.onclick = () => {
                    const layer = button.dataset.layer;
                    const action = button.dataset.action;
                    if (action === 'add') {
                        moveSelectedOptions(`${layer}ShaderPool`, `${layer}Shaders`);
                    } else if (action === 'remove') {
                        moveSelectedOptions(`${layer}Shaders`, `${layer}ShaderPool`);
                    } else if (action === 'up') {
                        moveSelectedUp(`${layer}Shaders`);
                    } else if (action === 'down') {
                        moveSelectedDown(`${layer}Shaders`);
                    }
                };
            }
        }
    async function refreshState() {
    const state = await fetch('/api/state').then(response => response.json());
                const presetManager = state.presetManager || { groups: [], activeScenePath: '', activeSceneReadOnly: false, saveMode: 'new-preset-only' };
            const pinkKeyAudioAlgorithms = [
                { value: '0', label: '0: Bass focus' },
                { value: '1', label: '1: Low-mid focus' },
                { value: '2', label: '2: High-mid focus' },
                { value: '3', label: '3: High focus' },
                { value: '4', label: '4: Spectral centroid' },
                { value: '5', label: '5: Full-spectrum average' }
            ];
      const app = document.getElementById('app');
      app.innerHTML = `
        <section>
          <h2>Status</h2>
                    <div class="grid status-grid">
            <div>
              <strong>Scene file</strong>
              <div>${escapeHtml(state.sceneFile)}</div>
            </div>
            <div>
              <strong>Opened video</strong>
              <div>${escapeHtml(state.openedDevices.video)}</div>
            </div>
            <div>
              <strong>Opened audio</strong>
              <div>${escapeHtml(state.openedDevices.audio)}</div>
            </div>
            <div>
              <strong>Opened MIDI</strong>
              <div>${escapeHtml(state.openedDevices.midi)}</div>
            </div>
          </div>
          <label>Window status</label>
          <pre>${escapeHtml(state.status.windowStatus || '<none>')}</pre>
          <label>Fatal render error</label>
          <pre>${escapeHtml(state.status.fatalRenderError || '<none>')}</pre>
        </section>
                <section>
                    <h2>Presets</h2>
                    <label for="presetScenePath">Scene preset</label>
                    <select id="presetScenePath">${buildPresetOptions(presetManager.groups, presetManager.activeScenePath)}</select>
                    <div class="row">
                        <input type="text" value="${escapeHtml(presetManager.root || '')}" readonly>
                        <button id="loadPresetButton" type="button">Load preset</button>
                    </div>
                    <p class="muted">Directories map 1:1 to preset groups and scene files map 1:1 to selectable scenes. Loading a preset updates the visual scene immediately. ${presetManager.activeSceneReadOnly ? 'The initial startup scene is read-only, so future save flows must create a new preset instead of rewriting that source file.' : 'The current active preset is not the initial read-only startup scene.'} Device reopening is still read-only.</p>
                </section>
        <section>
          <h2>Background</h2>
          <div class="grid">
            <div>
              <label for="bgColor">Color</label>
              <input id="bgColor" type="color" value="${escapeHtml(state.backgroundColor.hex)}">
            </div>
            <div>
              <label for="bgAlpha">Alpha</label>
              <input id="bgAlpha" type="number" min="0" max="1" step="0.01" value="${escapeHtml(state.backgroundColor.a)}">
            </div>
            <div>
              <label for="bgPlacement">Image placement</label>
              <select id="bgPlacement">
                ${buildOptions(['center', 'stretched', 'proportional-stretch', 'tiled'], [state.backgroundImage.placement])}
              </select>
            </div>
          </div>
          <label for="bgImage">Background image</label>
          <select id="bgImage">
            <option value="">&lt;none&gt;</option>
            ${buildOptions(state.availableBackgroundFiles, [state.backgroundImage.file])}
          </select>
          <label for="bgImageCustom">Custom background path override</label>
          <input id="bgImageCustom" type="text" value="${escapeHtml(state.backgroundImage.file)}">
                    <label class="layer-toggle"><input id="timecodeEnabled" type="checkbox" ${state.timecode ? 'checked' : ''}> <span>Film timecode overlay</span></label>
        </section>
        <section>
                    <h2>Playback</h2>
                    <div class="grid">
                        <div>
                            <label>Playback file</label>
                            <input type="text" value="${escapeHtml(state.playbackInput.file || '<none>')}" readonly>
                        </div>
                        <div>
                            <label for="playbackStartMs">Start ms</label>
                            <input id="playbackStartMs" type="number" min="0" step="1" value="${escapeHtml(state.playbackInput.startMs ?? 0)}">
                        </div>
                        <div>
                            <label for="playbackLoopStartMs">Loop start ms</label>
                            <input id="playbackLoopStartMs" type="number" min="0" step="1" value="${escapeHtml(state.playbackInput.loopStartMs ?? 0)}">
                        </div>
                        <div>
                            <label for="playbackLoopEndMs">Loop end ms</label>
                            <input id="playbackLoopEndMs" type="number" min="0" step="1" value="${state.playbackInput.loopEndMs == null ? '' : escapeHtml(state.playbackInput.loopEndMs)}">
                        </div>
                        <div>
                            <label for="playbackLoopRepeat">Loop repeat</label>
                            <input id="playbackLoopRepeat" type="number" min="0" step="1" value="${escapeHtml(state.playbackInput.loopRepeat ?? 0)}">
                        </div>
                        <div>
                            <label for="playbackRate">Playback rate</label>
                            <input id="playbackRate" type="number" min="0.01" step="0.01" value="${escapeHtml(state.playbackInput.playbackRate ?? 1)}">
                        </div>
                        <div>
                            <label for="playbackRateLooping">Playback rate looping</label>
                            <input id="playbackRateLooping" type="number" min="0.01" step="0.01" value="${escapeHtml(state.playbackInput.playbackRateLooping ?? 1)}">
                        </div>
                    </div>
                    <p class="muted">Leave loop end empty to disable custom looping. Loop repeat 0 means infinite loops.</p>
                </section>
                <section>
                    <h2>Pink Key</h2>
                    <div class="grid">
                        <div>
                            <label for="pinkKeyAudioAlgorithm">Audio detector</label>
                            <select id="pinkKeyAudioAlgorithm">
                                ${buildLabeledOptions(pinkKeyAudioAlgorithms, Math.max(0, Math.min(5, Math.round(state.pinkKey.audioAlgorithm ?? 0))))}
                            </select>
                        </div>
                        <div>
                            <label for="pinkKeyAudioReactivity">Audio reactivity</label>
                            <input id="pinkKeyAudioReactivity" type="number" min="0" max="1.5" step="0.01" value="${escapeHtml(state.pinkKey.audioReactivity ?? 0.45)}">
                        </div>
                        <div>
                            <label for="pinkKeyMidiReactivity">MIDI reactivity</label>
                            <input id="pinkKeyMidiReactivity" type="number" min="0" max="1.5" step="0.01" value="${escapeHtml(state.pinkKey.midiReactivity ?? 0.35)}">
                        </div>
                    </div>
                    <p class="muted">Detector modes: 0 bass, 1 low-mid, 2 high-mid, 3 high, 4 centroid, 5 full-spectrum average.</p>
                </section>
                <section>
          <h2>Shaders</h2>
          <div class="grid">
                        ${buildShaderEditor('video', state.layers.video, state.availableShaders)}
                        ${buildShaderEditor('playback', state.layers.playback, state.availableShaders)}
                        ${buildShaderEditor('screen', state.layers.screen, state.availableShaders)}
          </div>
        </section>
        <section>
          <h2>Devices</h2>
          <div class="grid">
            <div><strong>Available video devices</strong><ul>${deviceList(state.availableDevices.video)}</ul></div>
            <div><strong>Available audio devices</strong><ul>${deviceList(state.availableDevices.audio)}</ul></div>
            <div><strong>Available MIDI devices</strong><ul>${deviceList(state.availableDevices.midi)}</ul></div>
          </div>
        </section>
        <section>
          <div class="row">
            <button id="applyButton">Apply live changes</button>
            <button id="refreshButton" type="button">Refresh</button>
          </div>
          <p class="muted" id="message"></p>
        </section>`;

      document.getElementById('applyButton').onclick = applyChanges;
      document.getElementById('refreshButton').onclick = refreshState;
                        document.getElementById('loadPresetButton').onclick = loadPreset;
            wireShaderEditorButtons();
    }
        async function loadPreset() {
            const presetPath = document.getElementById('presetScenePath')?.value || '';
            if (!presetPath) {
                document.getElementById('message').textContent = 'No preset selected.';
                return;
            }

            const response = await fetch('/api/apply', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ presetPath })
            });
            const result = await response.json();
            document.getElementById('message').textContent = result.ok ? 'Preset loaded.' : `Preset load failed: ${result.error}`;
            await refreshState();
        }
    async function applyChanges() {
      const color = document.getElementById('bgColor').value;
      const payload = {
        backgroundColor: {
          r: parseInt(color.slice(1, 3), 16) / 255.0,
          g: parseInt(color.slice(3, 5), 16) / 255.0,
          b: parseInt(color.slice(5, 7), 16) / 255.0,
          a: parseFloat(document.getElementById('bgAlpha').value || '1')
        },
        backgroundImage: {
          file: document.getElementById('bgImageCustom').value || document.getElementById('bgImage').value,
          placement: document.getElementById('bgPlacement').value
        },
                timecode: document.getElementById('timecodeEnabled').checked,
                pinkKey: {
                    audioAlgorithm: Math.max(0, Math.min(5, Math.floor(numericValue('pinkKeyAudioAlgorithm', 0)))),
                    audioReactivity: Math.max(0, Math.min(1.5, numericValue('pinkKeyAudioReactivity', 0.45))),
                    midiReactivity: Math.max(0, Math.min(1.5, numericValue('pinkKeyMidiReactivity', 0.35)))
                },
                playbackInput: {
                    startMs: Math.max(0, Math.floor(numericValue('playbackStartMs', 0))),
                    loopStartMs: Math.max(0, Math.floor(numericValue('playbackLoopStartMs', 0))),
                    loopEndMs: nullableNumericValue('playbackLoopEndMs'),
                    loopRepeat: Math.max(0, Math.floor(numericValue('playbackLoopRepeat', 0))),
                    playbackRate: Math.max(0.01, numericValue('playbackRate', 1.0)),
                    playbackRateLooping: Math.max(0.01, numericValue('playbackRateLooping', 1.0))
                },
        layers: {
                    video: { enabled: document.getElementById('videoEnabled').checked, shaders: orderedValues('videoShaders') },
                    playback: { enabled: document.getElementById('playbackEnabled').checked, shaders: orderedValues('playbackShaders') },
                    screen: { enabled: document.getElementById('screenEnabled').checked, shaders: orderedValues('screenShaders') }
        }
      };
      const response = await fetch('/api/apply', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
      const result = await response.json();
      document.getElementById('message').textContent = result.ok ? 'Applied.' : `Apply failed: ${result.error}`;
      await refreshState();
    }
    refreshState();
  </script>
</body>
</html>
)HTML");
}

QJsonObject SceneControlServer::build_state_object() const
{
    QJsonObject object;
    if (scene_ == nullptr || window_ == nullptr)
    {
        return object;
    }

    const QColor background_color = QColor::fromRgbF(scene_->background_color.red, scene_->background_color.green,
                                                     scene_->background_color.blue, scene_->background_color.alpha);
    bool active_scene_read_only = false;
    if (initial_scene_read_only_)
    {
        std::error_code initial_error;
        std::error_code current_error;
        const auto canonical_initial = std::filesystem::weakly_canonical(initial_scene_file_, initial_error);
        const auto canonical_current = std::filesystem::weakly_canonical(scene_file_, current_error);
        active_scene_read_only = !initial_error && !current_error && canonical_initial == canonical_current;
    }
    object.insert(QStringLiteral("sceneFile"), filesystem_path_to_url_value(scene_file_));
    object.insert(QStringLiteral("presetManager"),
                  preset_groups_to_json(preset_root_directory(scene_file_), scene_file_, active_scene_read_only));
    object.insert(QStringLiteral("backgroundColor"),
                  QJsonObject{{QStringLiteral("r"), scene_->background_color.red},
                              {QStringLiteral("g"), scene_->background_color.green},
                              {QStringLiteral("b"), scene_->background_color.blue},
                              {QStringLiteral("a"), scene_->background_color.alpha},
                              {QStringLiteral("hex"), background_color.name(QColor::HexRgb)}});
    object.insert(QStringLiteral("backgroundImage"),
                  QJsonObject{{QStringLiteral("file"), QString::fromStdString(scene_->background_image.file)},
                              {QStringLiteral("placement"), placement_to_string(scene_->background_image.placement)}});
    object.insert(QStringLiteral("timecode"), scene_->timecode);
    object.insert(QStringLiteral("pinkKey"), pink_key_to_json(scene_->pink_key));
    object.insert(QStringLiteral("layers"),
                  QJsonObject{{QStringLiteral("video"), layer_to_json(scene_->video_layer)},
                              {QStringLiteral("playback"), layer_to_json(scene_->playback_layer)},
                              {QStringLiteral("screen"), layer_to_json(scene_->screen_layer)}});
    object.insert(QStringLiteral("layerOrder"), layer_order_to_json(scene_->layer_order));
    object.insert(QStringLiteral("playbackInput"), playback_input_to_json(scene_->playback_input));
    object.insert(QStringLiteral("openedDevices"),
                  QJsonObject{{QStringLiteral("video"), device_info_.opened_video},
                              {QStringLiteral("audio"), device_info_.opened_audio},
                              {QStringLiteral("midi"), device_info_.opened_midi}});
    object.insert(QStringLiteral("availableDevices"),
                  QJsonObject{{QStringLiteral("video"), QJsonArray::fromStringList(collect_media_labels(QMediaDevices::videoInputs()))},
                              {QStringLiteral("audio"), QJsonArray::fromStringList(collect_audio_labels(QMediaDevices::audioInputs()))},
                            {QStringLiteral("midi"), QJsonArray::fromStringList(collect_midi_labels())}});
    object.insert(QStringLiteral("availableShaders"), QJsonArray::fromStringList(available_shader_files()));
    object.insert(QStringLiteral("availableBackgroundFiles"), QJsonArray::fromStringList(available_background_files()));
    object.insert(QStringLiteral("status"),
                  QJsonObject{{QStringLiteral("windowStatus"), window_->status_message()},
                              {QStringLiteral("fatalRenderError"), window_->fatal_render_error()}});
    return object;
}

bool SceneControlServer::apply_update_from_json(const QJsonObject &payload, QString *error_message)
{
    if (scene_ == nullptr || window_ == nullptr)
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Scene control server is not bound to a live window");
        }
        return false;
    }

    SceneDefinition updated = *scene_;
    auto updated_scene_file = scene_file_;
    auto updated_resources_directory = resources_directory_;
    auto updated_shader_directory = shader_directory_;

    if (const auto preset_path = payload.value(QStringLiteral("presetPath")); preset_path.isString())
    {
        const auto requested_preset = preset_path.toString().trimmed();
        if (!requested_preset.isEmpty())
        {
            const auto preset_root = preset_root_directory(scene_file_);
            if (preset_root.empty())
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Preset root directory is not available");
                }
                return false;
            }

            const auto requested_scene_file = preset_root / requested_preset.toStdString();
            std::error_code preset_error;
            const auto canonical_root = std::filesystem::weakly_canonical(preset_root, preset_error);
            if (preset_error)
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Could not resolve preset root directory");
                }
                return false;
            }

            const auto canonical_scene_file = std::filesystem::weakly_canonical(requested_scene_file, preset_error);
            if (preset_error || !std::filesystem::exists(canonical_scene_file))
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Preset not found: %1").arg(requested_preset);
                }
                return false;
            }

            const auto [root_end, candidate_mismatch] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                                                      canonical_scene_file.begin(), canonical_scene_file.end());
            if (root_end != canonical_root.end())
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Preset is outside the preset root: %1").arg(requested_preset);
                }
                return false;
            }

            Q_UNUSED(candidate_mismatch);

            std::string load_error;
            const auto loaded_scene = load_scene_definition(canonical_scene_file, &load_error);
            if (!loaded_scene.has_value())
            {
                if (error_message != nullptr)
                {
                    *error_message = QString::fromStdString(load_error);
                }
                return false;
            }

            updated = *loaded_scene;
            updated_scene_file = updated.source_path;
            updated_resources_directory = updated.resources_directory;
            updated_shader_directory = !updated.shader_directory.empty() ? std::filesystem::path{updated.shader_directory}
                                                                        : default_shader_directory_;

            if (!validate_scene_shader_files(updated, updated_shader_directory, error_message))
            {
                return false;
            }
        }
    }

    if (const auto background_color = payload.value(QStringLiteral("backgroundColor")); background_color.isObject())
    {
        const auto object = background_color.toObject();
        updated.background_color.red = std::clamp(static_cast<float>(object.value(QStringLiteral("r")).toDouble(updated.background_color.red)), 0.0F, 1.0F);
        updated.background_color.green = std::clamp(static_cast<float>(object.value(QStringLiteral("g")).toDouble(updated.background_color.green)), 0.0F, 1.0F);
        updated.background_color.blue = std::clamp(static_cast<float>(object.value(QStringLiteral("b")).toDouble(updated.background_color.blue)), 0.0F, 1.0F);
        updated.background_color.alpha = std::clamp(static_cast<float>(object.value(QStringLiteral("a")).toDouble(updated.background_color.alpha)), 0.0F, 1.0F);
    }

    if (const auto background_image = payload.value(QStringLiteral("backgroundImage")); background_image.isObject())
    {
        const auto object = background_image.toObject();
        updated.background_image.file = object.value(QStringLiteral("file")).toString(QString::fromStdString(updated.background_image.file)).toStdString();
        updated.background_image.placement = placement_from_string(object.value(QStringLiteral("placement")).toString(placement_to_string(updated.background_image.placement)));
        if (!updated.background_image.file.empty())
        {
            std::filesystem::path file_path{updated.background_image.file};
            if (!file_path.is_absolute())
            {
                file_path = updated_resources_directory / file_path;
            }
            if (!std::filesystem::exists(file_path))
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Background file not found: %1").arg(QString::fromStdString(updated.background_image.file));
                }
                return false;
            }
        }
    }

    if (const auto timecode = payload.value(QStringLiteral("timecode")); timecode.isBool())
    {
        updated.timecode = timecode.toBool(updated.timecode);
    }

    if (const auto pink_key = payload.value(QStringLiteral("pinkKey")); pink_key.isObject())
    {
        const auto object = pink_key.toObject();
        if (const auto audio_algorithm = object.value(QStringLiteral("audioAlgorithm")); audio_algorithm.isDouble())
        {
            updated.pink_key.audio_algorithm = std::clamp(static_cast<float>(audio_algorithm.toDouble(updated.pink_key.audio_algorithm)), 0.0F, 5.0F);
        }
        if (const auto audio_reactivity = object.value(QStringLiteral("audioReactivity")); audio_reactivity.isDouble())
        {
            updated.pink_key.audio_reactivity = std::clamp(static_cast<float>(audio_reactivity.toDouble(updated.pink_key.audio_reactivity)), 0.0F, 1.5F);
        }
        if (const auto midi_reactivity = object.value(QStringLiteral("midiReactivity")); midi_reactivity.isDouble())
        {
            updated.pink_key.midi_reactivity = std::clamp(static_cast<float>(midi_reactivity.toDouble(updated.pink_key.midi_reactivity)), 0.0F, 1.5F);
        }
    }

    if (const auto playback_input = payload.value(QStringLiteral("playbackInput")); playback_input.isObject())
    {
        const auto object = playback_input.toObject();
        if (const auto start_ms = object.value(QStringLiteral("startMs")); start_ms.isDouble())
        {
            updated.playback_input.start_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(start_ms.toDouble()));
        }
        if (const auto loop_start_ms = object.value(QStringLiteral("loopStartMs")); loop_start_ms.isDouble())
        {
            updated.playback_input.loop_start_ms =
                std::max<std::int64_t>(0, static_cast<std::int64_t>(loop_start_ms.toDouble()));
        }
        if (const auto loop_end_ms = object.value(QStringLiteral("loopEndMs")); loop_end_ms.isNull())
        {
            updated.playback_input.loop_end_ms.reset();
        }
        else if (loop_end_ms.isDouble())
        {
            updated.playback_input.loop_end_ms =
                std::max<std::int64_t>(0, static_cast<std::int64_t>(loop_end_ms.toDouble()));
        }
        if (const auto loop_repeat = object.value(QStringLiteral("loopRepeat")); loop_repeat.isDouble())
        {
            updated.playback_input.loop_repeat = std::max(0, loop_repeat.toInt(updated.playback_input.loop_repeat));
        }
        if (const auto playback_rate = object.value(QStringLiteral("playbackRate")); playback_rate.isDouble())
        {
            updated.playback_input.playback_rate =
                std::max(0.01F, static_cast<float>(playback_rate.toDouble(updated.playback_input.playback_rate)));
        }
        if (const auto playback_rate_looping = object.value(QStringLiteral("playbackRateLooping"));
            playback_rate_looping.isDouble())
        {
            updated.playback_input.playback_rate_looping = std::max(
                0.01F, static_cast<float>(playback_rate_looping.toDouble(updated.playback_input.playback_rate_looping)));
        }

        if (updated.playback_input.loop_end_ms.has_value() &&
            *updated.playback_input.loop_end_ms <= updated.playback_input.loop_start_ms)
        {
            if (error_message != nullptr)
            {
                *error_message = QStringLiteral("loopEndMs must be greater than loopStartMs");
            }
            return false;
        }
    }

    if (const auto layers = payload.value(QStringLiteral("layers")); layers.isObject())
    {
        const auto apply_layer = [&](const QString &name, SceneLayer *layer) -> bool {
            if (layer == nullptr)
            {
                return true;
            }
            const auto value = layers.toObject().value(name);
            if (!value.isObject())
            {
                return true;
            }

            const auto object = value.toObject();
            layer->enabled = object.value(QStringLiteral("enabled")).toBool(layer->enabled);
            const auto requested_shaders = json_array_to_string_list(object.value(QStringLiteral("shaders")));
            std::vector<std::string> shaders;
            for (const auto &shader : requested_shaders)
            {
                if (shader.isEmpty())
                {
                    continue;
                }
                std::filesystem::path shader_path{shader.toStdString()};
                if (!shader_path.is_absolute())
                {
                    shader_path = updated_shader_directory / shader_path;
                }
                if (!std::filesystem::exists(shader_path))
                {
                    if (error_message != nullptr)
                    {
                        *error_message = QStringLiteral("Shader file not found: %1").arg(shader);
                    }
                    return false;
                }
                shaders.push_back(shader.toStdString());
            }
            layer->shaders = std::move(shaders);
            return true;
        };

        if (!apply_layer(QStringLiteral("video"), &updated.video_layer) ||
            !apply_layer(QStringLiteral("playback"), &updated.playback_layer) ||
            !apply_layer(QStringLiteral("screen"), &updated.screen_layer))
        {
            return false;
        }
    }

    if (const auto layer_order = payload.value(QStringLiteral("layerOrder")); layer_order.isArray())
    {
        std::vector<std::string> parsed_layer_order;
        parsed_layer_order.reserve(3);
        for (const auto &entry : layer_order.toArray())
        {
            if (!entry.isString())
            {
                continue;
            }

            const QString normalized = entry.toString().trimmed().toLower();
            if (normalized != QStringLiteral("video") && normalized != QStringLiteral("playback") &&
                normalized != QStringLiteral("screen"))
            {
                continue;
            }

            const std::string layer_name = normalized.toStdString();
            if (std::find(parsed_layer_order.begin(), parsed_layer_order.end(), layer_name) == parsed_layer_order.end())
            {
                parsed_layer_order.push_back(layer_name);
            }
        }

        if (!parsed_layer_order.empty() && parsed_layer_order.size() != 3)
        {
            if (error_message != nullptr)
            {
                *error_message = QStringLiteral("layerOrder must contain video, playback, and screen exactly once");
            }
            return false;
        }

        updated.layer_order = std::move(parsed_layer_order);
    }

    *scene_ = updated;
    scene_file_ = std::move(updated_scene_file);
    resources_directory_ = std::move(updated_resources_directory);
    shader_directory_ = std::move(updated_shader_directory);
    window_->apply_scene_update(updated);
    if (error_message != nullptr)
    {
        *error_message = window_->fatal_render_error();
    }
    return true;
}

QStringList SceneControlServer::available_shader_files() const
{
    return collect_relative_files(shader_directory_, {".glsl", ".frag"});
}

QStringList SceneControlServer::available_background_files() const
{
    return collect_relative_files(resources_directory_, {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".ppm"});
}

} // namespace cockscreen::runtime