#pragma once

#include <string>

// Playlist titles Winamp shows while an HLS station is opening or stalled.
// Bracketed strings match the Shoutcast-style status line the playlist item uses.

inline bool text_has_ci(const std::string& haystack, const char* needle) {
    if (!needle || !needle[0]) {
        return false;
    }
    const size_t needle_len = std::char_traits<char>::length(needle);
    if (needle_len > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle_len; ++j) {
            const char a = haystack[i + j];
            const char b = needle[j];
            const char ca = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
            const char cb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
            if (ca != cb) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

enum class StreamHint {
    None,
    PlaylistOpen,
    ManifestParse,
    MediaSegment,
    Reconnect,
    Http404,
    TimedOut,
    BadManifest,
    UnsupportedCodec,
    Drm,
};

inline bool looks_like_playlist_url(const std::string& text) {
    return text_has_ci(text, ".m3u8") || text_has_ci(text, ".m3u9");
}

inline StreamHint hint_from_text(const std::string& text) {
    if (text.empty()) {
        return StreamHint::None;
    }
    if (text_has_ci(text, "aes-128") || text_has_ci(text, "sample-aes") || text_has_ci(text, "ext-x-key") ||
        text_has_ci(text, "decryption key") || text_has_ci(text, "unable to open key") ||
        (text_has_ci(text, "encrypt") && (text_has_ci(text, "key") || text_has_ci(text, "drm"))) ||
        text_has_ci(text, "drm")) {
        return StreamHint::Drm;
    }
    if (text_has_ci(text, "404") || text_has_ci(text, "not found")) {
        return StreamHint::Http404;
    }
    if (text_has_ci(text, "timed out") || text_has_ci(text, "timeout") || text_has_ci(text, "timedout")) {
        return StreamHint::TimedOut;
    }
    if (text_has_ci(text, "no decoder") || text_has_ci(text, "decoder (codec") ||
        text_has_ci(text, "unsupported codec") || text_has_ci(text, "codec not found") ||
        text_has_ci(text, "unknown codec") || text_has_ci(text, "invalid codec")) {
        return StreamHint::UnsupportedCodec;
    }
    if (text_has_ci(text, "invalid data") || text_has_ci(text, "malformed") ||
        text_has_ci(text, "failed to read playlist") || text_has_ci(text, "does not contain an audio") ||
        text_has_ci(text, "invalid m3u") || text_has_ci(text, "bad manifest")) {
        return StreamHint::BadManifest;
    }
    if (text_has_ci(text, "will reconnect") || text_has_ci(text, "reconnect at") ||
        text_has_ci(text, "connection refused") || text_has_ci(text, "connection reset")) {
        return StreamHint::Reconnect;
    }
    if (text_has_ci(text, "#ext-x") || text_has_ci(text, "skip ('#ext")) {
        return StreamHint::ManifestParse;
    }
    if (looks_like_playlist_url(text) &&
        (text_has_ci(text, "opening") || text_has_ci(text, "hls request") || text_has_ci(text, "http"))) {
        return StreamHint::PlaylistOpen;
    }
    if ((text_has_ci(text, "opening 'http") || text_has_ci(text, "opening \"http") ||
         text_has_ci(text, "hls request for url")) &&
        !looks_like_playlist_url(text)) {
        return StreamHint::MediaSegment;
    }
    return StreamHint::None;
}

inline const char* status_for_hint(StreamHint hint) {
    switch (hint) {
    case StreamHint::Http404:
        return "[404 Segment Not Found]";
    case StreamHint::TimedOut:
        return "[Manifest Timed Out]";
    case StreamHint::BadManifest:
        return "[Invalid M3U8 / Bad Manifest]";
    case StreamHint::UnsupportedCodec:
        return "[Unsupported HLS Variant / Codec]";
    case StreamHint::Drm:
        return "[DRM Protected / Key Error]";
    case StreamHint::Reconnect:
        return "[Connecting...]";
    default:
        return nullptr;
    }
}

inline bool hint_is_terminal(StreamHint hint) {
    return hint == StreamHint::Http404 || hint == StreamHint::TimedOut || hint == StreamHint::BadManifest ||
           hint == StreamHint::UnsupportedCodec || hint == StreamHint::Drm;
}

inline std::string buffering_status(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return std::string("[Buffering... ") + std::to_string(percent) + "%]";
}

inline bool title_looks_like_status(const std::string& title) {
    if (title.size() < 3 || title.front() != '[') {
        return false;
    }
    return text_has_ci(title, "[connecting") || text_has_ci(title, "[downloading playlist") ||
           text_has_ci(title, "[parsing m3u8") || text_has_ci(title, "[selecting stream") ||
           text_has_ci(title, "[buffering") || text_has_ci(title, "[invalid m3u8") ||
           text_has_ci(title, "[404 ") || text_has_ci(title, "[manifest timed") ||
           text_has_ci(title, "[unsupported hls") || text_has_ci(title, "[drm protected") ||
           text_has_ci(title, "[segment buffer");
}
