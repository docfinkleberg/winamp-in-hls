#pragma once

#include <windows.h>

// Winamp advances the playlist when an input plug-in posts this message to
// the main window. The constant is not declared in in2.h. The SDK sample
// in_raw.c uses WM_USER+2 for the same purpose.
#ifndef WM_WA_MPEG_EOF
#define WM_WA_MPEG_EOF (WM_USER + 2)
#endif
