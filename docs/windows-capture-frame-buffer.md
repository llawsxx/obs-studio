# Frame-count buffering for Windows screen and game capture

Window Capture, Display Capture and Game Capture now have an opt-in
**Frame-count buffering** checkbox in their source properties. Existing sources
remain unbuffered. This is an input-side queue, before scene composition: it
affects preview, recording, streaming and virtual camera output, not just the
preview swap chain.

## Settings and scheduling

- Target buffered frames: default 2, range 1–30.
- Maximum buffered frames: default 5, range target–30.
- Rebuild capture timestamps: off by default. When enabled, enter the actual
  capture rate as a numerator and denominator, for example 60000 / 1001.

The queue uses the same scheduler as DirectShow frame-count buffering. Once the
target count has arrived, anchor the oldest capture timestamp to the current OBS
output timestamp. Native mode preserves subsequent capture timestamp spacing;
rebuilt mode advances one ordinal per captured frame using exact rational
arithmetic, including frames subsequently discarded by selection. It does not
round the fractional period once and repeatedly add the rounded result.

On each OBS source tick, select the oldest queued frame whose mapped/rebuilt
timestamp differs from the OBS output timestamp by at most ±0.6 OBS intervals.
Remove consumed frames, discard older expired frames, and keep future frames
queued. Multiple renders of the source during the same OBS frame use the same
selected texture; rendering is not another selection opportunity.

An empty queue retains the last displayed image and requires the target count
again. An arriving frame that would exceed the maximum discards the oldest
queued frames, retains the newest target count, and re-anchors on the next OBS
tick. Native timestamp regression or a jump of more than two seconds restarts
prefill. Changing configuration, resolution or pixel format clears the queue;
device loss and capture teardown also discard queued GPU resources.

WGC and Desktop Duplication need not deliver another frame for an unchanged
image. To avoid an indefinitely black initial picture, if initial prefill has
not completed and its newest frame has been idle for more than 250 ms, show
that still image once and remove it from the queue. This does not start the
playback clock: subsequent motion must still prefill the target count. No
synthetic duplicate frames are enqueued to claim that prefill has completed.

## Acquisition paths

| Path | Buffered acquisition |
| --- | --- |
| WGC window / display | Copy each `FrameArrived` texture into the queue before the next update can overwrite it. Use `SystemRelativeTime` for its timestamp. No polling collector is needed. |
| Desktop Duplication | A separate collector polls `AcquireNextFrame(0, ...)` approximately every millisecond, independently of OBS composition. Use `LastPresentTime` converted from QPC to nanoseconds and enqueue only new desktop updates. Sources sharing a duplicator maintain separate queues and cursors. |
| Game capture | The updated graphics hook publishes a coherent timestamp for each shared-texture update or completed shared-memory copy. An independent collector snapshots new updates rather than waiting for an OBS tick. Shared-memory timestamp reads occur under the existing texture mutex. |
| BitBlt window / legacy display | A separate collector takes screenshots on a deadline grid, using the OBS interval or configured rational capture rate. Late wakeups do not permanently stretch every subsequent interval or trigger a burst of catch-up screenshots. |

The polling collector is created lazily when needed. It shares the OBS graphics
context lock, so a prolonged graphics stall can still prevent intake. A game
hook or Desktop Duplication image overwritten before collection cannot be
recovered by the local queue. The game hook still has its existing transport,
not a new cross-process history ring. In buffered mode its capture throttle
allows twice the OBS rate even if the old limit-capture-framerate option was
checked, avoiding another OBS-rate sampling gate ahead of the queue.

Deploy `win-capture`, `libobs-winrt`, `libobs`, the D3D11 backend, and both game
hook architectures together, with OBS and captured applications closed. Already
injected older game hooks lack the timestamp extension; capture falls back to
the existing unbuffered path with a warning until the application is restarted.
Older WGC libraries also fall back to unbuffered capture with a warning. The
hook-info extension consumes reserved fields and preserves the 648-byte layout.

## Costs and limits

Queue copies stay on the GPU; there is no additional image readback for the
buffer. Textures are allocated lazily, with at most maximum + 1 retained
textures including the displayed frame. Maximum 5 at 1920×1080 BGRA can retain
about 47.5 MiB in addition to the source's original resources. Reducing the
maximum and target reduces queueing capacity and latency; disabling the option
releases the queue. Audio capture is not delayed automatically, so check A/V
sync and adjust audio delay where needed.

A buffer can absorb bounded delivery jitter and change the capture-to-OBS
sampling phase. It cannot create frames missing from the application itself,
recover frames already overwritten upstream, make a 30 FPS source provide 60
unique frames, eliminate sustained GPU overload, or repair missed BitBlt
screenshots. It is separate from buffered preview presentation.

Every five seconds an enabled, showing source logs `[capture-buffer: 'name']`
with cumulative received, delivered, repeated/no-new-selection, discarded and
rebuffer counts plus current queue depth. Initial prefill contributes to the
no-new-selection count; these are not OBS render-lag or final-display counters.

## Regression checks

Build the `capture-buffer-tests` target in the Windows build. Run it with the
D3D11 backend path as its argument, using a runtime containing the matching
`obs.dll` and the normal OBS data directory layout. It tests real GPU copies
and readback, prefill, overflow, held-frame ownership, limits, resolution and
timestamp resets, static-image startup, jittered arrival phases, fractional
capture deadlines and worker startup/shutdown. Each synthetic phase/jitter case
spans five minutes of 60 FPS timeline in native and rebuilt modes. Live WGC
callback/render/teardown and shared Desktop Duplication queues are also checked
when those APIs are available on the current desktop; unavailable capture APIs
are reported separately rather than presented as successful live captures.

The existing `test_async_frame_buffer` also tests the shared scheduler's
symmetric selection boundaries and rational timestamp rebuilding. These checks
do not replace end-to-end cadence measurements with actual games, WGC, display
drivers and refresh-rate combinations.
