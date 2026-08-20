# Changelog

## 0.9.115

- Replaced the configurable startup playlist row count with restoration of the last normal main-window size. The factory and reset size keeps a 12-row playlist, while user resizing is remembered across sessions.
- Replaced automatic character-based playlist field limiting with optional remembered column widths. User-adjusted widths are restored across sessions, while the normal adaptive playlist layout remains unchanged when the option is disabled.
- Improved realtime scheduling: RTKit fallback now works correctly on standard desktop systems, while optional direct RT permission is handled separately and clearly in Settings.
- Reworked playlist search around native GtkSearchEntry handling and removed the application-side search debounce timer.
- Restored native GTK rendering for secondary controls while keeping PCM-specific styling limited to the main player interface where needed.
- Set the minimum GUI stack to GTK 3.16 and GLib/GIO 2.52. GTK 3.22+ uses the current GdkMonitor work-area API while GTK 3.16–3.20 retains the compatible GdkScreen path.
- Improved AppImage realtime handling: persistent capability grant/revoke is disabled in the read-only AppImage environment while runtime RTKit support remains available.
- Hardened CUE and M3U/M3U8 loading with bounded reads and invalid local-path checks.
- Improved FLAC bit-perfect diagnostics with direct subprocess execution, cancellation and temporary-file cleanup.
- Improved MPRIS registration, GTK widget cleanup and several dialog, sizing and compiler-warning edge cases.
- Updated project documentation.

## 0.9.114

- Reworked playback around an event-driven model. Removed periodic transport polling and reduced unnecessary GUI wakeups.
- Defined the library compatibility baseline: FFmpeg 4.4 or newer and libFLAC 1.3.3 or newer. Added public-API compatibility paths across supported FFmpeg versions.
- Added verified separate-file gapless support for AIFF/AIF PCM, AU/SND PCM, CAF LPCM and TTA.
- Added strict AAC-LC/MOV boundary verification. Ambiguous or inconsistent files remain on the normal non-gapless playback path.
- Added optional character-based width limits for Artist, Title, Album and Source. Full metadata remains available for search, sorting and MPRIS. Default limit: 25 characters.
- Added exact decoded-span handling for DSF and raw DFF DSD while keeping separate-file DSD gapless disabled.
- Improved FLAC error handling and exact-range seeking.
- Added automatic SoXr → FFmpeg SWR fallback and runtime resampler reporting.
- Improved CUE and gapless transition handling under the new event model.
- Improved Search window resizing and preference saving; removed redundant timers.
- Added runtime library and realtime-service information to About.
- Removed the obsolete experimental --file console playback path and cleaned up unused code.

## 0.9.113

- Replace FFmpeg and FFprobe command-line processes with direct FFmpeg API integration for playback, seeking and metadata probing; building now requires the FFmpeg development libraries listed in README.
- Rework gapless playback around verified sample boundaries and trusted decoder completion; retain `RangeLimitedDecoder` for known ranges, improve CUE and resampled transitions, and remove silence keepalive.
- Add playlist sorting by track number, artist, title, album and source, with ascending, descending and original-order states while preserving playback and selection.
- Rework in-process metadata probing with native fast paths for FLAC, MP3 and PCM WAV/BWF, verified sample-count paths for WavPack, APE, TAK, PCM Wave64 and ALAC, a bounded presentation-origin probe for Ogg Vorbis, and bounded validation for raw ADTS AAC. Cache results and use libavformat for remaining formats and ambiguous inputs.
- Add a unified MODE control for Repeat, Random and Random + Repeat; align navigation, history and availability across the player, media keys and MPRIS.
- Rework level-meter updates with lock-free peak delivery and time-based ballistics; reduce unnecessary GUI polling while preserving transport responsiveness.
- Add optional restoration of the last active track and configurable startup playlist height adapted to GTK metrics and the available work area.
- Preserve native multichannel playback and bypass stereo tonal processing above two channels while retaining soft volume, level metering and clip detection.
- Improve ALSA setup, fallback negotiation, recovery and diagnostics; keep playback state unpublished until output initialization succeeds.
- Improve MPRIS track identity, stopped-state selection, seeking, track advancement and state-notification consistency.
- Improve playlist filtering, metadata updates and interface refresh efficiency.
- Derive the Lossless indicator from probed codec properties, including correct handling of WavPack hybrid streams.
- Improve cancellation, error recovery and resource ownership; remove obsolete code and unused state.

## 0.9.112

- Initial playlist filter implementation contributed by [@loki1368](https://github.com/loki1368).
- Further filter integration, optimization, bug fixes and release polishing.
- Add Album metadata to FLAC, FFprobe, CUE and WAV/BWF probes, including RIFF INFO and embedded-ID3 fallback; publish Album through MPRIS `xesam:album`.
- Parse WAV RIFF INFO Artist, Title, Album and `ITRK`/`IPRT` track numbers through the fast metadata path.
- Add drag-and-drop opening of local files, CUE, M3U/M3U8 and directories.
- Add right-click directory selection to the existing Open button.
- Refine playlist layout and sizing.
- Improve the settings-saving algorithm.
- Release the playlist model reference when the main window is destroyed and tie DSP size groups to the dialog lifetime.
- Improve alignment of GUI display elements.

## 0.9.111

- Improve lazy metadata loading performance and early playback, based on work contributed by [@loki1368](https://github.com/loki1368).
- Refine pending playback and metadata finalization for reliable playback while metadata is still loading.
- Add opening of local files and directories from Linux file managers and positional command-line arguments.
- Route file-open requests to the existing application instance.
- Add asynchronous recursive directory scanning with cancellation, natural ordering and duplicate suppression.
- Register supported audio, CUE, M3U/M3U8 and directory MIME types.
- Add multi-file CUE playback across all supported audio formats.
- Prefer valid single-file CUE sheets during directory scans while avoiding duplicate split-file and playlist entries.
- Preserve opened directories as top-level sources for startup restoration.
- Restore vertical soft-volume control expansion.

## 0.9.110

- Added DSF, DFF and DST playback with configurable DSD-to-PCM conversion up to 1.536 MHz.
- Added W64, MP2, M4R, AC3, SPX, VOC, RA and DTS support through the existing FFmpeg playback path.
- Added asynchronous metadata loading with progress reporting, cancellation, timeouts and safe playlist replacement.
- Improved metadata handling for container and audio-stream tags, UTF-8 and Windows-1251 text.
- Corrected high-resolution CUE timing and refined codec-based gapless playback.
- Added optional restoration of previously opened sources.
- Added unified ALSA buffer targets and negotiated-value diagnostics.
- Added Linux desktop integration with application icons and desktop entry.
- Standardized application dialogs while preserving native GTK controls.
- Further optimization, bug fixes, and polishing for MPRIS integration.
- Initial MPRIS implementation contributed by [@loki1368](https://github.com/loki1368).

## 0.9.109

- Added persistent realtime permission controls using pkexec/setcap cap_sys_nice.
- Added revoke control for persistent realtime permission.
- Added direct SCHED_RR request before RTKit when permissions allow it.
- Wrapped realtime status text to avoid Settings window expansion.
- Kept realtime priority disabled by default and audio path unchanged.

## 0.9.108

- Improved RTKit realtime status reporting and verification.
- Clamped RTKit requests to the service-reported maximum realtime priority.
- Applied RTKit to the current playback thread when the option is enabled during playback.
- Restyled the ALSA probe table for readability, with green OK and red unsupported markers.
- Kept realtime priority disabled by default.
- Kept 24-bit ALSA container preference and audio path unchanged.

## 0.9.106

- Removed the memory lock option.
- Moved Active ALSA output report to DSP Studio → Diagnostics / Tests.
- Added selected ALSA device probe for common PCM containers and sample rates.
- Improved realtime audio-thread priority status.
- Kept 24-bit ALSA container preference in Settings.

## 0.9.104

- Restored the classic scalar ALSA write conversion path.
- Removed SIMD PCM conversion and benchmark code.
- Added FFmpeg-backed playback for AU/SND, BWF, CAF, TAK, TTA, WMA/ASF/XWMA, ATRAC OMA/AA3/AT3, MPC and DSF.
- Fixed GTK3 maximize / restore layout behavior.

## 0.9.103

- Added local M3U / M3U8 playlist import.
- Added AAC, OGG and OPUS playback through FFmpeg.
- Improved raw ADTS AAC duration and seek handling.
- Improved ALAC-in-M4A probing, duration fallback and seek startup.
- Fixed forced FFmpeg conversion command building to use explicit SoXr / aresample filtering.
- Recalibrated Deep Bass Amount to -1 / 0 / +1.
- Fixed unwanted autoplay after opening new files.
- Added progress animation toggle and log-folder selection.
- Removed the SIMD usage counter from the UI.
- Reused the FFmpeg read buffer and reduced non-error log flushing.
- Fixed small DSP Studio dialog-data leaks.

## 0.9.94

- Restored the older playback-entry transport behavior for all tracks with known sample ranges.
- Applied the same transport rule inside same-format gapless chains.
- Left ALSA, DSP, Deep Bass tuning, SIMD PCM conversion, Processing Rules, CUE playback and decoder selection unchanged.

## 0.9.91

- Added AIFF / AIF playback support through FFmpeg.
- Fixed FFmpeg / FFprobe handling of paths containing apostrophes.
- Improved FFmpeg / FFprobe diagnostics.
- Reduced FFmpeg-backed playlist add latency.
- Added session-only external metadata caching.
- Improved FFmpeg-backed CUE startup and seeking.
- Improved APE + CUE seeking and track changes.
- Added case-insensitive CUE audio-file matching.
- Added faster WAV header probing with FFprobe fallback.
- Added continuous CUE image playback.
- Added same-format gapless playback for compatible playlist files.
- Added codec-aware M4A handling for ALAC and AAC.
- Removed the inactive libFLAC threaded decoder option.
- Added Deep Bass Amount control.
- Scoped SIMD acceleration to final PCM/S16 output conversion.
- Renamed SIMD UI and diagnostics wording to “SIMD PCM conversion”.
- Updated the SIMD benchmark.
- Set the FLAC bit-perfect test default length to 30 seconds.
- Added AppImage notes.
- Refined Settings layout and Processing Rules UI.
- Improved same-format gapless transitions for FFmpeg-backed files.
- Added a silence keepalive fallback when gapless prebuffering is late.

## 0.9.81

- Neutralized GTK theme-dependent accent colors.

## 0.9.80

- Replaced Unicode transport glyphs with GTK symbolic icons.

## 0.9.79

- Improved compatibility with older GLib / GTK 3 environments.

## 0.9.78

- Updated project descriptions and contact information.

## 0.9.77

- Expanded DSP Studio with Bass / Treble, tone graph, Deep Bass, shelf profiles, soft volume and Pre-EQ Headroom.
- Added Processing Rules for optional SoXr resampling and bit-depth conversion.
- Improved diagnostics and playback path reporting.
- Reduced GUI overhead with tiered refresh timing.
- Added defensive UTF-8 sanitizing for GTK / Pango strings.
- Preserved clean DSP bypass when all DSP controls are neutral.

## 0.9.45

- Replaced the two headroom selectors with a single Pre-EQ Headroom control.
- Added automatic headroom calculation from Bass / Treble.
- Reset manual headroom adjustment when Bass or Treble changes.
