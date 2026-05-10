#pragma once

#include <string>

namespace cockscreen::runtime::pi
{

enum class DisplayOutput
{
    Hdmi,
    CompositePal,
    CompositeNtsc,
};

struct DisplaySwitchResult
{
    bool ok{false};
    std::string error;
};

/// Path to the file that persists the chosen display output across restarts.
constexpr const char *kDisplayPreferenceFile = "/tmp/cockscreen-display";

/// Name of the DRM connector to use for HDMI output (as seen by Qt eglfs KMS).
constexpr const char *kHdmiConnectorName = "HDMI1";

/// Name of the DRM connector to use for composite output.
constexpr const char *kCompositeConnectorName = "Composite1";

/// Returns the connector name to use based on the persisted preference file.
/// Falls back to HDMI if no preference has been saved.
const char *preferred_connector_name();

/// Returns the connector name that should be used for startup, preferring the
/// persisted preference only when that DRM connector is currently connected.
/// Returns nullptr when no known display connector is connected.
const char *startup_connector_name();

/// Write the display preference and restart the process via execv.
/// The calling process will be replaced; this function only returns on error.
/// argc/argv must be the original values passed to main().
DisplaySwitchResult switch_display_output(DisplayOutput output, int argc, char **argv);

} // namespace cockscreen::runtime::pi
