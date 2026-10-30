# CrossPoint LocalSend receiver

Adapted from PapyriX's MIT-licensed `LocalsendService`, `LocalsendHttp`, and
`LocalsendApp`. Original copyright and permission notice are in `LICENSE`.
The portable protocol core has no Arduino dependencies. Network and storage
operations live in `LocalsendServer`; all filesystem access uses CrossPoint's
`Storage` / `HalFile` interfaces.

## Integration

- Include `activities/network/LocalsendActivity.h` in the activity router.
- Construct `makeUniqueNoThrow<LocalsendActivity>(renderer, mappedInput)`, check
  for allocation failure, then pass the result to `replaceActivity` using the
  existing network-activity routing pattern.
- Add a LocalSend item to `NetworkModeSelectionActivity`, labelled
  `tr(STR_LOCALSEND)` with description `tr(STR_LOCALSEND_DESC)`.
- The activity launches `WifiSelectionActivity` itself if disconnected. It
  stops sockets, closes/removes partial files and releases buffers on exit,
  then follows the existing network activities' Wi-Fi disconnect/silent-reboot
  convention. It prevents automatic sleep only while the receiver is running.
- No modifications to CrossPointWebServer or FsHelpers are needed. Files appear
  beneath `/received`; the normal file browser can open them. An already-built
  persistent library index still follows the existing rebuild workflow.
- No additional external dependency is required: ArduinoJson 7, Arduino-ESP32
  Wi-Fi/UDP, and the existing mbedTLS SHA-256 implementation are used. The
  library is discovered by the `LocalsendServer.h` include; `platformio.ini`
  need not change for the library itself.

New English translation keys (add through the normal I18n generator, not by
editing generated headers):

| Key | English |
| --- | --- |
| `STR_LOCALSEND` | LocalSend |
| `STR_LOCALSEND_DESC` | Receive books from LocalSend |
| `STR_LOCALSEND_HINT` | Send to CrossPoint from LocalSend on the same Wi-Fi |
| `STR_LOCALSEND_DESTINATION` | Saved to /received |
| `STR_LOCALSEND_WAITING` | Waiting for files... |
| `STR_LOCALSEND_TRANSFER_FAILED` | Transfer failed |
| `STR_LOCALSEND_STORAGE_FAILED` | Could not save received file |
| `STR_LOCALSEND_CANCELLED` | Transfer cancelled |

Existing connection, memory-error, exit, and Calibre receiving/received strings
are reused. Progress rendering is throttled to 1.5 seconds; completion/error
transitions request an immediate next render. Rendering reads a copied snapshot
under `RenderLock`, never mutable transport state.

## Wire behavior and bounds

The receiver advertises `CrossPoint`, LocalSend protocol `2.0`, HTTP port
`53317`, and `download:false`. Discovery uses multicast `224.0.0.167:53317`,
three startup announcements, and `announce:false` replies to other members'
announcements. The fingerprint is random for each receiver run. HTTP discovery
through `POST /api/localsend/v2/register` and `GET /api/localsend/v2/info` is also
supported. No outbound TLS connection, reverse download API, PIN or background
listener is installed.

`POST /api/localsend/v2/prepare-upload` accepts a `files` object whose map keys
are the file IDs. Each accepted value must have a string `fileName` and unsigned
integer `size`; an optional `id` must match the map key. Optional `sha256` is
null/absent or exactly 64 hexadecimal digits. Unsupported or invalid entries
are omitted from the response, which returns `sessionId` and per-file tokens.
A valid empty files object returns 204, invalid requests return 400, and an
occupied session returns 409. Limits are 16 accepted files, 64-byte IDs,
256 MiB per file, a 16 KiB control body, and a separate 12 KiB JSON allocation
arena (exhaustion returns 413). JSON nesting is bounded and trailing garbage
is rejected. Metadata/preview fields are not persisted.

`POST /api/localsend/v2/upload?sessionId=...&fileId=...&token=...` streams
content-length or chunked bodies, including chunk extensions/trailers, directly
to SD in 4 KiB slices. Percent-encoded and form-encoded IDs are supported.
Tokens and the prepare sender's IP must match. The declared decoded size must
match exactly; optional SHA-256 is checked before publication (422 on mismatch).
`Expect: 100-continue` is supported after request authorization. Ambiguous
Content-Length/Transfer-Encoding framing and malformed HTTP are rejected.

Two uploads can run concurrently, with a third connection slot for control
requests. A duplicate in-flight file returns 409. The transport is polled by
the activity without a background task or blocking body-read loop, so Back,
Wi-Fi loss, and `POST /api/localsend/v2/cancel?sessionId=...` can interrupt an
upload. Cancel requires the matching session and sender IP. The idle session
expires after 60 seconds; upload progress refreshes it. Connections have a
15-second stall timeout, plus a 30-second absolute control-request deadline;
progressing file bodies have no absolute duration cap.

Sender paths are flattened to a FAT-safe basename, stripped of controls and
leading/trailing dots/spaces, and truncated at UTF-8 boundaries; invalid UTF-8
is rejected. Names retain the final extension where possible. Name collisions
produce `name (N).ext`
(up to 999), including collisions between simultaneous uploads. Existing files
are never opened for truncation. Data is first written using exclusive creation
to `/received/.localsend-<token>.part`. A successful size/hash check and HAL
close/sync precede the rename to the final filename. Errors, cancellation,
disconnection and exit remove partials; a later receiver start also removes
interrupted files matching this private temporary-file pattern. Completed files
are kept when the rest of a session is cancelled.

The receiver auto-accepts while its screen is open, like a fast-receive device.
HTTP is unencrypted: use a trusted Wi-Fi network. Closing the activity stops
receiving. All large buffers are allocated once as part of the fallible server
object and freed at activity exit; the parser cannot grow into the Wi-Fi heap.
Actual target heap headroom still needs integrated firmware measurement.

## Host checks for the integrator

The implementation agent did not run builds or tests. The portable regression
file can be compiled independently of PlatformIO or a reader:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -Ilib/Localsend/src \
  lib/Localsend/src/LocalsendService.cpp \
  lib/Localsend/src/LocalsendHttp.cpp \
  lib/Localsend/tests/protocol_test.cpp \
  -o /tmp/crosspoint-localsend-protocol-test
/tmp/crosspoint-localsend-protocol-test
```

The cases cover session/IP/token validation, replay rejection, completion,
expiry and timer wraparound, checksum metadata, path safety, collision naming,
request routing/query decoding, HTTP framing conflicts, and chunk fragmentation
at every byte boundary (including malformed, truncated, zero-length and in-place
bodies). They do not claim coverage of real sockets, SD writes or UI rendering.
Run the integrated firmware build and normal project formatter only after the
other implementation slices land. Real LocalSend interoperability, simultaneous
uploads/cancellation, write-failure cleanup, e-ink responsiveness, and target heap
headroom remain integration checks; no physical reader was used here.

Protocol reference: <https://github.com/localsend/protocol/blob/main/README.md>.
