#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QAudioDevice>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QString>

#include "Application.hpp"

namespace cockscreen::runtime
{

bool is_pi_target();
bool has_direct_drm_access();
std::optional<std::string> read_text_file(const std::filesystem::path &path);
bool looks_like_touch_input(const std::string &name);
std::optional<std::string> find_usb_touchscreen();
bool print_startup_preflight();
bool validate_render_path(const ApplicationSettings &settings);

struct SystemMetricsSnapshot
{
		double cpu_percent{0.0};
		double memory_percent{0.0};
		double memory_used_mb{0.0};
		double memory_total_mb{0.0};
		bool available{false};
};

class SystemMetricsSampler final
{
	public:
		SystemMetricsSampler() = default;

		[[nodiscard]] SystemMetricsSnapshot sample();

	private:
		bool have_previous_sample_{false};
		double previous_cpu_seconds_{0.0};
		std::chrono::steady_clock::time_point previous_sample_time_{};
};

std::optional<std::pair<int, int>> parse_capture_mode_dimensions(std::string_view mode);
bool is_default_output_monitor_token(std::string_view device);
std::optional<QString> default_output_monitor_source_name();
std::optional<QAudioDevice> select_audio_input(const ApplicationSettings &settings, QString *selected_label = nullptr);
std::optional<QCameraDevice> select_video_input(const ApplicationSettings &settings, QString *selected_label);
std::optional<QCameraFormat> select_camera_format(const QCameraDevice &device, int requested_width, int requested_height);
QString camera_format_label(const QCameraFormat &format);
QString shader_label_for(const ApplicationSettings &settings, std::string_view shader_file);
QString shader_label_for(const ApplicationSettings &settings);

} // namespace cockscreen::runtime