# Third-party notices

This file records what was actually checked for the pinned dependency build. It is not a statement that every transitive notice inside those libraries has been reproduced, and it is not a general claim that a redistribution is free of further obligations.

## FFmpeg 9.0.2 Win32 LGPL shared (MSVC)

Source of the binaries used to develop and package this plug-in:

- Project: System233/ffmpeg-msvc-prebuilt
- File: `ffmpeg-9.0.2_x86-windows-shared-lgpl.zip`
- SHA256: `893BDBDFB334D02246098751A8FA06F1AA6903F7FAB504701748E1F8DF59026C`
- Declared package architecture: `x86-windows`
- PE machine of `avformat-63.dll`, `avcodec-63.dll`, `avutil-61.dll`, `swresample-7.dll`, `swscale-10.dll` and `ffmpeg.exe`: `0x014C` (i386)

`ffmpeg.exe -version` reported this configuration (wrapped):

`--enable-version3 --disable-static --enable-shared --enable-avcodec --enable-avformat --enable-swresample --enable-swscale` and a set of third-party codecs including libopus, libvorbis, libmp3lame, libaom, dav1d and OpenSSL. The configuration line contained **no** `--enable-gpl` and **no** `--enable-nonfree`. `BUILD_INFO` in the archive says `LibraryLinkage: dynamic` and `CRTLinkage: static` (the FFmpeg DLLs were built with `/MT`).

The archive's `LICENSE.txt` is the GNU Lesser General Public License version 3. `--enable-version3` selects that licence for FFmpeg itself. LGPLv3 is used here by dynamic linking: `in_hls.dll` delay-loads the FFmpeg DLLs and does not absorb their object code.

Obligations that follow from that combination, if you distribute the DLLs with the plug-in:

- Keep a prominent notice that FFmpeg is used and is covered by the LGPLv3. `README.txt` in the package and this file are that notice.
- Ship the LGPLv3 text and the GPLv3 text it refers to. `scripts/package.ps1` copies `LICENSE.txt` from the FFmpeg archive and the GNU GPL-3.0 text.
- Dynamic linking lets a recipient replace the FFmpeg DLLs. Do not strip the delay-load behaviour or fold these libraries into `in_hls.dll`.
- The FFmpeg source corresponding to this binary is the FFmpeg 9.0.2 release plus the packager's build options above. A recipient who wants to relink against a modified FFmpeg can replace the DLLs beside the plug-in.
- The archive does not include a separate licence file for every library named in the configuration (OpenSSL, libvorbis, and the others). Those components were linked by the packager. Review the packager's release and each component's licence before you rely on this package commercially or in a product that forbids a particular licence. This repository does not add those missing texts and does not describe the result as fully audited.

Runtime DLLs copied into the package: `avutil-61.dll`, `swresample-7.dll`, `swscale-10.dll`, `avcodec-63.dll`, `avformat-63.dll`. Development import libraries, headers, `ffmpeg.exe`, `ffprobe.exe` and `ffplay.exe` are not part of the runtime package.

## Winamp interface headers

`third_party/winamp-sdk/in2.h` and `out.h` are the Winamp 5.6x input and output plug-in interfaces. They are not MIT-licensed with this plug-in. See `third_party/winamp-sdk/README.md`.
