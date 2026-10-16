# Dependencies

## FFmpeg

`scripts/setup-deps.ps1` downloads this exact archive when `third_party/ffmpeg/bin/avformat-63.dll` is absent:

`ffmpeg-9.0.2_x86-windows-shared-lgpl.zip`

from System233/ffmpeg-msvc-prebuilt. The script checks the SHA256 and that the DLL machine type is `0x014C`.

After extraction the tree looks like this:

- `third_party/ffmpeg/include` — headers
- `third_party/ffmpeg/lib` — MSVC import libraries
- `third_party/ffmpeg/bin` — x86 runtime DLLs and the FFmpeg tools

The download and the extracted tree are gitignored. They are not MIT-licensed. Read `THIRD-PARTY-NOTICES.md` and `third_party/ffmpeg/LICENSE.txt`.

`ffmpeg_win32.props` names the import libraries and the delay-loaded DLL names for this 9.0.2 build (`avformat-63.dll`, `avcodec-63.dll`, `avutil-61.dll`, `swresample-7.dll`). If you replace the FFmpeg build you must update those names and rebuild.

## Winamp SDK headers

`third_party/winamp-sdk` contains `in2.h` and `out.h` only. See the README in that directory. Do not replace them with guessed structure definitions.
