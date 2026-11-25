# Movies (libSceAvPlayer)

## Why movies were skipped

The game plays its cutscenes through the console's `libSceAvPlayer` library
(`sceAvPlayerInit` → `AddSource` → event `READY (2)` → `StreamCount` /
`GetStreamInfo` / `EnableStream` → `Start` → `IsActive` / `GetVideoDataEx` /
`GetAudioData` → `Close`).

This runtime implemented that protocol as a **diagnostic stub**: `AddSource`
logged the requested file (size + MP4 brand) but `StreamCount` always answered
`0`, `IsActive` always answered `false` and `GetVideoDataEx` never returned a
frame. The game's `READY` callback therefore saw zero streams and treated the
movie as finished immediately — that is the skip. No decoder existed, so there
was nothing to fix in the game: the runtime simply never decoded anything.

## The fix: optional FFmpeg backend

`runtime/jstars_avplayer.c` now has two modes, chosen at build time:

- **Without `JSTARS_HAS_FFMPEG`** (default): the previous stub. Movies are
  skipped, every call is logged. No extra dependency.
- **With `-DJSTARS_HAS_FFMPEG`** (libavformat + libavcodec + libavutil): the
  source is demuxed and decoded for real. `StreamCount`/`GetStreamInfo`
  report the file's streams, `Start` begins playback (`PLAY` event),
  `GetVideoDataEx` returns NV12 frames (`width*height*3/2` bytes, pitch =
  width). Every video frame now starts on a 256-byte boundary
  (`JSTARS_AV_ALIGN`, default 256): the GPU texture base is stored shifted
  right by 8 (256 granularity), but the frame used to be requested from the
  game's `allocateTexture` with alignment 32, so a frame could start
  +32/+64/... bytes into a 256 block and be sampled shifted, with the end of
  the previous row wrapping to the left edge. If the game allocator ignores
  the 256 request, the backend over-allocates and aligns inside the block;
  `SYSALLOC=1` uses aligned host memory. The first-frame log reports the
  buffer address mod 256 to verify. `GetAudioData` returns S16 PCM frames.
  `IsActive` stays true until the end of the stream (then the movie ends
  normally), `Pause`/`Resume`/`JumpToTime`/looping work, and `Close` frees
  everything.

The Sony ABI layouts (InitData 120 bytes, StreamInfo/FrameInfo 40 bytes,
FrameInfoEx 104 bytes, …) are packed structs matching the PS4 SDK, following
fpPS4's open-source `ps4_libsceavplayer` as the reference implementation
(demux → per-stream packet queues → `send_packet`/`receive_frame`, audio
clock A/V sync, game-allocator-first frame buffers).

Supported inputs: anything FFmpeg demuxes/decodes with a YUV420P video track
(H.264 in MP4, which is what this game's movies are) and FLTP/S16/S16P/FLT
audio (AAC). Sources resolve through the normal `/app0` mapping; if the game
serves the bytes itself through its file-replacement callbacks (packed
mounts), they are dumped to `<user-dir>/avplayer_dump/` once and decoded
from there.

Not implemented: timed text/subtitles (reported as unknown streams), HTTP
streaming (`AddSourceEx` handles local-file URIs only), trick speeds.

## Building with videos

### Windows, one click

```powershell
powershell -ExecutionPolicy Bypass -File COMPILAR_WINDOWS.ps1 -ConVideos
```

The first run downloads the FFmpeg shared build (~85 MB, BtbN
`win64-lgpl-shared`, cached next to the toolchain) and compiles the runtime
with movie decoding; the FFmpeg DLLs are copied next to the `.exe` (they must
stay there, otherwise the `.exe` does not start). Without `-ConVideos` the
build is exactly as before and movies are skipped.

### Linux native (dev harness / self-test)

```sh
sudo apt install libavformat-dev libavcodec-dev libavutil-dev pkg-config
make -C runtime JSTARS_FFMPEG=1 jstars-boot
```

### Release cross-build (Windows GPU runtime on Linux)

```sh
export FFMPEG_WIN_DIR=/path/to/ffmpeg-master-latest-win64-lgpl-shared
tools/build_release.sh <libjstars_gpu.a>
```

The release folder then contains `runtime/*.dll` (FFmpeg) plus
`LICENSES/FFmpeg-LICENSE.txt`. `FFMPEG_WIN_DIR` unset = previous behavior.

## Presentation experiments (env overrides)

If a movie decodes (frames arrive, audio plays) but looks wrong on screen
(shifted, wrong aspect), the display side can be probed without rebuilding —
all three are read at runtime and logged on the first frame:

- `JSTARS_AV_SYSALLOC=1`: never use the game's `allocateTexture`; always own
  aligned memory. Tells a game-allocator texture path apart from sampling/rect math.
- `JSTARS_AV_ALIGN=N`: frame-buffer base alignment (default 256, must be a
  power of two). Only needed if a future GPU path needs something else.
- `JSTARS_AV_PITCH=N`: pack rows with stride N and report pitch N
  (default N = width). The BMP dumps follow the same stride.
- `JSTARS_AV_ASPECT=F`: report aspect F (StreamInfo + FrameInfoEx) instead of
  width/height.
- `JSTARS_AV_XOFF=N`: rotate each row by N pixels (positive = content moves
  right). Workaround when the game presents the picture shifted horizontally:
  try values until the seam disappears (e.g. ±16, ±32, ±64, ±128). The BMP
  dumps show the shifted buffer, so a correct value also looks shifted in the
  BMP but straight on screen.
- `JSTARS_AV_YOFF=N`: rotate the picture by N lines (positive = down).
- `JSTARS_AV_SWAPUV=1`: swap U/V bytes (test for chroma-swap tint).
- `JSTARS_AV_TEST=red|black|white|bars`: replace the picture with a solid
  color or vertical grayscale bars. Bypass/pattern test: if the screen does
  NOT turn red with `TEST=red`, the picture on screen is not coming from this
  backend at all (another player/middleware). `bars` + `XOFF` lets you
  straighten a known pattern live.

File fallback (same values, no console needed): create text files in
`<user>/avplayer_dump/` named `SYSALLOC.txt`, `PITCH.txt`, `ASPECT.txt`,
`XOFF.txt`, `YOFF.txt`, `SWAPUV.txt`, `TEST.txt`, `ALIGN.txt`, each containing the value
(e.g. `XOFF.txt` with `512`, `TEST.txt` with `red`). Useful if the launcher
does not pass environment variables to the game process.

A stale `.exe` shows a first-frame line WITHOUT the trailing fields: rebuild.
The current line ends with `xoff N yoff N swapuv N test N`.

On Windows set them in the console you launch the game from, e.g.
`set JSTARS_AV_SYSALLOC=1 && JStarsReborn.exe`. The first-frame log line shows
the effective `pitch / aspect / alloc` values.

## Verifying

`--selftest-host` covers the player state machine in both modes (Sony ABI
sizes, handle validation, no-media paths; with FFmpeg, the no-media errors).

With a movie build, the log of a real movie looks like this (and **not**
like the old `NO DECODER` lines):

```
sceAvPlayerInit(init data ...): FFmpeg backend (...)
sceAvPlayer: FFmpeg backend: version ... (avformat .., avcodec .., avutil ..)
sceAvPlayerAddSource('...mp4'): FFmpeg opened '...', ... bytes ...: 2 stream(s), duration .. s
sceAvPlayer: event BUFFERING (5) ... / event READY (2) ...
sceAvPlayerGetStreamInfo(0): video 1280x720 ...
sceAvPlayerGetVideoDataEx('...'): first frame 1280x720
```

If a movie still misbehaves, report: the `AddSource` path, the
`StreamInfo` lines, and whether the log shows `unsupported video/audio
format` warnings. Movies the game never requests (no `AddSource` line) are a
game-flow question, not a decoder question.

## Acceptance status

Real on-hardware movie playback (this game, full story flow) has **not** been
verified yet: it needs an FFmpeg build on a Windows machine with the full
game folder, played until a cutscene. Until then the backend is
code-reviewed + ABI-asserted, and the default build is unchanged.
