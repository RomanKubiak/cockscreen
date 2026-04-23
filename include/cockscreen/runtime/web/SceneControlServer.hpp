#pragma once

#include "cockscreen/runtime/Scene.hpp"

#include <filesystem>

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QHostAddress>
#include <QTcpServer>

class QTcpSocket;

namespace cockscreen::runtime
{

class ShaderVideoWindow;

struct SceneControlDeviceInfo
{
    QString opened_video;
    QString opened_audio;
    QString opened_midi;
};

class SceneControlServer final : public QObject
{
  public:
    struct HttpRequest
    {
        QString method;
        QString path;
        QByteArray body;
    };

    SceneControlServer(SceneDefinition *scene, ShaderVideoWindow *window, std::filesystem::path scene_file,
                       std::filesystem::path resources_directory, std::filesystem::path shader_directory,
                       SceneControlDeviceInfo device_info, bool active_scene_read_only = false,
                       QObject *parent = nullptr);

    bool start(const QHostAddress &address, quint16 port);
    [[nodiscard]] quint16 port() const;

  private:
    void handle_new_connection();
    void handle_socket_ready_read(QTcpSocket *socket);
    void handle_request(QTcpSocket *socket, const HttpRequest &request);
    void send_response(QTcpSocket *socket, int status_code, QByteArray content_type, QByteArray body) const;

    [[nodiscard]] QByteArray build_index_html() const;
    [[nodiscard]] QJsonObject build_state_object() const;
    bool apply_update_from_json(const QJsonObject &payload, QString *error_message);
    [[nodiscard]] QStringList available_shader_files() const;
    [[nodiscard]] QStringList available_background_files() const;

    SceneDefinition *scene_{nullptr};
    ShaderVideoWindow *window_{nullptr};
    std::filesystem::path scene_file_;
    std::filesystem::path initial_scene_file_;
    std::filesystem::path resources_directory_;
    std::filesystem::path shader_directory_;
    std::filesystem::path default_shader_directory_;
    SceneControlDeviceInfo device_info_;
    bool initial_scene_read_only_{false};
    QTcpServer server_;
};

} // namespace cockscreen::runtime