# Android Auto on original HU_NBT: USB bring-up

> Development log of the USB/TLS/protocol bring-up (state of 2026-11-06). The video tools mentioned here
> (`Inspect-NbtVideoCapture.py`, `Test-CinemoPixels.py`, `Prepare-NbtVideoFixture.py`, `test_cinemo_*.c`) now live in
> [experiments/png-pipeline/](../experiments/png-pipeline/). For the current structure see [src/README.md](../src/README.md).

Status: 2026-11-06. User selected USB and a OnePlus 7 Pro running OxygenOS 12
(Android 12). Hello NBT menu and text page are visually confirmed on the car.
The protocol core now builds as a QNX x86 static library with an alternative
Mbed TLS backend. VM tests pass. No Android Auto receiver has been installed
on the NBT. A real USB VersionRequest/Response (version 1.7, status 0) and
verified TLS 1.2 handshake succeeded with this phone. AUTH_COMPLETE now elicits
an encrypted ServiceDiscoveryRequest, successfully decrypted on the NBT. The
capability response and video receipt have now succeeded with the real phone,
as recorded below. Native iDrive display, audio and controller remain unverified.

## Real OnePlus Android Auto dashboard captured

Two bounded physical tests on 2026-11-06 successfully negotiated USB/AOA,
verified TLS, advertised capabilities, focused video and recorded the phone's
actual H.264 stream. The initial 60-message capture showed the Android Auto
startup logo. The user then confirmed the remaining phone setup/consent.

The second capture used a 300-message ceiling (still 15 seconds and 8 MiB)
and ended with 242 media messages, one codec configuration and 256,742 bytes,
with no capture validation error. Local FFmpeg decoding showed the actual
Android Auto dashboard, including Google Maps and Spotify. This is real phone
video received through the NBT, **not a synthetic preview or an iDrive display
confirmation**. Evidence: `build/aa-phone-capture-02/vehicle-results.json`,
`capture.log`, `phone.h264`, `phone.csv`, and `decoded/last-frame.png`.
The first test remains under `build/aa-phone-capture-01/`.

The longer native binary was rebuilt from the previously tested session core
with only the capture-message ceiling increased, compiled without warnings,
self-tested, transferred and verified byte-for-byte before use. The inspector
accepts up to 300 indexed media messages accordingly. An initial shell launcher
failed before receiver startup due to Windows CRLF line endings; the successful
launchers were explicitly written with LF endings. Never upload shell scripts
using unnormalized Windows text output.

Both captures/indexes were downloaded twice and compared before cleanup. Both
90-second accessory windows expired and restored all six original handler paths.
All 18 explicitly named temporary RAM files (binaries, public test identity,
trust material, launchers, logs and captures) were removed and absence verified.
No HMI restart or display call was performed. The installed static `NBT Bildtest`
plugin remains separate; next work is getting a captured image onto the iDrive,
followed by continuous frame delivery.

## Bounded real-phone video capture prepared

`aa_video_capture.cpp` connects the hardened AASession protocol core to a
full-duplex transfer bridge. `oneplus_aoa.c --capture-video BUS DEVICE`, in the
new capture build, uses separate USB URBs and DMA buffers for IN and OUT.
The session core performs version negotiation, verified TLS, service discovery,
video setup/focus and acknowledgments. It replaces the discovery-only probe's
immediate shutdown. The existing exact-device and accessory-window guards stay
mandatory. There is no USB retry/reset, display operation or audio playback.

The diagnostic writes an exclusive `.h264` capture plus a `.csv` index with
codec/media message boundaries and PTS. Limits: 15 seconds of session bridging,
8 MiB output, 300 media messages, 2 MiB per callback; the native entry point has
a 30-second process alarm. USB operations retain their bounded completion and
abort handling. The reader is joined before its buffers/URBs are freed.
Malformed Annex-B payloads are refused. Root capture prefixes must begin with
`/fs/ram3/nbt-aa-`; existing output files are not overwritten.

The latest successful build is
`build/aa-session-20261106T014542Z-f41e2ba1/oneplus_capture`, SHA-256
`e61e5a783ff3145be111dd25fd59083919ed9cf53aa448b4ad7b58da5370605c`.
The simulator verifies actual TLS authentication, encrypted capabilities,
video setup/focus, session-specific ACK, exact codec/media bytes and PTS,
32 KiB fragmented video reassembly, and malformed-media rejection. Existing
TLS/protocol tests and 400 lifecycle cycles also pass (descriptor count 5->5).
The simulator payload is a protocol fixture, not a decodable phone video.

`scripts/Inspect-NbtVideoCapture.py` validates capture/index consistency and
decodes the first image locally with FFmpeg. Its pipeline was exercised with
the known synthetic fixture, **not an actual phone capture**.

The binary, accessory-window helper and existing TLS identity/trust files are
staged privately in vehicle RAM with prefix
`/fs/ram3/nbt-aa-capture-20261106-01-` (probe, window, cert, key, ca), all verified
by readback. Native selftest and `--tls-preflight` pass; no new USB session was
started. Evidence is under `.local/aa-capture-staging/`.
The local-only launcher `.local/run-aa-video-capture.sh` sets BMW identity,
removes secondary video descriptors and the media-audio sink (2,4,9), retaining
the protocol's guidance/microphone descriptors. Those retained descriptors do
not imply implemented audio/microphone functionality. Bluetooth routing remains
untested. The launcher still requires an active accessory window and the
verified current accessory bus/device; it is not an automatic plug-and-play app.

Next physical test: establish the bounded accessory window, switch the observed
OnePlus into AOA, run capture once, retrieve both outputs in bounded chunks,
and decode the first phone image. Native iDrive frame display is still a
separate unverified step; the installed NBT Bildtest remains static.
Remove the five exact staged RAM files after the test and the capture files
only after verifying retrieval. Do not restart HMI for this capture test.

## Original-NBT graphics investigation and failed probe, 2026-11-06

### Decoder fixture and bounded VM harness prepared

The vehicle was temporarily switched off while charging. The user subsequently
reported PuTTY working and requested continuation; a fresh managed SSH session
and read-only copies succeeded at approximately 01:54-02:00 UTC on 2026-11-06.

### Cinemo VM startup diagnosis

**Stream end and recovery follow-up:** HTTP/1.1 chunked transfer with a proper
zero-length final chunk was tested without a Content-Length header. It still
produces only 85/90 frames; the unknown-length tail issue remains unresolved.
This rules out a simple switch to chunked HTTP as the fix for this runtime.

`test_cinemo_stream.c --reconnect` now deliberately truncates the first response
after 160,000 compressed bytes and accepts a new request, without restarting
the Cinemo service. In the test the first playback emitted 25 frames before
the interruption. A fresh player session on the **same service process** then
decoded all 90 frames. Every decoded byte again matched the independent
reference. A separate earlier interruption during startup also recovered.
This establishes recovery of the VM service between player sessions, not
automatic reconnect of the unfinished Android Auto receiver.

Evidence: `build/aa-cinemo-reconnect-20261106-0232/` includes chunked and both
interruption logs, service log, updated source, decoded capture and
`results.json`. Final service termination/reaping and node cleanup passed.
No vehicle access or configuration changes were required for these tests.

**Paced streaming experiment:** `test_cinemo_stream.c` supplies the known
Annex-B clip three times through a loopback-only HTTP server, in 1024-byte
chunks paced by 10 ms sleeps. It has a 13-second alarm and bounded requests.
The VM-only supervisor adds `--vm-http-dump`; existing captures are protected
against overwrite. No vehicle connection was used for this experiment.

- With Content-Length (482,493 compressed bytes), Cinemo produces all 90
  frames, zero reported drops, CRC32 `1e0761ff`. The 51,840,000 decoded bytes
  equal three copies of the independent reference, SHA-256
  `e56bc3c55c6ef7f89e538526127e65be860c02dbf0f97883b904d8bec729c01d`.
  The first full frame is already visible in the output after 51,200 input
  bytes, well before the server finishes sending the body.
- Without Content-Length, first output is likewise observed during delivery
  (49,152 input bytes). However, only 85 of 90 frames are emitted before/after
  connection close. Pixel hashes match exactly the first 85 expected frames;
  this is a missing tail, not corruption or loss at clip repetition boundaries.
  The service reports `DownloadError(CinemoErrorEOF)`. The same behavior was
  reproduced; adding H.264 sequence/stream end markers did not resolve it.

Evidence: `build/aa-cinemo-stream-20261106-0223/` (source snapshots, logs,
captures, frame mapping and known-length pixel validation). Service shutdown,
private-node removal and unchanged absent system node were verified, and the
temporary launcher removed. Client exit 0 / reported drops 0 still do not
prove every source frame was delivered.

This establishes **incremental decode through Cinemo's HTTP input**, not an
Android Auto integration or an acceptable display-latency bound. The source is
deliberately paced below real-time frame rate. Dump-file writes are diagnostic.
An indefinite AA stream needs a defined drain/disconnect policy, explicit
timestamp handling, bounded backpressure and a frame-output API; pretending its
unknown length is a large fixed file would not resolve those requirements.

**Raw input and pixel validation update:** Both the original MP4 fixture and
its Annex-B `.h264` elementary-stream file decode to 30 frames, zero drops,
client exit 0 and Cinemo video CRC32 `e1af1832`. Adding the original
`libNmeEncoder.so` supplies `NmeDumpFilter`, required by `-dv` to capture the
decoded output. Its SHA-256 is
`e12d7105392fd3bee3c2e82718b5295f7f12cc61d53c998dae07480f7bd83674`.
Without this module the player misleadingly exits 0 after decoding zero frames;
exit status alone must never be used as the decode-success criterion.

Both captures contain 17,280,000 bytes of tightly packed 800x480 YUV420P
(30 frames). They are byte-identical to each other and to independent FFmpeg
7.1 decoding of the fixture, SHA-256
`0628a50df1567ef37065621b370401c628a79d6614e4f726b56091d2d900b7af`.
`scripts/Test-CinemoPixels.py` checks exact size and every frame's bytes,
records per-frame hashes, and rejects mismatches. One-byte corruption and
truncated-frame negative checks also passed.

Evidence: `build/aa-cinemo-raw-20261106-0216/` contains pixel validation JSON,
three raw captures, client/service logs and source snapshots. The service was
terminated and reaped; its private node disappeared and the VM system node
remained absent. The temporary root-console launcher was removed.

This proves decoding **finite local Annex-B files**, not a live input API or
continuous Android Auto stream. The dump file is a diagnostic output mechanism,
not the proposed real-time display backend. Next work must establish incremental
input, timestamps/access-unit boundaries and decoded-frame delivery before
attempting the vehicle's native display path.

**Update: software decode now succeeds in the VM.** Evidence and exact source
snapshots: `build/aa-cinemo-vm-20261106-0212/results.json`. Original Cinemo
1.24.13.28196 decoded all 30 frames of the known 800x480 baseline H.264 MP4 with
zero dropped frames and client exit 0. The service log confirms `NmeVideoSW`
loaded and playback reached EOF/FINISHED. Pixel comparison and finite Annex-B
files are now validated above; live Android Auto input remains untested. VM throughput does not measure
vehicle performance; this does not establish iDrive display or hardware decode.

`test_cinemo_foreground.c` is a **VM-only test adapter**: it suppresses
`procmgr_daemon` and maps `/dev/io-nme` to a private test node, allowing the
supervisor to retain and reap its child. QNX's daemon call otherwise detaches
the child from its parent ([QNX process documentation](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_prog/process.html)).
Non-root attachment failed even at the private node. The service was therefore
started through the already logged-in VM root console; the player stayed UID
100. The supervisor permits root only in its explicit VM service-start mode,
checks the local VM test adapter exists and refuses an existing system node.
It terminates its own service after 15 seconds (exit 124 is expected).
The successful run verified child reaping, automatic private-node removal and
that `/dev/io-nme` remained absent. No persistent VM service was installed.

Required additional copies included `libNmeUtils`, `libNmeImage`,
`libNmeSubtitle`, `libNmeRedSource`, and the audio-device dependency
`liba2i_client`; SHA-256 hashes are recorded in the result manifest.
Optional `NmeAvrcp` discovery warnings remained but did not prevent this
video-only fixture from decoding. All copies remain ignored/private.

The following describes the preceding startup failures, now resolved for this
adapted VM test:

Copied the original core libraries, class XML and selected filesystem,
navigator, audio and video modules into ignored `.local/aa-decoder-runtime/`.
The plugin manifest records source paths, sizes and SHA-256 hashes. No copied
vehicle binaries belong in the repository or distributable artifacts.

The bounded `NmeCmdLine -n -a0 -wd 0 -wi 0 -wm 0 -i` experiment ran only in the
unprivileged QNX VM and exited 255 before playback. Loader diagnostics confirmed
core library loading; a temporary VM-only diagnostic interposer confirmed that
the class XML opens successfully and exposed `NmeOpen() failed!`.

Offline disassembly of the original `libNme.so` shows `NmeOpen` calling `open`
on the resource-manager path `/dev/io-nme`. The node exists on the vehicle but
is absent in the VM. Thus the command-line player is a service client: copying
codec libraries alone is insufficient. The original `/opt/mm/bin/io-nme` was
also copied for offline inspection, but has **not** been launched in the VM or
on the vehicle. It imports `procmgr_daemon`; any future VM service harness must
account for daemonization and explicitly clean up only its own service.

The VM supervisor now checks node existence without opening it and reports
`DECODER_PREREQUISITE_MISSING` (exit 3) instead of an unexplained child failure.
Its bounded-child termination/reaping self-test still passes. Evidence:
`.local/aa-cinemo-error-trace.txt`, `.local/aa-cinemo-service-inventory.txt`,
`.local/aa-cinemo-prerequisite-test.txt`. At that stage decoding was not proven;
the subsequent controlled VM service test above resolves that prerequisite. No live
vehicle graphics/decoder executions were performed during this investigation.

### Offline H.264 input stage

`src/android_auto/h264_annexb.hpp` implements a bounded incremental Annex-B NAL
splitter. It accepts mixed three-/four-byte start codes across arbitrary chunk
boundaries, passes one NAL at a time to a callback, and has sticky failure/reset
semantics. It rejects non-Annex-B prefixes, invalid NAL headers, empty NALs,
oversized payloads/zero runs and callback failures. The callback receives a
borrowed pointer valid only during that call. Feed **only media payload after
AA message ID/timestamp removal**, never raw USB or encrypted AA framing.

The exact header/tests compiled and ran on QNX x86 with chunk sizes 1, 2, 3, 7,
31, 1024 and 65536 bytes. All produced the same 33 NALs from the known fixture,
including one SPS, one PPS and one IDR NAL. Boundary testing includes a maximum-
size NAL followed by another start code, reset after failure and sink rejection.
The normalized Annex-B stream written by the VM was fetched and decoded with
laptop FFmpeg: its 30 frame hashes exactly match the original MP4's frame hashes.
Evidence and exact source snapshots: `build/aa-h264-20261106-0102/results.json`.

This component is not wired into the live receiver yet and is not a decoder,
SPS/profile validator, frame/access-unit assembler or surface renderer. NAL
presence does not establish video readiness. End-of-stream `finish()` emits the
last buffered NAL; a streaming integration must preserve AA timing and end-of-
access-unit boundaries explicitly rather than treating every USB chunk as a frame.
No vehicle connection or Cinemo execution was used for these tests.

`scripts/Prepare-NbtVideoFixture.py` now generates a one-second, 800x480/30 fps
synthetic H.264 constrained-baseline level-3.1 video with no audio, in MP4 and
Annex-B forms. FFmpeg decoding on the laptop yields exactly 30 identical frames
from both. Commands, encoder version/hash and file hashes are recorded in
`build/video-fixture-20261105T235626Z-2ee1b5dc/results.json` with frame-MD5 logs.
This validates the fixture only; **Cinemo and vehicle decoding are not tested**.
The pinned imageio-ffmpeg 0.6.0 package is installed only in ignored
`.local/video-tools`; no software was installed in the car.

`src/android_auto/test_decoder_deadline.c` supplies a VM-only supervisor for
`./NmeCmdLine -n -a0 -wd 0 -wi 0 -wm 0 -i pattern-800x480.mp4`. It allows
15 seconds, then terminates only its own child process group, escalating after
one further second, and reaps the child. Core dumps are disabled for the child;
running this supervisor as root is refused. Its actual QNX VM selftest passed
with a child deliberately ignoring SIGTERM. Evidence:
`.local/aa-decoder-deadline-test.txt` (`DECODER_DEADLINE_OK`). No Cinemo command
has yet been executed, including in the VM.

At this point vehicle SSH disconnected and a fresh login failed; a subsequent
TCP check reported Windows error 10065 (host unreachable). VM SSH remains usable.
The actual Cinemo test is blocked on copying its runtime dependencies, notably
`/opt/mm/lib/libNme.so` (48268 bytes) and `libNmeBaseClasses.so` (1216560 bytes),
then resolving their dependencies/plugins in the VM. The initial copy attempt
was refused locally because the SSH session was already failed; it did not run
on the vehicle. The existing SSH helper caps output at 1 MiB, so the larger
library must be fetched in bounded chunks and verified before use.
Do not mark this preparation as a successful native H.264 decoder test.

### Follow-up: offline bootstrap and existing decoder path

SSH was restored with a fresh authenticated session. A subsequent read-only
check confirmed `/fs/ram3/nbt-graphics-query-20261106-0031` is absent. No further
graphics or decoder code was executed on the car. The user confirms that MP4
videos already play from a USB stick; this supports the existing video-playback
capability, but the exact codec/profile of those files has not been inspected.

Offline disassembly gives a concrete explanation for the standalone EGL crash:

- BMW's `wal::WalApplicationInterfaceImpl::initKD()` at 0xb636 calls
  `kdInitializeNV` at its PLT call site 0xb67f. The paired `destroyKD()` calls
  `kdTerminateNV`. This establishes an initialization dependency in the existing
  BMW client, not a complete safe standalone ABI contract.
- The reported fault address 0xf706 in the copied NVIDIA EGL library is
  `call dword ptr [eax + 8]`, immediately following a load of the global KD
  dispatch-table pointer. The native fault reported address 8, consistent with
  a null table pointer. `NvEglKdInit` at 0xf720 stores its argument into that
  table-pointer slot. The failed probe did not initialize KD at all.
- This strongly supports missing NVIDIA/KD bootstrap as the direct probe crash
  cause. It does not establish why the entire head unit subsequently restarted,
  nor prove that merely adding one initialization call makes a live test safe.

`Audit-NbtMediaAbi.py` now records selected bootstrap call sites via offline
disassembly as well as ELF dependencies and hashes. It successfully identified
the WAL-to-KD call in `.local/aa-media-bootstrap-report.json`. Private detailed
disassembly is `.local/aa-media-init-disassembly.txt`.

The existing decoder route is more concrete now: libNmeVideoDevice contains
NmeMsvdx, NvdecDXVA and `/dev/msvdx` paths; the `/dev/msvdx` resource-manager node
exists. It also contains several surface implementations (NVIDIA/Screen/GF
names alone do not select the active one). `NmeCmdLine`'s embedded usage text
documents `-n` (null audio/video renderers for benchmarking), `-a0`, `-wd`, `-i`,
`-ic` and video-dump options. These were extracted from the binary **without
executing it**. A null renderer does not yet prove an entirely graphics-free
startup, so verify its dependency/initialization path in the VM first.

Next validation should isolate decoding from display: a short, known H.264 MP4
fixture, null rendering and bounded runtime, initially in the VM where possible.
Then compare elementary H.264 input support and the live-stream interface.
Only after that should the original NBT's supported surface/window integration
be exercised. No AA capability manifest should advertise video solely because
MP4 playback or decoder symbols exist.

Evidence: `.local/aa-media-resume.txt`, `.local/aa-media-symbols.txt`,
`.local/aa-msvdx-metadata.txt`, copied private ELF inputs and their audit hashes.

**Do not repeat the standalone EGL probe on the vehicle.** It crashed in the
NVIDIA EGL library, followed by loss of SSH and a user-confirmed iDrive restart.
The user subsequently confirmed the normal menu returned. A causal link to the
probe is plausible but not established by the available logs; treat it as an
unsafe initialization path. No firmware, boot files or stock services were
modified or deliberately stopped. The source was removed from the current tree;
the exact failed source/binary remain under ignored
`build/graphics-inventory-20261106/` solely as failure evidence.

The command attempted to load `/lib/libEGL.so`, obtain/initialize the default
display, query strings/configuration count, then terminate its own EGL connection.
It contained no context/surface/window creation or display-mode changes. There
were no per-call progress markers, so the precise failing API is not established.
The native report names `eglReleaseThread+0x14c6`, not necessarily the actual
function at that offset. Result:
`task-20261106T003053-f93cfc099e.result.json` (SIGSEGV, PID 4767845).
The subsequent state query never opened an SSH channel and cannot establish
process state. SSH remained unreachable after the normal menu returned.
Consequently absence of `/fs/ram3/nbt-graphics-query-20261106-0031` and any crash
dump has **not** been verified. On restored SSH, inspect/remove only that exact
temporary executable and identify any matching diagnostic dump before deletion.

Read-only inventory and offline ELF analysis established:

- `/lib/libKD.so` is NVIDIA-specific: dependencies include libnvsys, libnvgpu,
  libnvblt and `/lib/libEGL.so`; exports include `kdInitializeNV`,
  `kdGetNativeDisplayNV` and display/window functions. Its initialization contract
  must be established before invoking EGL or creating any window.
- `/lib/graphics/libKD.so.1` is a different QNX implementation, referencing
  `libEGL.so.1` and `libscreen.so.1`. It must not be confused with the NVIDIA
  library or assumed to be the HMI's supported path. The earlier inventory's
  missing libscreen at selected paths was not proof of system-wide absence.
- Stock processes include screen, DisplayManagerScreen, WalServer, io-msvdx,
  io-media-cinemo, io-media-generic and mme-becker. Cinemo configuration specifies
  H.264 level 4.1 and a maximum video size of 1920x1088; these are configured
  limits, not a demonstrated Android Auto decoder interface.
- Cinemo's libNmeVideoHW exports `NmeCreateCodecNvdecHW`. Decoder ownership,
  stream input, render surfaces and audio/controller integration remain unknown.

Implemented `scripts/Audit-NbtMediaAbi.py` parses copied ELF files offline,
records hashes/dependencies/relevant exports and explicitly leaves all AA media
backends unready. It does not dlopen, execute, or contact the vehicle. Successful
report: `.local/aa-media-abi-report.json`. Other evidence:
`.local/aa-graphics-media-inventory.txt`, `.local/aa-media-runtime-details.txt`.
No new service capability response was sent. Continue with offline SDK/ABI and
existing-HMI integration analysis, not unverified standalone graphics calls.

## Real-phone authentication and discovery request, 2026-11-06 00:26 CET

The bounded `oneplus_tls --discover 0 2` probe completed version negotiation and
verified TLS, then sent exactly one 8-byte plaintext AUTH_COMPLETE control frame
with status 0. Plaintext framing for this particular control indication follows
the pinned [aasdk control-channel implementation](https://github.com/f1xpl/aasdk/blob/046b3b381595509d0939fa84b14a90978f46ff63/src/Channel/Control/ControlServiceChannel.cpp).
The phone responded with 912 USB bytes: an encrypted control frame containing
ServiceDiscoveryRequest and 877 protobuf bytes (six top-level fields). TLS
decryption succeeded and protobuf wire lengths/types were checked. Field values
were not logged, and the diagnostic does not claim full schema validation.

The probe then sent a 37-byte encrypted ShutdownRequest (quit reason) and closed;
it did not wait for a shutdown acknowledgement. No capability manifest, media
channel, projection or audio was offered. The next task is an original-NBT
capability response tied to working display/audio/input implementations.

Evidence: `.local/aa-discovery-result.txt`, including
`AA_AUTH_COMPLETE_SENT status=0 after_verified_tls=1`,
`AA_SERVICE_DISCOVERY_REQUEST_OK encrypted=1 protobuf_bytes=877 fields=6`, and
`AA_DISCOVERY_PROBE_OK shutdown_request_sent=1 shutdown_ack_checked=0 no_projection=1`.
VM build: `build/aa-session-20261105T232423Z-d53fa8e0`.
Probe SHA256: `6d62cb65accc7785f983bce353e3fdc032a25ae4ed88aab5ed9efd0c1c2f4431`.
Tests exercise the actual adapter's fragmented handshake, exactly-once auth,
encrypted discovery and shutdown, and reject plaintext, modified ciphertext,
malformed protobuf lengths and truncated frames. Prior TLS/pin/session tests and
400 lifecycle cycles also pass. The process still has a 20-second alarm and all
USB transfers retain their bounded timeouts.
The path lease released automatically; all six stock executable paths were
readable. All six temporary RAM files (including the reference key) were removed
and checked absent. Evidence: `.local/aa-discovery-restoration.txt` and
`.local/aa-discovery-cleanup.txt`.

## Full real-phone TLS handshake, 2026-11-06 00:19 CET

The bounded `oneplus_tls --tls-handshake 0 2` probe completed the handshake on
the original NBT with the OnePlus. After version 1.7/status 0, USB transfers were
322 bytes out, 2353 in, 1218 out, 57 in, all successful. Final marker:
`AA_USB_TLS12_HANDSHAKE_OK peer_verified=1 no_auth_complete=1 no_projection=1`.
Evidence: `.local/aa-tls-0018-handshake.txt`. This is a completed TLS handshake,
not an Android Auto service session or working display.

Changes enabling this result:

- `scripts/mbedtls_legacy_time.py` overlays the pinned private Mbed TLS build.
  It accepts the legacy UTCTime `YYMMDDHHMMSS+/-hhmm` form, validates fields and
  normalizes the parsed time to UTC. Signed DER/TBS bytes remain unchanged.
  This is a deliberate compatibility extension: RFC 5280 requires Zulu encoding.
  No system library, clock, expiry check or signature verification is changed.
- `test_legacy_time.c` covers the observed dates, leap days, month/year rollover,
  malformed offsets and invalid dates. `test_certificate_compat.c` confirms both
  public reference certificates retain their raw bytes and valid signatures
  against the captured issuer key, and rejects a modified TBS digest. The latter
  is signature evidence, not independent authentication of the issuer.
- The backend accepts `NBT_AA_TLS_PEER=sha256:<64 hex digits>` for this non-DNS
  protocol. It checks the leaf fingerprint during certificate verification,
  retaining chain, validity and usage checks. DNS-name validation remains the
  default for ordinary peer values. Tests reject wrong/malformed pins, a wrong
  trust chain even with the right pin, and a mismatched local certificate/key.
- `tls_usb_probe.cpp` wraps the existing TLS backend in bounded AA control frames
  over the proven USB callbacks. It handles split USB reads/coalesced frames,
  rejects unsupported AA fragmentation/invalid headers, and stops after TLS.
  It sends no AUTH_COMPLETE, service list, audio or video. The process has a
  20-second alarm; the USB-path lease still expires after 90 seconds.

The probe used the existing public NBT-AAD head-unit identity and key, loaded
explicitly from mode-600 temporary RAM files, rather than embedded in the binary.
The peer root and leaf fingerprint came from the preceding controlled USB capture
of the user's physically connected phone (first-use trust). These shared AA
credentials do not establish a unique phone or head-unit identity. They are not
a general Internet trust configuration, and no global trust store was changed.

Successful VM artifacts:
`build/aa-tls-20261105T231248Z-75f2b516` (crypto/TLS/time regression tests) and
`build/aa-session-20261105T231745Z-aaa1844d` (backend, pin checks, USB adapter,
four protocol-simulator handshakes, negative cases, and 400 lifecycle cycles with
5 file descriptors before/after). Certificate audit: `.local/aa-cert-compat-audit.txt`.
Probe SHA256: `8b110f423c01e9af2492aebe411dff6923f71371f77ddb26e247133a9e1865d2`.
The window released automatically and all six stock executable paths were readable.
All six temporary RAM files, including the reference key, were removed with absence
checks. Evidence: `.local/aa-tls-0018-restoration.txt`, `.local/aa-tls-0018-cleanup.txt`.
Next: send AUTH_COMPLETE only after this verified handshake and inspect the
phone's service-discovery request before integrating display/audio services.

## Real-phone USB milestone, 2026-11-06

The bounded `accessory_window.c` helper temporarily masks six executable paths
(aoap-switch, devc-serusb-aoap and io-fs-media, each under /opt and its /fs/sda0
alias) for 90 seconds. It prevents new stock phone handlers from claiming the
device during the test; it does not modify those files or kill stock services.
Four paths were insufficient because the MTP/PFS handler claimed the phone.
The six-path root selftest verified expiry, SIGTERM and SIGKILL restoration:
`build/aa-core-20261105T224150Z-294840bc/window-root.log`. Ordinary non-root VM
builds report ROOT_REQUIRED for this test, not a successful root runtime test.

After a fresh physical reconnect, `oneplus_aoa.c` exclusively attached to the
verified OnePlus 22d9:2764 at bus 0/device 2. GET_PROTOCOL returned 2. Sending
the six Android Auto identity strings and one START request caused re-enumeration
as 18d1:2d00. The user confirmed the phone displayed Android Auto connected.
The callback-based bulk probe then wrote 12 bytes and read 12 bytes successfully:
`AA_VERSION_RESPONSE major=1 minor=7 status=0`, followed by
`AA_USB_VERSION_OK no_tls_no_projection=1`.

The earlier bulk probe incorrectly assumed synchronous completion; its zero
byte result was inconclusive, not a protocol rejection. It was corrected to
wait for completion callbacks, with bounded waits and abort handling, and tested
only after a fresh disconnect/reconnect. Latest VM build:
`build/aa-core-20261105T224808Z-4823e5ca`; component tests and probe selftests/guards
passed. Vehicle binary SHA256:
`40f47875496c39754a61b156dac82ac005bfc380a1cf9ce6c5249f041c0656e6`.
Private evidence: `.local/aa-switch-v3.txt`, `.local/aa-version-v3.txt`.
All four test windows released automatically; all six stock paths were readable
after the last window. Saved logs and removed all nine temporary RAM test files,
with absence checks (`.local/aa-final-window-restoration.txt` and
`.local/aa-final-ram-cleanup.txt`). No stock services were killed.
Next work is a bounded real-phone TLS handshake using the tested protocol core,
with explicit identity/trust configuration, before service discovery and media.

## Real-phone TLS first flight, 2026-11-06

`oneplus_aoa --tls-inspect` now sends one fresh TLS 1.2 ClientHello after a
successful AA version exchange. It captures at most 64 KiB/eight USB transfers,
then closes without finishing TLS or sending AUTH_COMPLETE or application data.
The ClientHello is generated by `scripts/Inspect-AndroidAutoTls.py hello` on the
laptop using OS randomness; no client key is transferred for this diagnostic.
The existing 20-second alarm and exclusive-device/window guards apply.

Vehicle result: 151 bytes written (AA envelope included), 2353 bytes received,
then a USB read timeout ending capture. The complete public server flight contains
ServerHello, Certificate, ServerKeyExchange, CertificateRequest and ServerHelloDone.
TLS version is 0x0303 (1.2), cipher 0xc02f (ECDHE_RSA_WITH_AES_128_GCM_SHA256).
The phone supplied a CarService leaf and Google Automotive Link root. These are
saved as **untrusted evidence**, not installed into a trust store. The leaf is
valid 2014-07-04 through 2027-02-03; the root through 2044-06-05. This proves
TLS negotiation reached the certificate request, not authentication or encryption
of application traffic. No phone-specific identity is established by these shared
Android Auto certificates.

Private evidence: `.local/aa-tls-flight-20261106-0007.txt` and
`.local/aa-peer-20261106-0007/summary.json`. Source/binary build:
`build/aa-core-20261105T230527Z-5d165859`; all eight component tests and probe
guards passed. Four Python tests cover fresh ClientHello randomness, a real
OpenSSL server certificate flight, split/coalesced transport framing, and malformed
lengths/types. The unchanged window binary matches the earlier root-tested hash.
All six stock paths were restored and all four new RAM test files removed with
absence checks (`.local/aa-tls-window-restored.txt`, `.local/aa-tls-ram-cleanup.txt`).
The native phone address was 0:3 this time, then changed to 0:2 in accessory mode;
never assume device numbers persist across reconnects. The attempted switch at
0:2 refused without sending a control transfer because no matching device existed.

### Previous TLS compatibility issue (resolved above)

The pinned NBT-AAD head-unit certificate and the certificate in aasdk revision
046b3b381595509d0939fa84b14a90978f46ff63 both fail parsing with our Mbed TLS 3.6.7
build: **-9216, X509 date tag/value invalid**. Python cryptography independently
rejects their UTCTime encoding. Both captured phone certificates parse successfully
with the actual QNX Mbed TLS libraries. Audit: `.local/aa-cert-parser-audit.txt`.
No reference private key was uploaded to the car or used for authentication.
Resolve compatibility with the head-unit certificate's legacy date encoding while
preserving signed certificate bytes, and define explicit non-DNS peer validation
(the phone certificate has no hostname), before a full real-phone handshake.
Do not assume the simulator's lab identity or DNS name works on the phone.

Protocol references: [aasdk Cryptor at the audited revision](https://github.com/f1xpl/aasdk/blob/046b3b381595509d0939fa84b14a90978f46ff63/src/Messenger/Cryptor.cpp)
and [AARE certificate notes](https://github.com/prototux/AARE/blob/main/docs/certificates.md).
Their runtime authentication claims are reference material; the local capture and
parser audit above are the evidence for this particular phone and TLS build.

## Target and evidence

- Original 2014 F07 HU_NBT, QNX 6.5.0, x86 32-bit, HMI 3D1407.
- Working native menu plugin: see IDRIVE-HELLO.md. Keep this proven entry intact
  until a receiver can actually start and stop independently.
- Build VM: 192.168.56.10. Vehicle: 169.254.199.99.
- VM SSH is restored. Its dhcp.client had changed en0 to 192.168.56.101.
  The verified VM host key and nbtbuild key worked at that address. Stopped
  only the VM's dhcp.client and restored 192.168.56.10 at its root console.
  No boot files were changed; this runtime setting may need restoring after
  a VM restart. Builds run as nbtbuild, never as vehicle root.
- The user reconnected Ethernet on 2026-11-05. Vehicle SSH is restored with
  the existing verified host key; read-only inventory completed successfully.
  The first cached USB snapshot showed only the internal PEIKER TCB device
  (2239:f004), with no phone and no /dev/android* or /dev/oap* node.
- After the user connected the OnePlus, cached enumeration showed 18d1:2d04
  at bus 0/device 2, high speed, configuration `accessory_audiosource`.
  The stock system created `/dev/oap02` and started devc-serusb-aoap with
  `-l 20480`, plus an io-audio USB instance. No custom code initiated this
  switch. This establishes accessory transport availability, not AA startup.
  The phone offered My BMW; the user declined. This confirms the stock BMW
  accessory launch path. Declining that prompt did not release the head unit's
  own media application's connections to the accessory driver.
- Earlier vehicle snapshot 010-hmi-inventory.result.json confirms /bin/io-usb
  (EHCI/OHCI), /opt/sys/bin/vdev-medialaunch using
  /opt/sys/etc/umass-enum.cfg, /bin/screen, DisplayManagerScreen, WalServer and
  dev-ioamp-router. This establishes a USB host stack, not AOA driver support.

## Reference audit

Pinned reference: Sq00ky/Supra-iDrive6-Android-Auto commit
41975f269b065dbdbc77af9f568387cd22b9ed27, under .local/reference-nbt-aad.
Its license is PolyForm Noncommercial 1.0.0; any reused source must retain the
license and required notice, Copyright (c) 2026 nanaisu. Selected protocol
sources/tests are built in ignored build directories. The adapted AACryptor
interface and new backend retain this license and notice; the full license is
in src/android_auto/LICENSE.NBT-AAD.md. No proprietary libraries were added to
tracked sources. The upstream daemon and its USB takeover logic were not run.

The reference requirements specify NBT Evo and CarPlay. Its milestone-7 Makefile
targets ARMv7 and links a2i_client, socket, ssl, crypto, screen and usbdi. Merely
changing the compiler target will not provide the original NBT's runtime paths.

| Area | Finding | Required next evidence |
| --- | --- | --- |
| Protocol | Static core, 400 lifecycle cycles and simulated-phone TLS/service exchange pass | Real phone protocol negotiation and trust/identity configuration |
| USB | Native AOA query, controlled AA identity switch and direct USB version 1.7 exchange succeeded | Sustained transport and real-phone TLS handshake |
| Direct USB | usb_direct uses QNX libusbdi, can claim driver paths and SIGTERM/SIGKILL stock drivers | Must separate transport from these Evo-specific interventions before use |
| Video | Reference uses CarPlay mm-renderer and Evo display integration | Original NBT decoder and display APIs; do not assume screen/LayerManager compatibility |
| Audio | Reference relies on vehicle-specific routing/backends | Original NBT audio interfaces and stream ownership |
| Menu | Our separate legacy PluginApp works | Later callback/start-stop integration, after standalone receiver validation |

## VM implementation and validation

1. `scripts/Test-AndroidAutoCore.py`: 8 upstream component tests passed with
   qcc 4.4.2 gcc_ntox86_cpp: service discovery, channel filtering, audio format,
   audio channels, deferred acknowledgements, media acknowledgements, media
   metadata and session ownership. aa_session, aa_cryptor, usb_accessory and
   usb_direct also compiled as objects. USB objects were not executed.
2. The stock SDK/runtime has OpenSSL 0.9.8m. `tls_capabilities` confirms the
   required TLSv1_2_client_method export is missing. An object compiling is
   not sufficient to link the original cryptor against this library.
3. `scripts/Test-QnxTls.py`: builds the official Mbed TLS 3.6.7 release,
   verifies archive SHA256, retains upstream license, uses C99 and disables
   AESNI, HAVE_ASM and PADLOCK because the old compiler cannot compile their
   intrinsics/assembly. Libraries are static, private build artifacts.
   Nothing is installed over QNX's system SSL libraries.
4. AES/GCM/SHA256/RSA known-answer tests pass. Real TLS 1.2 mutual certificate
   authentication succeeds with ECDHE-RSA/AES-128-GCM-SHA256. Binary payloads
   of 1, 255, 4096 and 16000 bytes pass in both directions over fragmented
   in-memory transport. Modified ciphertext and a wrong peer hostname are
   rejected. Entropy comes from the VM OS; no deterministic RNG fallback.
5. `src/android_auto/aa_cryptor_mbedtls.cpp` supplies the reference interface
   with bounded input/output queues. Its test checks missing credentials,
   fragmented mutual TLS, encryption/decryption, terminal failure after
   tampering, reinitialization and oversized input rejection.
6. `scripts/Test-AndroidAutoSession.py`: links the actual reference AASession
   against this backend and produces `libnbt_aa_core.a`. Its 400-cycle test
   exercises version requests, peer closure, local stop, reconnect and a stop
   racing with peer closure. File descriptors: **5 before, 5 after**.
   Test-only QNX adaptations replace Linux /proc/self/fd enumeration with
   fcntl/getrlimit and unsupported GCC atomic test counters with a mutex;
   an alarm bounds hangs.
7. `scripts/aa_session_overlay.py` applies checked changes to the pinned session
   source in the build archive, leaving the reference checkout clean. An explicit
   version/handshake/ready state now rejects refused or malformed versions,
   repeated negotiation, premature encrypted traffic and application messages
   before authentication. TLS initialization/handshake/read failures terminate
   the session; incomplete TLS input remains buffered. AuthComplete is sent once.
   Session exit/restart clears TLS state. Channels outside the implementation's
   32-slot table and empty frames are rejected. Plain control pings remain supported.
8. `src/android_auto/test_session_protocol.cpp` runs a simulated phone against
   the actual session over a local socketpair, using generated lab certificates.
   Four TLS handshakes and encrypted service discovery exchanges pass, including
   reconnect on the same session object. Socket writes and TLS handshake input
   are deliberately fragmented. A synthetic PCM block reaches the audio callback
   with its channel, bytes and timestamp intact and receives its acknowledgment.
   Ping timestamp echo and encrypted ByeBye/response work. Twelve initial
   negotiation failures, tampered ciphertext, repeated authentication and plaintext
   service requests terminate promptly. This tests protocol plumbing, not audio
   playback or the phone's actual Android Auto identity/trust requirements.

The native `usb_inventory` diagnostic only subscribes to device notifications.
It never attaches to devices, opens endpoints, changes configurations or sends
AOA requests. It connected/disconnected successfully as root in the VM with
zero insertion events (no phone attached); screenshot evidence is private at
.local/qnx-usb-root.png. Running as nbtbuild returned ENOENT even though the
stack node exists and is root-owned; this result does not mean io-usb is absent.
No USB service was restarted to make the probe work.

Successful evidence directories:

- build/aa-core-20261105T193437Z-09e90e4d
- build/aa-tls-20261105T194218Z-e6b52d4b
- build/aa-session-20261105T202216Z-95df5ae5

Each contains source inputs, build/test logs and JSON results. TLS/session
results include SHA256 hashes for downloaded artifacts. The session directory
contains the reusable static core library and three verified ELF32/i386 test
executables. These tests are not an installable Android Auto application.
The backend executable's dynamic dependencies are libsocket.so.3, libm.so.2,
libcpp.so.4 and libc.so.3; readelf confirms no libssl/libcrypto dependency.
Mbed TLS built with one GCC 4.4 may-be-uninitialized warning in x509.c's
otherName parser. Reviewed control flow assigns type_id on the successful
parse path before copying it; no upstream crypto source was modified to
suppress the warning. This port has not undergone a full compiler/security
qualification or target performance test.

Reproduction from the project directory (use the real Python executable):

```powershell
& $python scripts/Test-AndroidAutoCore.py
& $python scripts/Test-QnxTls.py
& $python scripts/Test-AndroidAutoSession.py --core-build build/<core-run> --tls-build build/<tls-run>
```

The latter command uses the two successful result directories from the first
commands. Reference checkout must match the pinned commit and remain clean.
VM host keys remain enforced; scripts reject other SSH hosts/users. TLS test
fixtures are generated locally, use no real credentials, and stay in ignored
build archives. Their broad validity window accommodates the VM's old clock;
they must never be used as Android Auto identities or trusted on the phone.

The backend currently requires explicit NBT_AA_TLS_CERT, NBT_AA_TLS_KEY,
NBT_AA_TLS_CA and NBT_AA_TLS_PEER settings and rejects an unverified peer.
Actual Android Auto identity/trust compatibility has not been established.
It does not reuse the reference's embedded private key or disable certificate
verification. The phone simulator exercises AA framing, TLS and service discovery
with lab identities. It does not establish compatibility with a real Android Auto
phone, decode video, play audio or exercise the USB transport. The upstream
reassembly logic and remaining service handlers still need further review and
target validation; these tests are not a complete protocol conformance suite.

## Next executable step

The read-only vehicle inventory has now run. `/opt/mm/bin/aoap-switch` and
`devc-serusb-aoap` exist. The stock enumeration configuration recognizes
18d1:2d00/2d01 and starts the accessory driver with a 20480-byte buffer.
Its fallback starts aoap-switch in application-trigger mode (`-t`) with
`/opt/mm/etc/aoap.cfg`, whose identity is BMW / BMWAccessory. This is not the
Android Auto identity; driver presence alone does not start Android Auto.

The vehicle's x86 aoap-switch was copied read-only into the ignored local audit
directory and disassembled in the VM (never executed there). SHA256:
`a72c8e69487ab6d4b9f7f25e01e7c11e7a90637fe16c5baae7e50ca357e2747f`.
Its dispatch recognizes 0x40040501, 0xc0040502,
0xc6040503 and 0x40040504, matching the reference mode/switch/config-switch/
protocol operations. The custom identity uses a 4-byte protocol followed by
six fields at 256-byte strides, copied into 128-byte local buffers: the
reference's 127-byte string limit remains necessary. No devctl or switch has
been sent on the vehicle. Private evidence: `.local/aa-inventory.txt`,
`.local/aa-oap-details.txt`, `.local/aa-usb-tree.txt` and `.local/aa-driver-audit/`.

`scripts/Inspect-NbtUsb.sh` now supplies a repeatable cached USB snapshot and
lists accessory node/process names. It sets the library path for the one USB
utility invocation because SSH lacks `/lib`; without that, the stock utility
fails to load libusbdi.so.2. It does not use `usb -D` or open phone endpoints.
Run it after the unlocked phone is plugged into the car's USB data port, then
compare with the TCB-only baseline before selecting a transport or switching
the phone's mode. Video/audio runtime interfaces remain unverified.

The first connected-phone snapshot is saved privately as
`.local/aa-usb-phone.txt`; `.local/aa-phone-descriptors.txt` and
`.local/aa-phone-processes.txt` provide cached descriptor/process evidence.
Do not use `pidin fds` for further ownership checks here: both a global query
and a query restricted to NBTMediaMainApp hung. Their SSH commands timed out;
termination signals were sent only to the two diagnostic pidin processes.
The targeted query exited. The global query (PID 847977) still appears in
QNX REPLY state, blocked on PID 167995 despite SIGKILL; do not claim cleanup
complete or kill that resource manager to release it. No stock media/USB
process was stopped. Accessory endpoint ownership remains unknown;
do not add a competing reader based only on the existence of `/dev/oap02`.

A kernel-only replacement is now implemented as
`src/android_auto/connection_inventory.c`. It uses ConnectServerInfo to list
connections from an explicit client PID to an explicit server PID without
opening devices or sending io_fdinfo requests. The five-second bounded tool
checks numeric arguments, enumeration limits and query errors. No arguments
runs its VM self-test (own connection, cross-process inspection, detach).
The self-test uses a side channel so fork/exit cannot send IO_CLOSE to the
dummy server. `scripts/Test-AndroidAutoCore.py` now builds/tests this diagnostic.
Successful build: `build/aa-core-20261105T222827Z-bf38736f`, all eight component
tests passed and connection_inventory returned zero. The earlier failed VM
self-test at 222644Z used a normal descriptor and was corrected before deployment.

The verified ELF32/i386 diagnostic was temporarily transferred to vehicle RAM,
read back and hash-matched, executed once against the observed processes, then
removed and absence verified. SHA256:
`a9b598b448a7f348c32ec0ee53081fead604203fd28e3618209ed5bc7aa79ee3`.
It found **two live connections** (coids 36/37, channel 1) from NBTMediaMainApp
PID 127021 to devc-serusb-aoap PID 778339. This proves a competing client exists;
it does not prove which connection is reading or writing. Private evidence:
`.local/aa-phone-kernel-connections.txt`, `.local/aa-connection-probe-cleanup.txt`.
The phone endpoint was never opened by our diagnostic. The next transport test
must address stock-client contention as well as the BMW accessory identity;
simply launching the session on `/dev/oap02` is not a controlled AA test.

scripts/Inspect-NbtAndroidAuto.sh is a read-only inventory of device names,
selected process arguments, candidate library metadata and USB enumeration
configuration. It does not read device streams, change USB mode or stop a
driver. Local sh -n syntax validation and target execution passed. Save its output only under
.local because process arguments/configuration may identify the vehicle.

Using the existing local SSH queue helper with a ready session:

```powershell
& $python C:\NBT-Project\.local\nbt_request.py 'sh -s' --input-file C:\NBT-Project\scripts\Inspect-NbtAndroidAuto.sh --output-file C:\NBT-Project\.local\aa-inventory.txt
```

Here $python is the actual Python executable, not the Windows Store alias.
The script must retain LF newlines. Missing candidates mean only that these
specific paths were not found; follow up with targeted searches if needed.

Using the captured baseline, compare enumeration with the
unlocked phone connected to the car's USB data port. Verify actual driver/USB
APIs before a bounded AOA negotiation test. An AOA connection alone is not an
Android Auto session: version exchange, TLS and service negotiation follow.
Display, audio and controller support remain separate milestones. Do not run
the reference daemon or direct-USB driver takeover on this car unchanged.

## Primary sources

- https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/tree/41975f269b065dbdbc77af9f568387cd22b9ed27
- https://source.android.com/docs/core/interaction/accessories/aoa
- https://source.android.com/docs/core/interaction/accessories/aoa2
- https://support.google.com/androidauto/answer/6348019?hl=de
- https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
- https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.ddk_en_usb/usbd_connect.html
- https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_lib_ref/c/connectserverinfo.html

AOA uses accessory bulk endpoints; USB debugging is not required for accessory
connection. AOA 2.0 USB audio is deprecated since Android 8 and should not be
confused with Android Auto's audio stream. Android 12 meets Google's current
OS minimum for wired Android Auto; this does not establish NBT compatibility.
