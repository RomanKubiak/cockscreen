#include "cockscreen/runtime/MidiInputMonitor.hpp"

#ifndef _WIN32
#include "cockscreen/runtime/midi/PortDiscovery.hpp"
#endif

#include <QString>

#include <algorithm>
#include <cctype>
#include <string>
namespace cockscreen::runtime
{

MidiInputMonitor::MidiInputMonitor(std::string requested_device, const std::vector<MidiCcMapping> *scene_midi_cc_mappings)
    : scene_midi_cc_mappings_{scene_midi_cc_mappings}, requested_device_{std::move(requested_device)}
{
    open_sequence();
}

MidiInputMonitor::~MidiInputMonitor()
{
    close_sequence();
}

bool MidiInputMonitor::is_active() const
{
    return active_;
}

QString MidiInputMonitor::status_message() const
{
    return QString::fromStdString(status_message_);
}

QString MidiInputMonitor::activity_message() const
{
    return QString::fromStdString(activity_message_);
}

void MidiInputMonitor::advance(float delta_seconds)
{
    if (delta_seconds <= 0.0F)
    {
        return;
    }

    for (auto &event : events_)
    {
        if (event.active)
        {
            event.age += delta_seconds;
        }
    }

    prune_events();
}

void MidiInputMonitor::poll()
{
    if (!active_)
    {
        return;
    }

#ifdef _WIN32
    std::vector<PendingMessage> snapshot;
    {
        std::lock_guard<std::mutex> lock{queue_mutex_};
        snapshot.swap(message_queue_);
    }

    for (const auto &msg : snapshot)
    {
        const int type    = msg.status & 0xF0;
        const int channel = msg.status & 0x0F;
        if (type == 0x90 && msg.data2 > 0)
        {
            push_note_on(channel, msg.data1, msg.data2);
        }
        else if (type == 0xB0)
        {
            update_control_change(channel, msg.data1, msg.data2);
        }
    }
#else
    if (sequence_ == nullptr)
    {
        return;
    }

    while (snd_seq_event_input_pending(sequence_, 1) > 0)
    {
        snd_seq_event_t *event = nullptr;
        const int result = snd_seq_event_input(sequence_, &event);
        if (result <= 0 || event == nullptr)
        {
            break;
        }

        if (event->type == SND_SEQ_EVENT_NOTEON)
        {
            const int velocity = event->data.note.velocity;
            if (velocity > 0)
            {
                push_note_on(event->data.note.channel, event->data.note.note, velocity);
            }
        }
        else if (event->type == SND_SEQ_EVENT_CONTROLLER)
        {
            update_control_change(event->data.control.channel, event->data.control.param, event->data.control.value);
        }
    }
#endif
}

void MidiInputMonitor::populate_frame(core::ControlFrame *frame) const
{
    if (frame == nullptr)
    {
        return;
    }

    frame->midi_primary = 0.0F;
    frame->midi_secondary = 0.0F;
    frame->midi_notes.fill(0.0F);
    frame->midi_velocities.fill(0.0F);
    frame->midi_ages.fill(-1.0F);
    frame->midi_channels.fill(0.0F);
    frame->midi_cc_values = cc_values_;

    bool have_primary = false;
    for (std::size_t index = 0; index < events_.size(); ++index)
    {
        const auto &event = events_[index];
        if (!event.active)
        {
            continue;
        }

        frame->midi_notes[index] = event.note;
        frame->midi_velocities[index] = event.velocity;
        frame->midi_ages[index] = event.age;
        frame->midi_channels[index] = event.channel;
        if (!have_primary)
        {
            frame->midi_primary = event.note;
            frame->midi_secondary = event.velocity;
            have_primary = true;
        }
    }
}

#ifdef _WIN32

namespace
{
std::string wide_to_utf8(const wchar_t *str)
{
    if (str == nullptr || str[0] == L'\0') { return {}; }
    const int wlen   = static_cast<int>(wcslen(str));
    const int needed = WideCharToMultiByte(CP_UTF8, 0, str, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) { return {}; }
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str, wlen, result.data(), needed, nullptr, nullptr);
    return result;
}
} // namespace

void MidiInputMonitor::open_sequence()
{
    if (requested_device_.empty() || requested_device_ == "@disabled@" || requested_device_ == "@DISABLED@")
    {
        set_status("disabled");
        return;
    }

    const UINT num_devices = midiInGetNumDevs();
    if (num_devices == 0)
    {
        set_status("MIDI input: no devices available");
        return;
    }

    UINT selected_id = 0;
    bool found       = false;

    // Try numeric device index first.
    try
    {
        const UINT parsed_id = static_cast<UINT>(std::stoul(requested_device_));
        if (parsed_id < num_devices)
        {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(parsed_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            {
                selected_id    = parsed_id;
                resolved_label_ = wide_to_utf8(caps.szPname);
                found          = true;
            }
        }
    }
    catch (...) {}

    if (!found)
    {
        // Case-insensitive name match.
        for (UINT i = 0; i < num_devices; ++i)
        {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) { continue; }
            const auto name = wide_to_utf8(caps.szPname);
            if (contains_case_insensitive(name, requested_device_) ||
                contains_case_insensitive(requested_device_, name))
            {
                selected_id    = i;
                resolved_label_ = name;
                found          = true;
                break;
            }
        }
    }

    if (!found)
    {
        set_status("MIDI input not found: " + requested_device_);
        return;
    }

    const MMRESULT result = midiInOpen(&midi_in_, selected_id,
                                       reinterpret_cast<DWORD_PTR>(&winmm_callback),
                                       reinterpret_cast<DWORD_PTR>(this),
                                       CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR)
    {
        midi_in_ = nullptr;
        set_status("MIDI input could not open: " + resolved_label_);
        return;
    }

    midiInStart(midi_in_);
    active_ = true;
    set_status("Input: " + resolved_label_);
}

void MidiInputMonitor::close_sequence()
{
    if (midi_in_ != nullptr)
    {
        midiInStop(midi_in_);
        midiInReset(midi_in_); // flushes pending sysex buffers
        midiInClose(midi_in_); // blocks until callback thread exits
        midi_in_ = nullptr;
    }
    active_ = false;
}

void CALLBACK MidiInputMonitor::winmm_callback(HMIDIIN /*hmi*/, UINT wMsg, DWORD_PTR dwInstance,
                                               DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/)
{
    if (wMsg != MIM_DATA) { return; }
    auto *self = reinterpret_cast<MidiInputMonitor *>(dwInstance);
    const PendingMessage msg{
        static_cast<int>(dwParam1 & 0xFF),
        static_cast<int>((dwParam1 >> 8) & 0xFF),
        static_cast<int>((dwParam1 >> 16) & 0xFF)};
    std::lock_guard<std::mutex> lock{self->queue_mutex_};
    self->message_queue_.push_back(msg);
}

#else // Linux ALSA

void MidiInputMonitor::open_sequence()
{
    if (requested_device_.empty() || requested_device_ == "@disabled@" || requested_device_ == "@DISABLED@")
    {
        set_status("disabled");
        return;
    }

    const auto resolved = resolve_requested_port(requested_device_, &resolved_label_);
    if (!resolved.has_value())
    {
        set_status(std::string{"MIDI input not found: "} + requested_device_);
        return;
    }

    source_client_ = resolved->first;
    source_port_ = resolved->second;

    if (snd_seq_open(&sequence_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0 || sequence_ == nullptr)
    {
        close_sequence();
        set_status("MIDI input could not open ALSA sequencer");
        return;
    }

    snd_seq_set_client_name(sequence_, "cockscreen-midi");
    input_port_ = snd_seq_create_simple_port(sequence_, "input",
                                             SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_APPLICATION);
    if (input_port_ < 0)
    {
        close_sequence();
        set_status("MIDI input could not create input port");
        return;
    }

    if (snd_seq_connect_from(sequence_, input_port_, source_client_, source_port_) < 0)
    {
        close_sequence();
        set_status(std::string{"MIDI input could not connect to "} + resolved_label_);
        return;
    }

    active_ = true;
    set_status(std::string{"Input: "} + resolved_label_);
}

void MidiInputMonitor::close_sequence()
{
    if (sequence_ != nullptr)
    {
        snd_seq_close(sequence_);
        sequence_ = nullptr;
    }

    input_port_ = -1;
    source_client_ = -1;
    source_port_ = -1;
    active_ = false;
}

#endif // _WIN32

void MidiInputMonitor::push_note_on(int channel, int note, int velocity)
{
    ++note_on_count_;
    activity_message_ = "notes=" + std::to_string(note_on_count_) + ", last note=" + std::to_string(note) +
                        ", channel=" + std::to_string(channel) + ", velocity=" + std::to_string(velocity);

    for (std::size_t index = events_.size() - 1; index > 0; --index)
    {
        events_[index] = events_[index - 1];
    }

    events_[0] = MidiEvent{static_cast<float>(note), static_cast<float>(velocity) / 127.0F, 0.0F,
                           static_cast<float>(channel), true};
}

void MidiInputMonitor::update_control_change(int channel, int controller, int value)
{
    if (channel < 0 || channel >= static_cast<int>(core::kMidiChannelCount) || controller < 0 ||
        controller >= static_cast<int>(core::kMidiCcCount))
    {
        return;
    }

    const float normalized = std::clamp(static_cast<float>(value) / 127.0F, 0.0F, 1.0F);

    ++controller_count_;
    activity_message_ = "cc=" + std::to_string(controller_count_) + ", last cc=" + std::to_string(controller) +
                        ", channel=" + std::to_string(channel) + ", value=" + std::to_string(value);

    if (scene_midi_cc_mappings_ != nullptr)
    {
        for (const auto &mapping : *scene_midi_cc_mappings_)
        {
            if (mapping.channel != channel || mapping.controller != controller)
            {
                continue;
            }

        }
    }

    const std::size_t index = static_cast<std::size_t>(channel) * core::kMidiCcCount + static_cast<std::size_t>(controller);
    cc_values_[index] = normalized;
}

void MidiInputMonitor::prune_events()
{
    for (auto &event : events_)
    {
        if (event.active && event.age > kEventLifetimeSeconds)
        {
            event = {};
            event.age = -1.0F;
        }
    }
}

void MidiInputMonitor::set_status(std::string message)
{
    status_message_ = std::move(message);
}

bool MidiInputMonitor::contains_case_insensitive(std::string_view text, std::string_view needle)
{
    if (needle.empty() || text.size() < needle.size())
    {
        return false;
    }

    for (std::size_t offset = 0; offset + needle.size() <= text.size(); ++offset)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            const auto left = static_cast<unsigned char>(text[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right))
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return true;
        }
    }

    return false;
}

#ifndef _WIN32

std::optional<std::pair<int, int>> MidiInputMonitor::parse_numeric_port(std::string_view text)
{
    const auto separator = text.find(':');
    if (separator == std::string_view::npos)
    {
        return std::nullopt;
    }

    try
    {
        const int client = std::stoi(std::string{text.substr(0, separator)});
        const int port = std::stoi(std::string{text.substr(separator + 1)});
        if (client >= 0 && port >= 0)
        {
            return std::pair{client, port};
        }
    }
    catch (...)
    {
    }

    return std::nullopt;
}

std::optional<std::pair<int, int>> MidiInputMonitor::resolve_requested_port(std::string_view requested_device,
                                                                            std::string *resolved_label)
{
    if (const auto numeric_port = parse_numeric_port(requested_device); numeric_port.has_value())
    {
        if (resolved_label != nullptr)
        {
            *resolved_label = std::string{requested_device};
        }
        return numeric_port;
    }

    const auto ports = midi::read_ports();
    for (const auto &record : ports)
    {
        const auto label = midi::make_label(record);
        if (requested_device == label || requested_device == record.client_name || requested_device == record.port_name ||
            contains_case_insensitive(label, requested_device) || contains_case_insensitive(record.client_name, requested_device) ||
            contains_case_insensitive(record.port_name, requested_device))
        {
            if (resolved_label != nullptr)
            {
                *resolved_label = label;
            }
            return std::pair{record.client, record.port};
        }
    }

    return std::nullopt;
}

#endif // !_WIN32

} // namespace cockscreen::runtime