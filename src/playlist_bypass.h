#pragma once

#include <windows.h>

// Winamp's playlist loader claims every .m3u8 path before an input plug-in
// can play it. These helpers rename that extension to .m3u9, which the loader
// does not recognise, and restore the real name when playback starts.
//
// Local .m3u / .pls station lists are parsed here so nested .m3u8 URLs are
// enqueued as titled .m3u9 streams instead of being expanded into segments.
void install_playlist_bypass(HWND main_window);
void remove_playlist_bypass();
