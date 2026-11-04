# Frame-count buffering for DirectShow capture

## Controls

In a Video Capture Device source's properties, select **Buffering → Frame-count
buffering**. Auto-Detect, Enable and Disable retain their existing behavior;
existing sources are not opted into this mode automatically.

- **Target buffered frames**, default 2, range 1–30: wait for this many queued
  video frames before starting playback.
- **Maximum buffered frames**, default 5, range target–30: when an arriving frame
  makes the queue exceed this count, discard the oldest frames until exactly the
  target count remains. Re-prime using those retained frames on the next output
  tick; it is not necessary to wait for additional arrivals.
- **Rebuild video scheduling timestamps**, default off: use a synthetic capture
  clock instead of native timestamp spacing.
- **Actual capture FPS numerator/denominator**, default 60/1: visible when
  rebuilding is enabled. For example, enter 60000 and 1001 for 59.94… FPS. These
  fields do not change the DirectShow capture mode or rely on its reported FPS.

Changing the buffering configuration discards pending frames and requires
prefill again. Disconnecting, stopping capture, or a format change also resets
the buffer. Each frame is consumed at most once. The source's last uploaded
texture remains visible whenever no new frame is selected.

## Output timestamp selection

The comparison clock is the current OBS **video output tick timestamp**
(`obs->video.video_time`), not a fresh system-clock reading inside the capture
callback. Capture timestamps can have a different epoch and must not be directly
subtracted from that output timestamp.

Once prefill completes, map the oldest queued capture frame to the current OBS
output timestamp. With native spacing, the scheduling timestamp of another frame
is `output_anchor + capture_timestamp - capture_anchor`.

With rebuilding enabled, scheduling timestamp number `n` is:

```
output_anchor + floor(n * 1,000,000,000 * fps_denominator / fps_numerator)
```

The implementation carries fractional remainders, rather than repeatedly adding
a rounded nanosecond interval. Capture arrival jitter does not change the clock;
each received frame advances its ordinal once, including subsequently discarded
frames. Missing frames cannot be inferred: if a device silently drops a frame,
the next received frame still has the next ordinal.

Let `T` be the configured OBS output interval and `S` a mapped/rebuilt capture
timestamp. On each OBS output tick `O`:

1. Discard queued frames with `O - S > 1.25 * T`.
2. Select the oldest remaining frame with `0 <= O - S <= 1.25 * T`.
3. Leave future frames (`S > O`) queued. Select at most one frame per output tick.

The equivalent signed range for `S - O` is `[-1.25*T, 0]`, not simply
`S - O < 1.25*T`, which would also accept arbitrarily stale frames.

When the queue empties, stop advancing playback and require the target count
again. After prefill or overflow, anchor the retained oldest frame to that new
OBS output tick and restart the fractional accumulator. In native-spacing mode,
backward capture timestamps or a forward jump exceeding two seconds also clear
pending frames and restart prefill. Rebuilt scheduling ignores device timestamp
discontinuities.

Example at OBS 60 FPS: the eligible age range is approximately 0–20.833 ms.
If two pending frames are 20 ms and 4 ms old, select the 20 ms-old frame and remove
it. If six frames accumulate with target 2 / maximum 5, discard four oldest
frames, retain two, and re-anchor the older retained frame on the next output
tick. A 120 FPS capture feeding a 60 FPS output necessarily discards frames;
a 30 FPS capture cannot create 60 distinct frames per second.

## Audio and scope

Only the video selection clock is rebuilt. The original `obs_source_frame`
timestamp is preserved for filters, source profiling, and existing audio timing
adjustment. This avoids mixing synthetic system-epoch video timestamps with
native-epoch DirectShow audio timestamps. It does not implement a new audio
buffer, audio resampling clock, or independent audio retimestamping; verify A/V
sync with the actual capture device and adjust the audio sync offset if needed.

The new UI is specific to Windows DirectShow. The opt-in libobs API can support
other asynchronous video sources, but no other source is automatically changed.
Normal and deinterlaced async selection both use the new scheduler when enabled.
This happens before scene composition, so it affects preview, recording,
streaming, and virtual camera, unlike buffered preview presentation.

## Validation

`test/test_async_frame_buffer.c` exercises prefill, underrun/restart, overflow
trimming, one- and thirty-frame limits, mapped native epochs, oldest selection,
exact age/future boundaries, stale queue exhaustion, native discontinuities,
configuration normalization, preservation of native timestamps, rational clock
rounding over up to 100,000 steps per rate, synthetic 60/60, 120/60, 30/60 and
fractional-rate cadence, and alternating -2/+2/+2 ms capture arrival jitter.
The 60/60 jitter model selects 600 distinct frames across 600 measured output
ticks, including rebuilt scheduling with unstable device timestamp spacing.
It is registered with CTest alongside the preview tests when test builds are
enabled. A standalone build is also possible:

```
gcc -std=c11 -Wall -Wextra -Werror -Ilibobs test/test_async_frame_buffer.c -o test_async_frame_buffer.exe
./test_async_frame_buffer.exe
```

These are scheduler tests, not a physical capture-card or A/V synchronization
test. A hard age cutoff cannot guarantee zero dropped frames with arbitrary
arrival jitter, main-thread stalls, missing capture callbacks, or an incorrect
manual rate. Test actual raw/encoded capture, interlaced modes, start/stop,
source hiding, changing properties, and audio synchronization on hardware.
