# GD Screen Recorder

A Geometry Dash Geode mod that records the game's OpenGL framebuffer through Eclipse Team's `eclipse.ffmpeg-api` v2 event API.
# WARNING : AI SLOP (yes the whole mod is made with ai)
## What is implemented

- Native framebuffer capture from a high-z `CCNode::draw()` inside `PlayLayer`.
- Exact `GL_RGBA` to `ffmpeg::PixelFormat::RGBA` data path.
- Fixed-rate frame pacing.
- Runtime-selectable FFmpeg encoder, bitrate, FPS, and output container. The
  F8 menu cycles only codecs reported by the installed FFmpeg API; `libx264`
  remains the broad MP4-player-compatible default.
- Start/stop button in the Geometry Dash pause menu.
- In-level recording indicator.
- Timestamped output in the mod save directory under `recordings/`.
- Optional post-recording stereo audio-file mux through `ffmpeg::events::AudioMixer::mixVideoAudio`.
- Optional microphone capture using FMOD's recording-driver API, with device
  enumeration and GUID selection in the F8 menu, mixed into the same MP4 as
  game audio.
- Automatic stop if framebuffer dimensions change or frame encoding fails.

## Audio

FMOD DSP tap on the master channel group captures live game audio while recording (toggle: **Record game audio**). The microphone path follows Globed's documented FMOD flow (`getRecordDriverInfo`, `recordStart`, `getRecordPosition`, and `Sound::lock`) on a worker thread (toggle: **Record microphone**). Set **Audio file override path** to mux a specific file instead; an override intentionally takes priority over live capture.

## Build

1. Install Geode SDK 5.8.2 or newer and configure `GEODE_SDK`.
2. Install `eclipse.ffmpeg-api` v2.0.1 in Geometry Dash.
3. From this source directory:

```sh
geode build
```

The generated `.geode` package appears in the build output directory.

### GitHub Actions: universal Windows + Android64 package

The included `.github/workflows/build.yml` workflow builds Win64 and Android64
on their supported runners, then combines both platform binaries into one
`pcpapc172.recorder.geode` package. Push the repository to GitHub and either
push a commit or run **Build Universal Geode Mod** manually from the Actions
tab. Download the `pcpapc172.recorder-universal` artifact when the workflow
finishes.

### Android64

The Android64 build uses Geode's Android toolchain and Eclipse FFmpeg API's
provided `arm64-v8a` libraries. The default Android encoder is
`h264_mediacodec`; the F8-equivalent **REC** touch button is placed in the
top-right overlay. Microphone capture requests Android's `RecordAudio`
permission through Geode before calling FMOD.

```sh
geode sdk install-binaries -p android64
geode build -p android64
```

Install the resulting package from `build-android64` into:

```text
/storage/emulated/0/Android/media/com.geode.launcher/game/geode/mods/
```

## Use

Press **F8** anywhere in-game to open the menu, adjust settings, and start/stop recording.

## Source grounding

- FFmpeg recorder and mixer calls use the uploaded API's `include/events.hpp` public event-based interface.
- `RenderSettings` fields and `PixelFormat::RGBA` are taken directly from the uploaded `include/render_settings.hpp`.
- The Geode dependency and settings format follow the uploaded Geode documentation.

## WSL build-agent instructions

See [`aiagent.md`](aiagent.md) for a documentation-grounded Ubuntu WSL cross-compilation procedure and strict no-guessing rules for automated coding agents.

## v1.3.1

v1.3.0 fixed the dropped-frame/audio-speed bug by having `captureFrame()` (on the render thread) deep-copy the last captured frame into `m_lastFrame` on every single captured frame, so it would have something to resubmit on a future drop. That copy - up to several MB for a full framebuffer, done synchronously on the render thread on every frame, not just dropped ones - was the source of the lag reported after testing that build.

Fixed by moving all of that duplication logic onto the encoder thread instead, and switching frame buffers from `std::vector<uint8_t>` to `std::shared_ptr<std::vector<uint8_t>>`:

- `captureFrame()` (render thread) now does nothing more on a drop than incrementing a counter and waking the encoder thread - no frame data is copied there at all, dropped or not.
- The encoder thread tracks how many capture ticks have elapsed in real time (`framesCaptured + framesDropped`, both already updated by the render thread) versus how many frames it's actually written. When it's drained its real-frame queue but is still behind that target, it re-writes its last-written frame - a `shared_ptr` copy (a refcount bump), never a pixel-data copy - to catch back up.
- The just-superseded "last frame" buffer is only returned to the reusable pool once a newer frame replaces it as the reference, so there's no aliasing between a buffer still being used for padding and one being reused for a fresh `glReadPixels` capture.

Also added:

- **A non-blocking stop with a progress popup.** `stop()` joins the encoder thread and does blocking FFmpeg audio-mux I/O, both of which can take a while for a long recording - previously this ran directly on the thread that clicked "Stop Recording" (the main/render thread), freezing the game for that whole time. Per `tutorials/async.md`'s documented pattern for blocking work ("use the blocking thread pool" + hand the result back via a main-thread callback), there's now `RecorderManager::stopAsync()`, which runs `stop()` on Arc's blocking thread pool and delivers the result back on the main thread. Clicking "Stop Recording" now immediately shows a small "Saving recording..." popup and returns control to the game; the popup closes and the usual "Recording saved" alert appears once the background work actually finishes.
- **Fixed the F8/saving-popup interaction.** The F8 keybind handler only ever looked for the settings-popup's scene-child ID to decide whether to close it or open a new one. It had no idea the new "Saving..." popup existed, so pressing F8 while it was up would spawn a *second*, redundant settings popup instead of dismissing it - and if that popup's node were removed some other way, the saving popup's "is one currently showing" bookkeeping would never get cleared, silently blocking any future one from appearing. F8 now recognizes and properly closes either popup via `SavingPopup::closeIfShowing()` (which keeps that bookkeeping in sync) rather than a generic node removal.

## v1.3.0

Fixes:

- **Dropped frames no longer speed up the video (and desync/pitch-shift the audio).** When the encoder queue was full, `captureFrame()` used to just skip the tick entirely. Since `ffmpeg-api`'s `Recorder::writeFrame()` has no timestamp/duration parameter (one call = one frame at the configured FPS), every skipped tick silently shortened the encoded video's timeline relative to how long the recording actually ran, so the file played back faster than real time whenever drops happened. `ffmpeg::events::AudioMixer::mixVideoRaw` then infers its "native" sample rate from `raw.size() / videoDuration`, so that same shortened video duration also inflated the sample-rate estimate, which is what sped up and pitch-shifted the audio. The recorder now resubmits its last successfully captured frame instead of skipping the tick when the queue is full, so the encoded frame count - and thus the video/audio duration used for the sample-rate calculation - keeps pace with real elapsed time. (See v1.3.1 above: the first version of this fix caused a lag regression, since fixed.)
- **"Saved to" text.** The popup used `<br>` for a line break; per `geometrydash/tags.md`, GD's `FLAlertLayer` text only recognizes color/delay/shake/fade tags, no line-break tag, so the literal string `<br>` was being printed. Replaced with a real `\n`.
- **Oversized menu text.** All of the small info labels (`smallLabel`) were being rendered at `bigFont.fnt` scale `.4f`, noticeably bigger than the `.35f` already used for the toggle-row labels elsewhere in the same popup. Brought everything down to a consistent `.3f`.

## v1.2

F8 opens a menu (settings + start/stop), replacing the pause-screen button. Capture moved from `PlayLayer` to `OverlayManager` (Geode's `CCDirector::m_pNotificationNode`), so it records every layer, not just gameplay. Game audio is now captured via an FMOD DSP tap on the master channel group and muxed with `mixVideoRaw`; the manual audio-file override still works and takes priority if set.

## v1.1 real-time pipeline

Version 1.1 moves `Recorder::writeFrame` to a dedicated encoder thread. The Geometry Dash render thread captures a framebuffer only when a frame is due, then places it into a bounded pool-backed queue. If the encoder cannot keep up, the recorder drops incoming recording frames instead of blocking the game or growing memory without limit.

The on-screen panel now starts hidden and contains no idle `READY` state. While recording, it shows encoded and dropped-frame counts.

This design removes FFmpeg conversion, codec work, packet retrieval, and file writes from the render thread. `glReadPixels` still has to execute on the active OpenGL context and can still cost performance, particularly at high resolutions and frame rates. Desktop recorders such as OBS can use operating-system or GPU capture APIs and hardware texture sharing that are not exposed by the supplied Geode or Eclipse FFmpeg API interfaces.
