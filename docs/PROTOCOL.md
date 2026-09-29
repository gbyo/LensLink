# LensLink — Wire Protocol (version 1)

The iOS app **listens** on TCP port **9979** on the device; the OBS plugin
**dials** it — over the LAN (the phone's IP, shown in the app) or through
usbmuxd (USB cable). Once connected, the app immediately sends HELLO and
the packet stream begins.

All multi-byte integers are **big-endian** (network byte order).

## Packet framing

Every packet starts with a fixed 20-byte header, immediately followed by
`payload_size` bytes of payload:

| Offset | Size | Field          | Notes                                    |
|--------|------|----------------|------------------------------------------|
| 0      | 4    | magic          | ASCII `OBSC` (0x4F 0x42 0x53 0x43)       |
| 4      | 1    | version        | `1`                                      |
| 5      | 1    | type           | see packet types below                   |
| 6      | 2    | flags          | bit 0 (`0x0001`) = keyframe (video only) |
| 8      | 8    | pts            | presentation timestamp, **nanoseconds**  |
| 16     | 4    | payload_size   | max 16 MiB (`16 * 1024 * 1024`)          |

A receiver that sees a bad magic, an unknown version, or an oversized
`payload_size` must drop the connection.

## Packet types

### 1 — HELLO
Sent once by the client right after the TCP connection is established.
Payload: UTF-8 JSON, e.g.

```json
{ "name": "Emma's iPhone", "deviceID": "B1092868-1234-5678-ABCD-0123456789AB",
  "app": "LensLink", "protocol": 1, "kind": "camera" }
```

`name` is the phone's display name — for status lines only; the user can
rename the phone at any time.

`kind` is `"camera"` (the app) or `"screen"` (the screen-mirror broadcast
extension); it lets the plugin label the source and, for `screen`, expect
system audio (packet type 10) instead of camera controls. Absent = camera.

`deviceID` (optional) is the installation's persistent identity — see
[Device identity](#device-identity). Apps older than the field omit it;
plugins older than it ignore it, like any unknown key. A value that isn't
a UUID in 8-4-4-4-12 hex form (either case; the app sends upper case) is
treated exactly as if it were absent. It is the same for the app and the
broadcast extension, so it identifies the phone, not the stream: `kind`
still says what this connection carries.

The plugin parses HELLO strictly: a syntax error, a payload over 4 KiB, or
a known key given twice makes the whole packet malformed (read as an
unnamed camera with no identity), while a wrong-typed field is ignored on
its own. A string holding an escaped `\u0000` is never cut short at the
NUL — it is treated as absent.

An optional `"standby": true` means the app is reachable but the camera
isn't running yet (the app is open and idle with its **Remote start from
OBS** option on). No video follows until the plugin sends a
`start_stream` control command (type 7). The app re-sends HELLO (without
`standby`) when the stream starts; the plugin also treats VIDEO_CONFIG as
leaving standby, so either signal suffices. Absent = a live stream
follows as usual.

### 2 — VIDEO_CONFIG
Sent after HELLO and again whenever the capture format changes.
Payload: UTF-8 JSON, e.g.

```json
{ "codec": "h264", "width": 1280, "height": 720, "fps": 30, "kind": "camera" }
```

`codec` is `"h264"` or `"hevc"` and selects the plugin's decoder; a codec
change mid-stream resets the decoder (the next keyframe re-initializes it).
`kind` mirrors the HELLO field. Dimensions/fps are informational; the
authoritative values come from the bitstream parameter sets.

An optional `"color"` field names the stream's colour mode: `"hlg"`
(10-bit HEVC, BT.2020 primaries, HLG transfer) or `"log"` (10-bit
HEVC, **Apple Log 1**: BT.2020 primaries and matrix, Apple's log curve
— the VUI carries transfer *unspecified*, because H.273 has no code
point for Apple Log; the receiver shows the flat log image untouched
for LUT grading). `"log2"` (Apple Log 2, which uses Apple-Gamut
primaries, not BT.2020) is reserved and must not be sent as `"log"`.
Absent = 8-bit SDR BT.709, which every stream was before the field
existed. For `"hlg"` the field is **informational** — the decoder
takes the authoritative colour description from the bitstream's VUI;
for `"log"` it is the *only* label (an elementary stream cannot carry
Apple Log identity), though a receiver that ignores it still renders
the flat image correctly since the VUI's matrix/primaries are real and
transfer-unspecified decodes as-is. 10-bit streams are HEVC-only (the
iPhone encoder has no 10-bit H.264) and camera-only (screen broadcast
stays 8-bit).

### 3 — VIDEO
Payload: one H.264 or HEVC **access unit in Annex B format** (start-code
delimited NAL units), per the codec announced in VIDEO_CONFIG. Keyframe
packets must set the keyframe flag and must be self-contained: parameter
sets (SPS/PPS for H.264, VPS/SPS/PPS for HEVC) are prepended before the
IDR/IRAP slice so a decoder can join mid-stream.

`pts` is the capture presentation timestamp in nanoseconds. It only needs to
be monotonic; OBS re-bases async timestamps itself.

### 4 — PING
Optional keep-alive, empty payload, sent by the app every ~2 s.
The plugin ignores it.

### 5 — TIMESYNC_REQ (plugin → app)
Empty payload; `pts` = the plugin's monotonic clock (t1, nanoseconds).
Sent about once a second while connected.

### 6 — TIMESYNC_RESP (app → plugin)
Sent immediately upon receiving a TIMESYNC_REQ. `pts` = the device's
host clock (t2) — the **same clock domain as video frame timestamps** —
and the payload is the echoed 8-byte big-endian t1.

The plugin computes, NTP-style, with t3 = its clock at receipt:
`rtt = t3 - t1`, `offset = t2 - (t1 + t3)/2` (phone clock minus plugin
clock, error ≤ rtt/2). Per-frame capture→decode latency is then
`now - (frame_pts - offset)`, logged and shown in the source status.

### 7 — CONTROL (plugin → app)
Camera remote control. Payload: UTF-8 JSON, one command per packet:

```json
{ "cmd": "zoom", "value": 2.0 }
{ "cmd": "exposure_bias", "value": -0.5 }
{ "cmd": "focus", "mode": "auto" }
{ "cmd": "focus", "mode": "locked", "lensPosition": 0.42 }
{ "cmd": "focus", "faces": true }
{ "cmd": "flashlight", "on": true }
{ "cmd": "flip" }
{ "cmd": "white_balance", "mode": "locked", "temperature": 5600 }
{ "cmd": "white_balance", "mode": "auto" }
{ "cmd": "exposure", "mode": "manual", "iso": 400, "shutterSeconds": 0.004 }
{ "cmd": "exposure", "mode": "auto" }
{ "cmd": "start_stream" }
{ "cmd": "stop_stream" }
{ "cmd": "pause_stream" }
{ "cmd": "resume_stream" }
{ "cmd": "set_format", "resolution": "1080p", "fps": 60, "codec": "hevc" }
{ "cmd": "set_quality", "quality": "maximum" }
{ "cmd": "mic", "id": "builtin:2" }
{ "cmd": "tally", "program": true, "preview": false, "sync": "locked" }
{ "cmd": "green_screen", "on": true, "maxDistance": 2.5 }
{ "cmd": "identify", "host": "Studio-Mac", "obs": "32.0.1", "transport": "usb" }
```

`set_format` switches the capture format mid-stream; any subset of its
fields may be present. The app validates the combination against the
active lens (the STATE snapshot advertises the valid choices in
`resolutions` / `frameRates` / `codecs`, plus the current `resolution` /
`fps` / `codec`) and ignores unsupported requests. A format change flows
through the normal live-reconfigure path: new VIDEO_CONFIG, fresh
keyframe, decoder reset on a codec change.

Screen-mirror connections honour only `set_format`'s `fps`, 60 or 30. The
plugin's LensLink Screen source sends `{ "cmd": "set_format", "fps": 30 }`
on every new connection and whenever its **Frame rate** property changes
(the same on-change + re-announce contract as `tally`). The phone keeps
capturing at the display's rate and skips frames down to the requested
rate before encoding, scaling the bitrate with it, and announces the new
rate in the next VIDEO_CONFIG. An older app ignores it and sends 60.

`set_quality` picks the bitrate strategy: `"balanced"` (the app's table,
safe on ordinary Wi-Fi, the adaptive loop only backs off from it) or
`"maximum"` (starts higher and probes upward while the link is clean, to
a transport-set ceiling, with the encoder's quality-first settings). The
STATE snapshot reports it as `quality`. A live stream rebuilds its
encoder on the change, like a format change.

`mic` selects which microphone feeds the phone-mic capture, hot-switchable
mid-stream. Ids come from the STATE snapshot's `mics` list: `"auto"`
(system routing), `"builtin:<dataSourceID>"` (a physical phone mic —
Bottom/Front/Back), or `"port:<uid>"` (an external input). The app
validates the id against the live list and ignores stale ones (e.g. a
Bluetooth mic that just disconnected).

`tally` drives the app's on-air light and its lip-sync readout. `program`
is true while the source is part of what OBS is actually sending out;
`preview` is true when it's visible somewhere else (preview, a projector)
but not on air — the two are mutually exclusive, and both false means
neither. `sync` reports auto lip-sync calibration as one of `"off"`,
`"measuring"`, `"locked"` or `"relocking"` (see docs/UI_DESIGN.md for the
words and colours each maps to).

Unlike the camera commands, `tally` is sent **on change** rather than on
request, and re-announced on every new connection — a device is never
assumed to remember what it was last told. The app clears the light
whenever the connection drops, so a stale "live" can't outlive the OBS
that set it. Screen-mirror connections receive it too and ignore it: the
broadcast extension has no UI.

`focus` takes any subset of its fields: `mode` (`"auto"` / `"locked"`)
with an optional `lensPosition`, and `faces` — whether the camera should
keep focus and exposure on the faces it sees while on auto (the STATE
snapshot reports it as `faceFocus`, and `supportsFaceFocus` says whether
this camera can). Absent fields are left as they are.

`identify` is the plugin introducing itself, sent once per connection
right after HELLO: `host` is the computer's host name (any domain tail
stripped), `obs` the OBS Studio version, `transport` `"usb"` or `"lan"`
for how the plugin dialed. The app uses it to name the computer on its
Home screen; it forgets the values when the connection drops. Fields may be empty strings where the plugin couldn't
learn them, and an older app ignores the command.

`reference` gates the lip-sync reference (packet type 9):

```json
{ "cmd": "reference", "on": false }
```

The reference costs the phone a live mic capture and ~256 kbit/s for as
long as it runs, but once the plugin's calibration has locked the mic
latency it has nothing left to learn — so the plugin sends `on: false`
after locking, and `on: true` while measuring/relocking and briefly
around each periodic re-verification. Same on-change + re-announce
contract as `tally`. The app only ever honours it *within* the user's own
settings: `on: true` never starts a capture whose **Auto lip-sync
reference** toggle is off, and `on: false` never touches a mic capture
serving as source audio (type 10). An older app ignores the command and
streams continuously, which the plugin handles fine; a plugin that never
sends it gets today's always-on behaviour.

`green_screen` toggles the virtual green screen and its depth cutoff;
either field may appear alone. `on` arms/disarms the segmentation +
green composite (the compositing happens **on the phone, before
encoding** — the wire carries ordinary video whose background happens
to be chroma green, so receivers need no new decode behaviour).
`maxDistance` (metres; `0` = no cutoff, otherwise 0.5–5.0) drives the
depth-assisted subject cutoff and is meaningful only while depth
assist is active — the app clamps and ignores as needed. Green screen
is SDR-only: arming it forces the Standard colour pipeline, and the
STATE fields below keep every surface honest about what's running.
Camera connections only; screen mirror ignores it.

Unknown commands are ignored, so new ones can be added compatibly. The
plugin's embedded web panel (http://localhost:9980) generates these.

`pause_stream` / `resume_stream` **hold** a running stream instead of
ending it: the app keeps the connection, the camera and the encoder, and
stops sending the camera's frames. In their place it draws a **held
picture** — the last frame blurred to a 64×36 thumbnail and back, in
grey, with a pause glyph — and sends that as ordinary VIDEO, forced to a
keyframe, about once a second for as long as the pause lasts. Repeating
it is what covers a source shown again mid-pause or a plugin that
reconnects; a static frame costs tens of bytes. Drawing it here rather
than in the plugin is deliberate: as video it reaches every decode
pipeline, where a plugin-side drawing could only ever work for the
standard one (the GPU pipeline keeps frames in textures). Audio is unaffected — a phone acting as the
wireless mic keeps carrying the show. Resuming asks the encoder for a
keyframe, so the plugin has something self-contained to restart decoding
on. Unlike remote start these need no **Remote start from OBS**
permission: they can only hold a stream the user already started, never
turn a camera on.

`start_stream` / `stop_stream` are the **remote start** commands: they
start/stop the camera itself (not just the connection) and are honoured
only while the app's **Remote start from OBS** option is on. The plugin
sends `start_stream` when the user clicks **Start camera on the phone**
(source properties or web panel), or automatically on receiving a standby
HELLO when its **auto-start** option is enabled — but only if the app was
previously unreachable (just opened/foregrounded), so stopping the stream
on the phone doesn't bounce straight back into streaming. With
**Disconnect when this source isn't shown anywhere** plus auto-start, the
plugin sends `stop_stream` before dropping the connection on hide and
`start_stream` again on show.

### 8 — STATE (app → plugin)
Camera-state snapshot, sent (debounced ~200 ms) whenever a control value
changes and once on connect. While the app is idle in standby it sends a
**telemetry-only** snapshot instead (see "Telemetry" below) — just the
phone-health keys, none of the camera controls — once on connect and
again whenever a telemetry value changes. Payload: UTF-8 JSON, e.g.

```json
{ "zoom": 2.5, "maxZoom": 10, "exposureBias": -0.5,
  "focusMode": "locked", "lensPosition": 0.4,
  "faceFocus": true, "supportsFaceFocus": true,
  "quality": "balanced",
  "flashlight": true, "hasFlashlight": true, "camera": "back" }
```

The plugin caches the latest snapshot and serves it at `/api/state` so
remote UIs mirror the app (and vice versa) regardless of where a change
was made.

The snapshot also carries white-balance and manual-exposure state
(`whiteBalanceMode`/`whiteBalanceTemperature`, `exposureMode`/`iso`/
`shutterSeconds` with their ranges, plus `supportsWhiteBalanceLock` and
`supportsManualExposure` so UIs hide what the camera lacks) and the
capture format (`resolution`/`fps`/`codec` with `resolutions`/
`frameRates`/`codecs` capability lists for `set_format` pickers).

While a 10-bit colour pipeline is active the snapshot says so —
`"hdr": true` for HLG, `"color": "log"` for Apple Log — and the
`codecs` list shrinks to `["hevc"]`, which is how remote UIs refuse
H.264 through the ordinary `set_format` validation. Both fields are
absent on SDR streams, whose snapshots are unchanged from before
colour modes existed.

Pause rides the snapshot too, so surfaces can say *why* a picture is
held rather than only show it: `"paused": true` while video is held, with
`"pauseReason"` saying who held it — `"user"` for an operator pause (the
app's Pause button, the web panel, or the source properties) or
`"camera"` when iOS interrupted capture (another app opening the camera
beside LensLink on an iPad, say). The reason is what lets a surface choose between offering a Resume button
and explaining that the phone is waiting on iOS.

Green screen state rides the snapshot the same way:
`"supportsGreenScreen": true` advertises the feature (remote UIs gate
their row on it), `"greenScreen": true` appears while it is armed,
`"greenScreenDepth": true` while depth assist is actually running
(TrueDepth front / LiDAR rear Main lens, and a depth-capable format
matched — absent means segmentation-only; remote UIs key the distance
control off truthiness), and `"greenScreenMaxDistance"` carries the
cutoff in metres when one is set. Boolean snapshot fields are emitted
only when true: a receiver's STATE cache has finite room (older
plugins truncate silently past 1 KiB), so snapshot growth is real
cost — weigh every new field against it. The plugin also reacts to `"greenScreen": true` on a
camera source by auto-adding a pre-configured chroma-key filter —
once, by name, respecting a user's later deletion of it.

While the phone mic streams as the source's audio (packet type 10), the
snapshot additionally carries `micEnabled: true`, the selected `mic` id,
and the selectable `mics` list (`[{ "id", "name" }, …]`) for the `mic`
command's pickers. Absent otherwise, so remote UIs key their mic row off
`micEnabled`.

#### Telemetry (phone health)

The snapshot also reports the phone's power and capture health, so an
operator can see a camera dying or being throttled before the picture
suffers. Every key is optional and **absence means unknown or
unsupported** — an older app sends none of them, and a receiver must never
substitute a default (no "nominal", no "0%", no "off").

| Key | Type | Values |
|-----|------|--------|
| `batteryPercent` | integer | `0`–`100`. Omitted when iOS won't say; never `-1`. |
| `batteryState` | string | `unplugged`, `charging`, `full`, `unknown` |
| `lowPowerMode` | bool | `true` / `false` (Low Power Mode is on / off). Sent as an explicit `false`, unlike the flags above: absent means "not reported". |
| `thermalState` | string | `nominal`, `fair`, `serious`, `critical` |
| `systemPressure` | string | `nominal`, `fair`, `serious`, `critical`, `shutdown` |
| `systemPressureFactors` | array of strings | any of `systemTemperature`, `cameraTemperature`, `depthModuleTemperature`, `peakPower`, `batteryStress`. Sent only when non-empty, and only alongside `systemPressure`. |

The values are stable English tokens — never localized, never a
platform's description string; a UI translates them for display only.

**`systemPressure` is not `thermalState`.** They are different facts and
must not be merged or one used to label the other:

- `thermalState` is the *whole device's* thermal condition
  (`ProcessInfo.thermalState`).
- `systemPressure` is the *camera's* pressure
  (`AVCaptureDevice.systemPressureState`) — the signal the app's own
  mitigation already reacts to (`serious` halves the bitrate, `critical`
  quarters it and halves the frame rate; `shutdown` means iOS has stopped
  the capture device and the session is interrupted). It describes the
  active capture device, so it is absent until a camera has been
  configured. `systemPressureFactors` says *why*: the three temperature
  factors are heat; `peakPower` and `batteryStress` (the phone is about
  to shut down at the current load) are power. A `serious` pressure with
  only `peakPower` is a power problem on a cool phone, and a pressure
  with no factors at all says nothing about heat — so name it "camera
  system pressure", never "thermal", unless a temperature factor is
  present.

`lowPowerMode` is the platform's own flag, not inferred from the battery
level, and the battery level is never inferred from it: 74% with Low Power
Mode on is information, not a low battery. The facts stay separate on the
wire; how loudly to present them is the receiver's call. The plugin's own
classification (`camera-state.h`) treats *battery ≤ 20% and unplugged*,
`thermalState` or `systemPressure` at `serious` as a warning;
`thermalState` at `critical` and `systemPressure` at `critical` or
`shutdown` as a strong warning; and Low Power Mode alone as information.
The 20% is the phone's own low-battery threshold.

Receivers validate rather than repair: an out-of-range or fractional
`batteryPercent` (140 is not 100), an unknown string, or a value of the
wrong type makes *that field* unknown, never the snapshot; unknown
`systemPressureFactors` entries are ignored; factors without a
`systemPressure` are dropped. A key that appears twice takes its last
value, whatever its type. The plugin drops a connection's telemetry with
the connection.

Reporting only: none of this changes what the app captures or encodes. In
particular Low Power Mode and `thermalState` do not change quality, codec
or bitrate; only the pre-existing `systemPressure` mitigation does.

Size: the fields add about 120 bytes to a snapshot (896 vs 777 bytes for a
typical one) and at most 242 more in a worst-case pressure episode with
every factor set (1540 vs 1298 for the largest snapshot — every lens,
format, mic and green-screen key). The plugin's STATE cache is 2048 bytes;
a receiver from before it grew silently truncates past 1023 (see above),
which the *typical* snapshot still clears (1019 bytes worst case).
Measure before adding keys.

### 9 — AUDIO (app → plugin)
Reference audio for lip-sync auto-calibration. Payload: raw **16 kHz mono
signed-16-bit little-endian PCM**; `pts` = capture time of the first
sample, in the same clock domain as video frames. Sent in ~100 ms chunks
only while the app's "Auto lip-sync reference" option is on.

This audio is **never played out**. The plugin converts each chunk's pts
to its own clock (via the TIMESYNC offset), builds an amplitude envelope,
and cross-correlates it against the OBS microphone the user selected. The
correlation peak is the mic's true latency `L_mic`; the applied sync
offset is then `L_v − L_mic` (video latency minus mic latency), measured
directly with no manual entry. Low-confidence windows (silence) hold the
last value.

### 10 — SCREEN_AUDIO (app → plugin)
**Playable audio**, output as the source's audio in OBS via
`obs_source_output_audio` — unlike type 9, this is not a lip-sync
reference. Payload: raw **48 kHz stereo signed-16-bit little-endian
interleaved PCM**; `pts` = capture time of the first sample, in the same
clock domain as the video frames, so OBS keeps A/V aligned.

Two senders use it:

- the **broadcast extension** (`kind: "screen"`): the mirrored screen's
  system audio. Microphone audio is intentionally omitted there (a
  streamer mics themselves in OBS; the phone mic would double it).
- the **camera app** (`kind: "camera"`), only while its **Send phone mic
  to OBS** option is on: the phone microphone as the camera source's
  audio — the phone as a wireless mic. Mutually exclusive with the
  type-9 lip-sync reference (one mic, one role).

### 11 — DIAG (app/extension → plugin)
Optional diagnostics. Payload: a short UTF-8 text line summarising the
sender's internal counters (screen samples in, frames encoded/sent, bytes,
connection state). The plugin echoes it into the OBS log (prefixed
`[lenslink][phone]`) when the source's **Verbose diagnostics** option is on,
so both ends of the pipeline appear together — the broadcast extension has
no console of its own. Purely informational; a receiver may ignore it.

### 12 — REQUEST (app → plugin)
CONTROL's mirror image: one UTF-8 JSON command per packet, directed at
the plugin itself rather than the camera. Commands:

```json
{ "cmd": "recalibrate" }
```

`recalibrate` asks the plugin to discard its locked lip-sync mic latency
and measure afresh (the app's sync pill tap; the web panel does the same
thing via `POST /api/recalibrate`, no packet involved). Unknown commands
are ignored, so new ones stay compatible; a plugin older than this type
logs an unknown-type warning and carries on.

## Discovery (Bonjour)

Whenever the app's listener is up (streaming, standby, or a screen
broadcast), it is advertised over Bonjour as **`_lenslink._tcp`** with the
device's name as the service instance. The plugin performs a one-shot
mDNS-SD browse (RFC 6762 §5.1: PTR query from an ephemeral port with the
unicast-response bit, answers arrive unicast) — when a source's
properties open, to offer discovered phones by name in the Phone field,
and when a source bound to a device needs its current address (see
[Device identity](#device-identity)). The wire port is fixed and the
responder's source address is the phone, so SRV and A records are
ignored and no multicast group membership is required. Typing an IP
directly still works exactly as before.

The service's TXT record (RFC 6763 §6) identifies and describes the
service; it is not a telemetry channel — nothing that changes while the
service is advertised (battery, temperature, bitrate, addresses) goes
there:

| Key | Value | Meaning |
|---|---|---|
| `id` | the deviceID, e.g. `B1092868-1234-5678-ABCD-0123456789AB` | the installation's persistent identity; absent if the app has none |
| `proto` | `1` | the wire protocol version the listener speaks |
| `kind` | `camera` or `screen` | what the listener would stream (the app or the broadcast extension) |

Apps older than device identity publish no TXT keys. The plugin reads
TXT from any section of the reply, matching it to the PTR's instance by
owner name; keys are case-insensitive and, per RFC 6763 §6.4, the first
occurrence of a key wins — except `id`, which is ignored altogether if it
appears twice. An `id` that isn't a valid UUID is ignored. Goodbye
announcements (TTL 0) are not listed. The reply is untrusted input: it is
bounds-checked throughout, compression pointers are limited to 16 jumps
and 255-byte names, and display names are made printable UTF-8. A
discovered id only chooses which address to dial; the phone must still
prove it in HELLO.

## Device identity

Each LensLink installation owns one random UUID, its **deviceID**,
generated once by the app (`Sources/LensLinkIdentity.swift`) and reused
forever after. It is what lets an OBS source find *the same phone* again
after a DHCP move, a rename, a Wi-Fi reconnect, or an OBS restart.

| Concept | Scope | Changes when | Used for |
|---|---|---|---|
| deviceID | the app installation | the app is deleted and reinstalled | binding a source to a phone |
| phone name (HELLO `name`) | the phone | the user renames it | status text |
| IP address | the network | DHCP, a new network | dialing — a cache, never identity |
| `kind` | one connection | the user switches camera ↔ screen broadcast | routing to Camera/Screen sources |
| source runtime id (web API `id`) | one OBS session | every OBS launch | addressing a live source in the web API |
| OBS source name | the scene collection | the user renames the source | the production role ("Stage Left") |

The deviceID is not derived from the hardware or the network — no serial
number, MAC or IP address, advertising identifier, or
`identifierForVendor` — and it never leaves the local network: it is
announced in the Bonjour TXT record and sent to the plugin in HELLO. The
app keeps
it in a file in the App Group container it shares with the broadcast
extension, so both present the same identity; the file is excluded from
backups, so restoring a backup onto another phone does not clone it. A
build without the App Group entitlement (an unsigned or re-signed
sideload) keeps the id in the app's own storage, and its broadcast
extension then sends none rather than a second, different one.

The deviceID is an identifier, not a credential: anything on the LAN can
claim any id. It protects against *accidents* — a stale address now
belonging to another phone — not against an attacker.

### Bound sources (plugin)

A LensLink source is either **unbound** — its `device_id` setting is
empty, as for every source saved before identities existed — or
**bound** to one deviceID. An unbound source behaves exactly as before:
it dials the Phone field's address (or USB) and accepts whatever
LensLink app answers there. Nothing binds a source implicitly; binding is
an explicit operation (`lenslink_source_bind_device`, for a setup flow to
call once the user has picked a phone).

A bound source's settings hold `device_id` (authoritative) and
`last_known_host` (an address cache, never identity). Over Wi-Fi each
connection round tries, in order:

1. the address Bonjour reports for the bound id right now (a browse of up
   to 1 s that returns as soon as the phone answers);
2. `last_known_host`;
3. the Phone field's address, if the user typed one.

Whichever answers must then send a HELLO whose `deviceID` equals the bound
id. Until it does, the plugin believes nothing from the connection
(video, STATE, audio and requests are ignored) and sends it nothing but
timesync — no identify, tally or queued commands. Over USB the same check
applies to each attached phone in turn.

| Source | HELLO `deviceID` | Result |
|---|---|---|
| unbound | anything, or absent | accepted (the historical behaviour) |
| bound to A | A | accepted |
| bound to A | B | rejected: "a different LensLink phone answered" |
| bound to A | absent or malformed | rejected: the phone can't confirm it is A (an app older than device identity) |
| bound to A | no HELLO within 5 s | dropped, as unconfirmed |

A rejection never changes `device_id` and never moves the source to the
other phone. The address that answered wrongly is left alone for a minute
— the app hands its stream to its newest connection, so redialing would
keep interrupting that phone's own stream — unless Bonjour reports the
bound phone there. `last_known_host` is rewritten only after a HELLO has
proved the id (a Bonjour answer alone never moves it), and saved with the
scene collection. The search runs on the source's existing dial thread,
between reconnect attempts; there is no background discovery thread.

Two sources bound to the same phone can't both use it: the second shows
"already used by another LensLink source", as for two sources aimed at
one address.

Compatibility: an old plugin ignores `deviceID` and the TXT keys; a new
plugin with an old app works for unbound sources and refuses to treat the
unidentified app as a bound source's phone. No protocol version change is
involved — both additions are optional fields that older peers ignore.

## USB transport

The packet protocol is identical over USB. The plugin reaches the app's
listener through the usbmuxd protocol (Apple Mobile Device Service on
`localhost:27015` on Windows; the `/var/run/usbmuxd` socket on
macOS/Linux): `ListDevices` → first attached device → `Connect` with the
port in network byte order. After a successful `Connect` result the mux
socket is a raw byte pipe and the normal packet stream begins with the
app's HELLO.
