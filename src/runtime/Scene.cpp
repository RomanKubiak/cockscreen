#include "cockscreen/runtime/Scene.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/scene/Parse.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>

namespace cockscreen::runtime
{

std::optional<SceneDefinition> load_scene_definition(const std::filesystem::path &path, std::string *error_message)
{
    const auto text = read_text_file(path);
    if (!text.has_value())
    {
        if (error_message != nullptr)
        {
            *error_message = "Unable to read scene file: " + path.string();
        }
        return std::nullopt;
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(*text), &parse_error);
    if (parse_error.error != QJsonParseError::NoError)
    {
        if (error_message != nullptr)
        {
            *error_message = "Scene JSON parse error in " + path.string() + ": " +
                             parse_error.errorString().toStdString();
        }
        return std::nullopt;
    }

    if (!document.isObject())
    {
        if (error_message != nullptr)
        {
            *error_message = "Scene file is not a JSON object: " + path.string();
        }
        return std::nullopt;
    }

    return scene_parser::parse_scene_definition(document.object(), path);
}

} // namespace cockscreen::runtime