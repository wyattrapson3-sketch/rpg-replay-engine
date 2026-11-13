# RPG Replay Engine

vMix-style instant replay for OBS Studio, by Rapson Production Group.

Records up to 4 camera sources at once (each with its own audio), and plays any moment back inside OBS at 100%, 50% or 25% speed, controlled from a Stream Deck through Bitfocus Companion or from the built-in control page.

**Status:** phase 1a (record, replay, slow motion, live). Clips, the clip library and the Stream Deck page come in phase 1c. See the design spec for the full plan.

## Requirements

- Windows 10/11 x64
- OBS Studio 30.1 or newer
- NVIDIA GPU recommended (uses NVENC; falls back to x264 on the CPU)
- A fast drive for recordings (NVMe recommended; about 90 GB per hour for 4 cameras at 50 Mbps)

## Install

1. Close OBS.
2. Download the latest Windows build (`rpg-replay-engine-…-windows-x64.zip`) from this repository's **Actions** or **Releases** page.
3. Unzip it into `C:\ProgramData\obs-studio\plugins\` so you end up with
   `C:\ProgramData\obs-studio\plugins\rpg-replay-engine\bin\64bit\rpg-replay-engine.dll`
   and `…\rpg-replay-engine\data\`.
4. Start OBS. The log (Help → Log Files → View Current Log) shows `RPG Replay Engine loaded`.

## First replay

1. **Open the control page.** In OBS: **Tools → RPG Replay Engine: Open Control Page**, or browse to <http://127.0.0.1:8765/>.
   Tip: add it as a dock with **Docks → Custom Browser Docks**, URL `http://127.0.0.1:8765/`.
2. **Assign cameras.** Open **Setup**, choose an OBS source for Camera 1 (capture card, NDI, OMT, SRT, anything with video), set the recording folder to your NVMe drive, and click **Save setup**.
   If the source has a "Deactivate when not showing" option, turn it off so it keeps sending video.
3. **Add the player.** In your Replay scene, add a source of type **RPG Replay Player**.
4. **Record.** Click **REC**. Each camera records to `<folder>\<date_time>\camN.ts`.
5. **Replay.** Click **PAUSE**, step back with **−1 s** / **−1 F**, pick **25%** or **50%**, press **PLAY**, and take your Replay scene to air in OBS. **LIVE** jumps back to the newest moment.

### Slow-motion audio

The **AUDIO** button cycles through three modes (same choices as JoeReplay):

| Mode | Below 100% speed |
| --- | --- |
| Follow speed (default) | audio slows with the video and the pitch drops |
| Preserve pitch | audio slows but keeps normal pitch |
| Mute | silent |

At 100% the camera's own audio always plays normally.

## Bitfocus Companion

Use Companion's **Generic HTTP** module. For each button add an action **POST** with the URL below (no body needed).

| Button | URL |
| --- | --- |
| REC (toggle) | `http://127.0.0.1:8765/api/record/toggle` |
| REC start / stop | `…/api/record/start` · `…/api/record/stop` |
| LIVE | `…/api/live` |
| PLAY / PAUSE | `…/api/play` · `…/api/pause` |
| CAM 1–4 | `…/api/camera/1` … `…/api/camera/4` |
| 25% / 50% / 100% | `…/api/speed/25` · `…/api/speed/50` · `…/api/speed/100` |
| AUDIO MODE | `…/api/audio-mode/next` (or `follow`, `preserve`, `mute`) |
| −1 / +1 frame | `…/api/step/frames/-1` · `…/api/step/frames/1` |
| −1 / +1 second | `…/api/step/seconds/-1` · `…/api/step/seconds/1` |

`GET /api/status` returns the current state as JSON.

The API only listens on `127.0.0.1` (this computer), only accepts commands as POST, and rejects requests from other websites.

## Building

The GitHub Actions workflow builds the Windows plugin on every push to `main`. To build locally on Windows, install Visual Studio 2022 and CMake 3.28+, then:

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Built against OBS Studio 30.1.2 so it loads in OBS 30.1 and newer.

## License

GPL-2.0 (see `LICENSE`). Includes [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT, `deps/httplib-LICENSE`).
