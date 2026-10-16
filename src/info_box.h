#pragma once

#include <windows.h>

// Shows the HLS file info dialog (Winamp "View file info" / I). Always returns
// INFOBOX_UNCHANGED because we do not edit tags.
int show_hls_info_box(const char* file, HWND parent);
