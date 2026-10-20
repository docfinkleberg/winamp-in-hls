HLS Input Plug-in 1.1.1 for 32-bit Winamp 5.x
================================================

This folder plays HTTP and HTTPS HLS audio (.m3u8), including live radio
and on-demand playlists that FFmpeg can demux. DRM-protected streams are
not supported.

Installation
------------
Preferred: run the NSIS installer (in_hls-1.1.1-win32-setup.exe). It finds
Winamp's Plugins folder when the registry entries are present.

Manual install:
1. Install 32-bit Winamp 5.x. A 64-bit Winamp process cannot load this DLL.
2. Copy in_hls.dll into Winamp's Plugins directory.
3. Copy the in_hls folder (the FFmpeg DLLs) into that same Plugins directory,
   so the DLLs live in Plugins\in_hls and do not clash with other plug-ins.
4. Restart Winamp.
5. Play a local .m3u8 file, or open a URL whose path ends in .m3u8.
   Signed query strings are passed through unchanged.

Winamp otherwise treats .m3u8 as one of its own playlists. This plug-in
renames that extension to .m3u9 when you open, drop or pass the file on
the command line, then plays the original playlist. The playlist editor
may show the .m3u9 name.

The plug-in loads the FFmpeg DLLs from the Plugins\in_hls folder next to
in_hls.dll. An FFmpeg installation on PATH is not used and is not required.
Leave that folder beside the plug-in. Do not copy those DLLs into Plugins.

Playback
--------
Live streams do not show a duration and cannot be seeked.
On-demand streams report a duration when FFmpeg provides one, and seeking
uses avformat_seek_file.

Open the plug-in configuration from Winamp's input plug-in preferences to
change the network timeout, buffer, user agent and diagnostic log.
The log is in_hls.log next to the DLL when that folder is writable.
If Winamp's Plugins folder is protected, the log is written to
%LOCALAPPDATA%\in_hls\in_hls.log instead.

Licences
--------
in_hls.dll's own code is under LICENSE.md (MIT).
The FFmpeg DLLs are an LGPLv3 shared build. FFMPEG-LGPL-3.0.txt and
GPL-3.0.txt are included because LGPLv3 requires them. See
THIRD-PARTY-NOTICES.md before you redistribute this folder. Replacing
the FFmpeg DLLs with another build of the same names is how a recipient
can relink against a modified FFmpeg.
