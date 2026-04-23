#include "cockscreen/runtime/Scene.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/scene/Parse.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>

#include <string_view>

namespace cockscreen::runtime
{

namespace
{

std::string strip_jsonc_comments(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    bool in_string = false;
    bool escaping = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const char current = input[index];
        const char next = index + 1 < input.size() ? input[index + 1] : '\0';

        if (in_line_comment)
        {
            if (current == '\n')
            {
                in_line_comment = false;
                output.push_back(current);
            }
            continue;
        }

        if (in_block_comment)
        {
            if (current == '*' && next == '/')
            {
                in_block_comment = false;
                ++index;
            }
            else if (current == '\n')
            {
                output.push_back('\n');
            }
            continue;
        }

        if (!in_string && current == '/' && next == '/')
        {
            in_line_comment = true;
            ++index;
            continue;
        }

        if (!in_string && current == '/' && next == '*')
        {
            in_block_comment = true;
            ++index;
            continue;
        }

        output.push_back(current);

        if (!in_string)
        {
            if (current == '"')
            {
                in_string = true;
            }
            continue;
        }

        if (escaping)
        {
            escaping = false;
            continue;
        }

        if (current == '\\')
        {
            escaping = true;
        }
        else if (current == '"')
        {
            in_string = false;
        }
    }

    return output;
}

} // namespace

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
    const auto jsonc_text = strip_jsonc_comments(*text);
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(jsonc_text), &parse_error);
    if (parse_error.error != QJsonParseError::NoError)
    {
        if (error_message != nullptr)
        {
            *error_message = "Scene JSON/JSONC parse error in " + path.string() + ": " +
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