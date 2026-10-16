# HLS input plug-in for Winamp 5.x

`in_hls.dll` is a 32-bit Winamp input plug-in. It plays HTTP and HTTPS HLS audio whose URL path ends in `.m3u8`, and local `.m3u8` playlists. Decoding is done by FFmpeg's libraries (`libavformat`, `libavcodec`, `libavutil` and `libswresample`). The plug-in does not run `ffmpeg.exe`.

Live playlists, which do not report a duration, are treated as non-seekable and use Winamp's unknown-length convention (`-1` milliseconds). On-demand playlists with a finite duration can be seeked. DRM-protected streams are not supported.

The plug-in code in this repository is under the MIT licence in `LICENSE.md`. FFmpeg and the Winamp interface headers have their own licences. See `THIRD-PARTY-NOTICES.md` before you redistribute a build.

## What you need

- 64-bit Windows with the 32-bit MSVC toolset (v143) and the Windows 10/11 SDK.
- This repository. `scripts/setup-deps.ps1` downloads the pinned FFmpeg 9.0.2 Win32 LGPL shared build if `third_party/ffmpeg` is missing.
- The authentic Winamp headers already stored in `third_party/winamp-sdk` (`in2.h` and `out.h`).

CMake is optional. The supported build is `scripts/build.ps1`, which uses MSBuild.

## Build

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The script stops if compilation, linking or the unit tests fail. A successful build writes:

`build\Win32\Release\in_hls.dll`

Open `winamp-hls.sln` in Visual Studio, choose **Release | Win32**, and build that configuration if you prefer the IDE. Do not select x64. Run `scripts/setup-deps.ps1` once so the FFmpeg headers and import libraries exist before the IDE build.

CMake, when it is installed:

```powershell
cmake -S . -B build\cmake -A Win32
cmake --build build\cmake --config Release
```

## Package

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

This creates `dist\in_hls-win32` with the plug-in, the x86 FFmpeg runtime DLLs, installation notes and the licence texts. Import libraries and headers are not included.

## Installer

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\make-installer.ps1
```

This runs the package step if needed, then compiles an NSIS installer to `dist\in_hls-1.0.0-win32-setup.exe`. NSIS is the same installer system Winamp and most Winamp plug-ins use. If `makensis` is not already on the machine, the script downloads the pinned NSIS 3.13 zip into `third_party/downloads` and extracts it under `tools/nsis`.

## Install in Winamp

**With the installer:** run `dist\in_hls-1.0.0-win32-setup.exe`. It detects Winamp's Plugins folder from the registry when possible. Restart Winamp afterwards.

**Manual / scripted:**

1. Install 32-bit Winamp 5.x.
2. Copy every file from `dist\in_hls-win32` into Winamp's `Plugins` folder (the same folder as `in_hls.dll`), or run `scripts\install.ps1`.
3. Restart Winamp.
4. Play a local `.m3u8` file, or open a location whose path ends in `.m3u8`. Query strings are preserved.

Winamp's own playlist loader claims a path ending in `.m3u8` and expands it into segment files. The plug-in renames that extension to `.m3u9` on the command line, in the Open URL box and for files dropped on the main window, then opens the original `.m3u8` playlist. The playlist editor can show the `.m3u9` alias. A normal Winamp playlist saved as `.m3u8` and opened with the playlist commands is left for Winamp.

The FFmpeg DLLs are loaded from the plug-in directory with `LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`, and the FFmpeg imports are delay-loaded. Winamp's own directory and the process `PATH` are not required. Leave the DLLs beside `in_hls.dll`.

Optional settings are stored in `in_hls.ini` next to the plug-in (`log`, `verbose`, `timeout_ms`, `buffer_ms`, `reconnect`, `user_agent`). The configuration dialogue in Winamp edits the common values. Set `IN_HLS_LOG=1` to force a log for one session. The log file is `in_hls.log` next to the plug-in when that folder can be written. Otherwise it is `%LOCALAPPDATA%\in_hls\in_hls.log`.

## Tests

`in_hls_tests.exe` checks the audio buffer, URL handling, a generated WAV, stop-during-connect, pause, seek and, when `ffmpeg.exe` from the development package is available, a local HLS playlist. It does not embed a public stream address. Set `IN_HLS_TEST_URL` to exercise a stream you are allowed to open.

Manual cases are listed in `tests/TEST-PLAN.md`. A successful compile is not the same thing as a test inside Winamp. This environment does not claim the plug-in was loaded by Winamp unless that was actually done.
