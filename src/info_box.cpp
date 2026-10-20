#include "info_box.h"

#include "playback.h"
#include "plugin.h"
#include "resource.h"
#include "url_util.h"

#include "in2.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kStatusIntervalMs = 500;

struct InfoBoxState {
    std::string file;
};

std::string format_clock(int ms) {
    if (ms < 0) {
        return "--:--";
    }
    const int total_sec = ms / 1000;
    const int sec = total_sec % 60;
    const int min = (total_sec / 60) % 60;
    const int hour = total_sec / 3600;
    char buffer[32] = {};
    if (hour > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hour, min, sec);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", min, sec);
    }
    return buffer;
}

std::string channel_label(int channels) {
    if (channels <= 0) {
        return "unknown channels";
    }
    if (channels == 1) {
        return "mono";
    }
    if (channels == 2) {
        return "stereo";
    }
    return std::to_string(channels) + " channels";
}

void set_item_text(HWND dialog, int id, const std::string& text) {
    SetDlgItemTextA(dialog, id, text.c_str());
}

std::string build_status_text(const PlaybackInfoSnapshot& snap) {
    std::string text;
    if (!snap.current || !snap.decoder.open) {
        text = "Not currently playing.\r\n\r\n";
        text += "Open this stream to see live HLS status (segment URL, bitrate, packets).";
        return text;
    }

    if (!snap.connection_status.empty()) {
        text += snap.connection_status;
        text += "\r\n";
    }
    text += snap.paused ? "Paused" : "Playing";
    text += snap.decoder.live ? " - Live HLS" : " - On-demand HLS";
    text += "\r\n";

    if (!snap.decoder.format.empty()) {
        text += "Demuxer: ";
        text += snap.decoder.format;
        text += "\r\n";
    }
    if (!snap.decoder.codec.empty()) {
        text += "Codec: ";
        text += snap.decoder.codec;
        text += "\r\n";
    }

    char line[160] = {};
    if (snap.decoder.bitrate_kbps > 0 || snap.decoder.sample_rate > 0) {
        std::snprintf(
            line,
            sizeof(line),
            "Audio: %d kbps - %d Hz - %s\r\n",
            snap.decoder.bitrate_kbps > 0 ? snap.decoder.bitrate_kbps : 0,
            snap.decoder.sample_rate,
            channel_label(snap.decoder.channels).c_str());
        text += line;
    }

    if (snap.decoder.live) {
        std::snprintf(line, sizeof(line), "Position: %s (live)\r\n", format_clock(snap.position_ms).c_str());
    } else {
        std::snprintf(
            line,
            sizeof(line),
            "Position: %s / %s\r\n",
            format_clock(snap.position_ms).c_str(),
            format_clock(static_cast<int>(snap.decoder.duration_ms)).c_str());
    }
    text += line;

    std::snprintf(
        line,
        sizeof(line),
        "Segments seen: %llu - Packets: %llu\r\n",
        static_cast<unsigned long long>(snap.decoder.segments_seen),
        static_cast<unsigned long long>(snap.decoder.packets_read));
    text += line;

    text += "\r\nCurrent segment / open URL:\r\n";
    if (!snap.decoder.last_segment_url.empty()) {
        text += snap.decoder.last_segment_url;
    } else {
        text += "(waiting for next segment open…)";
    }
    return text;
}

void refresh_dialog(HWND dialog, const InfoBoxState* state) {
    if (!state) {
        return;
    }
    Playback* playback = plugin_playback();
    if (!playback) {
        return;
    }
    const PlaybackInfoSnapshot snap = playback->info_snapshot(state->file.c_str());

    const std::string url = !snap.decoder.url.empty() ? snap.decoder.url : snap.file;
    set_item_text(dialog, IDC_INFO_URL, url);
    set_item_text(dialog, IDC_INFO_TITLE, snap.title);

    std::string type = "HTTP Live Streaming";
    if (snap.current && snap.decoder.open) {
        type = snap.decoder.live ? "Live HLS audio" : "On-demand HLS audio";
        if (!snap.decoder.format.empty()) {
            type += " (";
            type += snap.decoder.format;
            type += ")";
        }
    }
    set_item_text(dialog, IDC_INFO_TYPE, type);

    if (snap.current && snap.decoder.open) {
        char audio[128] = {};
        std::snprintf(
            audio,
            sizeof(audio),
            "%s - %d kbps - %d Hz - %s",
            snap.decoder.codec.empty() ? "audio" : snap.decoder.codec.c_str(),
            snap.decoder.bitrate_kbps,
            snap.decoder.sample_rate,
            channel_label(snap.decoder.channels).c_str());
        set_item_text(dialog, IDC_INFO_AUDIO, audio);
    } else {
        set_item_text(dialog, IDC_INFO_AUDIO, "Available while the stream is playing");
    }

    set_item_text(dialog, IDC_INFO_STATUS, build_status_text(snap));
}

INT_PTR CALLBACK info_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<InfoBoxState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        auto* initial = reinterpret_cast<InfoBoxState*>(lparam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));
        state = initial;
        refresh_dialog(dialog, state);
        SetTimer(dialog, kStatusTimer, kStatusIntervalMs, nullptr);
        return TRUE;
    }
    case WM_TIMER:
        if (wparam == kStatusTimer) {
            refresh_dialog(dialog, state);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, INFOBOX_UNCHANGED);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, INFOBOX_UNCHANGED);
        return TRUE;
    case WM_DESTROY:
        KillTimer(dialog, kStatusTimer);
        break;
    default:
        break;
    }
    return FALSE;
}

}  // namespace

int show_hls_info_box(const char* file, HWND parent) {
    InfoBoxState state;
    state.file = file ? canonical_media_path(file) : std::string{};
    if (state.file.empty() && file) {
        state.file = file;
    }
    const INT_PTR result = DialogBoxParamW(
        reinterpret_cast<HINSTANCE>(plugin_instance()),
        MAKEINTRESOURCEW(IDD_INFOBOX),
        parent,
        info_proc,
        reinterpret_cast<LPARAM>(&state));
    return result == -1 ? INFOBOX_UNCHANGED : static_cast<int>(result);
}
