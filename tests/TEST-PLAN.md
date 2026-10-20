# Manual test plan

Automated checks live in `tests/test_main.cpp` and run from `scripts/build.ps1`. They cover the byte buffer, URL classification (including a signed query string), a generated stereo WAV, seeking within that WAV, pause and stop, and cancellation of a blocked open. When `ffmpeg.exe` from the development package is present they also generate a two-second local AAC HLS playlist and play it through the Winamp output callbacks using a stand-in output module.

No public stream address is compiled into the tests. To try a stream you supply:

```powershell
$env:IN_HLS_TEST_URL = 'https://example.invalid/stream.m3u8'
.\build\Win32\Release\in_hls_tests.exe
```

Use an address you are allowed to open. The variable is optional.

## Winamp

These steps need 32-bit Winamp 5.x and the files from `dist\in_hls-win32` copied into `Plugins`. A successful DLL build does not by itself show that Winamp loaded the plug-in.

1. Restart Winamp and confirm **HLS Input Plug-in v1.1.0 (x86)** appears under input plug-ins. Open About and confirm the FFmpeg version string. The FFmpeg DLLs should be in `Plugins\in_hls`, not beside other plug-ins.
2. Play a local audio file wrapped as `.m3u8` only if you have one. The automated test already decodes a WAV directly. In Winamp, play a local `.m3u8` playlist whose segments Winamp can reach. Winamp's playlist loader would otherwise expand `.m3u8`; the plug-in renames that to `.m3u9` for the playlist editor and still opens the original file.
3. Play a finite VOD HLS URL ending in `.m3u8`, including one with a query string. The title should appear, the length should become a positive time once playback has opened, and the position should advance.
4. Seek within that VOD item. Playback should continue from the new position without a stuck UI.
5. Pause and resume. The position should hold, then continue. Stop should return promptly.
6. Play a live HLS URL. The length should stay unknown and seeking should do nothing.
7. Open a malformed playlist and an unreachable host. Winamp should skip or stop the item and remain responsive. With logging enabled, `in_hls.log` should contain the FFmpeg error.
8. Disconnect the network during a live item, then restore it. With reconnect enabled the plug-in should either resume or fail the item without freezing Winamp.
9. Stop Winamp and confirm `in_hls.log` records `playback thread stopped` if logging was on. The process should exit without a leftover audible buffer.

Enable logging from the plug-in configuration dialogue, or set `log=1` in `in_hls.ini`, before the failure cases so the log is useful. Leave it off for ordinary listening.
