# SubGHz Live Scope

Live raw-pulse oscilloscope + RSSI strip for SubGHz (CC1101), with automatic
band hopping across the hopper frequency list from `setting_user`. The app
parks on a frequency while RSSI stays above threshold (something is
transmitting) and resumes hopping once it's quiet again.

**Receive only — this app never transmits.**

## Controls
- **OK** — pause/resume hopping (park on the current frequency to inspect it)
- **Left / Right** — step frequency manually, one at a time (only while paused)
- **Back** — exit

## Display
- Top line: current frequency, live RSSI in dBm, and status (`scan` / `SIG` / `HOLD`)
- Upper strip: scrolling RSSI history
- Lower strip: scrolling pulse-edge activity (how busy the air is on this
  frequency — not a decoded waveform, just edge density per time slice)

## Build

```
./buildFap.sh applications_user/subghz_live_scope
```
(copy this whole folder into `applications_user/` first, same as any other
user app on this port — see the main repo's `applications_user/README.md`)

## API surface used (verify against your checkout)

This was written against source you provided from this repo
(`subghz_txrx.c`/`.h`, `subghz_read_raw.c`/`.h`, `subghz_worker.h`,
`radio_device_loader.c`/`.h`) rather than from a full local build, so please
compile and check these specifically before trusting it on hardware:

- `subghz_setting_get_preset_data_by_name()`, `subghz_setting_get_default_frequency()`,
  `subghz_setting_get_hopper_frequency_count()`, `subghz_setting_get_hopper_frequency()`
  — all used exactly as in `subghz_txrx.c`'s hopper logic.
- `subghz_devices_*` (`init`, `deinit`, `reset`, `idle`, `load_preset`,
  `set_frequency`, `flush_rx`, `start_async_rx`, `stop_async_rx`, `get_rssi`)
  — same call shapes as in `subghz_txrx_begin`/`subghz_txrx_rx`/`subghz_txrx_hopper_update`.
- `subghz_worker_rx_callback` as the low-level callback handed to
  `subghz_devices_start_async_rx`, with our own callback registered via
  `subghz_worker_set_pair_callback` — same pattern as `subghz_txrx_alloc`.
- `radio_device_loader_set/end` — copied as-is from the helper you provided
  (bundled in `helpers/` so the app is self-contained; delete it if your repo
  already has it in `components/` or similar and adjust the include path
  instead).

If any of these names or signatures have drifted in your checkout (this is a
fast-moving fork), the compiler will point at the exact line — it should be a
quick fix rather than a redesign.

## Known simplifications
- Always uses the `AM650` preset (OOK, 650 kHz) — same default as the stock
  SubGHz app. If you want FM/other presets, swap `PRESET_NAME` or add a menu.
- The "activity" trace counts pulse edges per ~60 ms tick bucket, not actual
  pulse widths — it tells you *how busy* the channel is, not what the
  waveform looks like. A true oscilloscope-style per-pulse trace (like
  Sub-GHz RAW Edit's post-capture view) would need a much deeper sample
  buffer and a different draw strategy; this is deliberately the lightweight
  live version.
- RSSI thresholds (`RSSI_STAY_THRESHOLD`, hold time) are the same ballpark as
  the stock hopper (`-90 dBm`, ~1s hold) — tune to taste for your environment.
