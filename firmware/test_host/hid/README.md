Run `bash firmware/test_host/hid/run.sh` from the repository root. Only a C11
compiler and Bash are required. Each executable includes the actual
`firmware/main/usb_hid.c`, with deterministic FreeRTOS/TinyUSB/cJSON facades;
the driver logic is not duplicated in a test implementation. Builds use strict
warnings and UndefinedBehaviorSanitizer at both 100 Hz (ESP-IDF's default) and
1000 Hz. The power-only implementation is compiled and exercised separately.

The fake clock advances only when firmware yields, and rejects zero-tick waits.
Reports become host-visible only upon the simulated completion callback. Cases
cover repeated characters, Shift, Enter, maximum-length typing, physical letter
chords, F12, whole-sequence rejection, malformed F names, driver/task allocation
failure, endpoint readiness, late completion, short and failed transfers,
rejected key-up submissions, indefinite stalls, slow-but-progressing endpoints,
remote-wakeup refusal/timeouts, disconnect/reconnect, suspend/resume, resets
entirely between polling iterations, fail-safe USB detach on permanent stalls,
and mutex ownership. Recovery tests invoke the production worker's `recovery_once()` body
without sending another command; all uncertainty tests assert that no following
character or Enter is replayed.

The 256-character plus Enter case completes in 5.15 seconds with 10 ms report
polling. Actions share an 8-second deadline including lock contention, with at
most one additional 200 ms cleanup attempt and a 50 ms USB detach when that
cleanup fails. Scheduling can add a tick to a deadline. A slow endpoint can
therefore produce partial text and an error;
clients must not automatically retry that text. Recovery retries only neutral
reports and never initiates remote wakeup. An endpoint that cannot accept a
release triggers detach/reconnect so the OS removes its held keyboard state;
input remains blocked until a genuine mount callback confirms new enumeration.
Lifecycle callbacks invalidate canceled reports even when brief USB resets are
never observed by polling. esp_tinyusb 1.x leaves these callbacks to this
application when its MSC helper is disabled, as in this firmware's S3 profile.

These are host tests, not an ESP-IDF cross-build or USB/OS integration test.
Descriptor encodings and TinyUSB's ASCII table are replaced by minimal test
data; actual host layout, controller behavior, bus faults, resume timing, and OS
text delivery need an ESP32-S3 hardware check. USB transfer completion establishes
delivery to the USB host, not consumption by a focused application. TinyUSB
0.15's completion callback does not expose the transfer result; short transfers
are rejected, but a falsely reported full-length completion cannot be detected
by the application. Newer TinyUSB's explicit failure callback is also handled.
