# Winamp interface headers

`in2.h` and `out.h` in this directory are the input and output plug-in interfaces from the published Winamp desktop source, commit `6581a99157179043e0243ad1cacba35c5d48f19c` (`Src/Winamp/IN2.H` and `OUT.H`).

They are stored here so the plug-in is built against the real structure layouts, calling conventions and version constants (`IN_VER_OLD` is `0x100`). They are not part of the MIT licence that covers the rest of this plug-in. The Winamp source tree they come from is distributed under the Winamp Collaborative Licence. Keep that distinction if you copy these headers onwards.

The plug-in sets `version` to `IN_VER_OLD` so the description stays a `char*` string and older Winamp 5.x clients keep the original initialisation behaviour. `Init` still matches the function-pointer type in this header, which returns `int`. On Win32 `__cdecl` that is compatible with a caller that ignores the return value.

No other Winamp source is included.
