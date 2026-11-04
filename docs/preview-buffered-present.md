# Buffered D3D11 main preview (experimental)

On Windows, the main preview uses buffered presentation by default. Recording,
streaming, virtual camera output, projectors, and other displays retain their
existing scheduling. This is an experiment, not a confirmed fix for every cause
of preview stutter.

The OBS graphics thread draws the complete preview, including editing overlays,
into an offscreen GPU texture. A separate presentation thread waits for the DXGI
frame-latency waitable object without holding the OBS graphics lock. It copies an
already completed texture to the swap-chain back buffer and calls
`Present(1, DXGI_PRESENT_DO_NOT_WAIT)`. A busy swap chain is retried without
blocking for queue space. Drawing and presenting still share the OBS graphics
context and GPU, so this does not guarantee that output work is unaffected.

## Selection and latency

- Frames carry a monotonic CPU timestamp taken when preview rendering begins.
  D3D11 event queries establish GPU completion without a blocking wait. Age is
  measured from that CPU timestamp, not from GPU completion or source capture.
- Only GPU-completed, unconsumed frames with age in `[0, 1.25 * refresh period]`
  are eligible. The oldest eligible frame is selected. At 60 Hz the upper bound
  is approximately 20.83 ms; a 20 ms-old frame wins over a 4 ms-old frame.
- A successfully submitted frame is immediately removed from the candidate
  queue. Its sequence number and all older sequence numbers cannot be selected
  again, even if the texture remains allocated. Failed or busy submissions do
  not consume a frame and can be retried.
- If no eligible new frame exists, no Present is issued. The last displayed
  image remains on screen, and the presentation thread retries after 1 ms while
  retaining swap-chain credit. Retaining an image is not reselecting its frame.
- Unconsumed frames older than 1.5 refresh periods (25 ms at 60 Hz) are expired
  during producer/consumer queue maintenance. The last displayed texture may
  remain allocated, but is no longer a candidate.
- Eight texture slots are allocated lazily. The producer never waits for a free
  slot; it reclaims an older completed candidate or skips that preview update.
- Refresh timing comes from DXGI frame statistics when available. The current
  monitor's nominal refresh rate is used until statistics establish a measured
  refresh interval. This fallback does not provide precise physical scanout timing.

The age window applies at selection, not physical scanout. GPU/compositor
scheduling and panel latency add delay. This is not a guaranteed measured
end-to-end delay. The waitable object is queue feedback, not a VSync interrupt;
this implementation does not claim to select exactly at physical VSync.

## A/B testing

Right-click the main preview and toggle **Buffered Preview Presentation
(Experimental)** (Chinese: **预览缓冲呈现（实验性）**). The toggle is also available
in the disabled preview's context menu when its display exists. Changes take
effect immediately and are saved to the user configuration. The check mark
reflects whether activation succeeded; a failed toggle keeps the previous mode
and shows an error.

For manual configuration instead of using the UI:

Close OBS before editing `%APPDATA%\obs-studio\user.ini`. Under `[BasicWindow]`,
set `PreviewBufferedPresent=false` and restart to use the original preview path.
Set it to `true`, or remove the entry, to enable the experiment. Unsupported
graphics backends fall back to the original path.

Build and deploy the frontend, libobs, and libobs-d3d11 together. Replacing only
one DLL is insufficient. On other platforms the original path is unchanged.

## Diagnostics

Open **View > Stats** (or the Stats dock) and find **Main Preview**. This section
refreshes with the existing 2-second stats timer. It shows the active mode,
refresh-rate estimate/measurement, preview production and submission FPS,
submission total, candidate queue depth, discards/expiry/producer skips,
not-ready skips, no-candidate/GPU-pending checks, busy Present retries, frame age,
Present duration, graphics-lock wait, and maximum submission gap. The existing
**Reset** button clears preview counts, averages and maxima without discarding
queued frames. Presentation-mode switches and buffered swap-chain resets also
start a new counter epoch; the first rate sample after a reset is a dash.

The original presentation path reports Present calls, not verified successful
submissions. Buffered-only metrics show a dash in the original path. Neither
path measures the number of distinct images physically displayed. Counts and
timing aggregates are cumulative since their reset, whereas FPS uses differences
between adjacent UI samples. The UI reads a snapshot under the graphics lock;
it does not issue GPU queries or flushes to gather statistics. Periodic log resets
do not reset the UI's cumulative counters. Statistics reset applies to both the
floating window and dock. Intentional preview pauses may extend the maximum gap.

Search the current OBS log for `[obs-preview-sync]`. Summaries are emitted about
once per second and bypass repeated-line suppression. They include production,
successful Present submissions, empty-candidate checks, expired/discarded frames, producer
skips, GPU-pending checks, nonblocking Present retries, queue depth, submission
age, maximum submission gap, graphics-lock wait, and available DXGI counters.

`submit_age_ms` measures CPU time from beginning the preview snapshot to the end
of Present, not actual display latency. `submitted` is an API submission count,
not the number of distinct images physically scanned out. `gpu_pending_checks`
counts failed readiness checks, not unique frames. At 120 FPS / 60 Hz, discarded
preview frames are expected and do not imply recording frame loss.
`no_candidate_checks` counts retry attempts, not missed physical refreshes.

Compare the same moving scene with the experiment enabled and disabled. Check
main output rendering lag as well as preview appearance. Verify 60/60 and
120/60, dragging, virtual camera on/off, resizing, minimizing, monitor moves,
display refresh changes, preview disable/enable, and shutdown. Actual physical
presentation still needs an external presentation trace or observation.

The portable selection test is `test/test_preview_frame_selection.c`; it is
registered with CTest when `BUILD_TESTS` or `ENABLE_UNIT_TESTS` is enabled.
`test/test_preview_stats.c` additionally checks interval accumulation, maxima,
non-destructive live snapshots, and counter reset.

## Synthetic boundary coverage and limitations

Tests exercise the actual selection helper using integer nanosecond timestamps,
eight candidates, a 120-refresh warm-up and 600 measured refresh opportunities.
They cover exact age/expiry boundaries, GPU readiness, timestamp ties, and the
case where an already consumed 2 ms-old frame is 18.67 ms old next time. That
frame remains ineligible. A busy Present retry is allowed only until success.

A 64-phase sweep at 60/60 with alternating `-0.2, +0.2, +0.2 ms` selection
jitter and 2 ms simulated GPU completion delay selects 600 distinct frames per
600 opportunities. At the same jitter, 120/60 and 144/60 also select 600 distinct
frames; 30/60 selects 300, as expected.

The hard 1.25-period cutoff is not universally stutter-proof. A regression also
documents a counterexample: at one phase, `-2, +2, +2 ms` jitter and 2 ms GPU
completion delay yield only 400 distinct selections per 600 opportunities in
a model restricted to one attempt per refresh. An older frame can expire from
the selection window while the next frame is still GPU-pending; consumed frames
cannot fill that hole. The implementation's 1 ms retry may help but is not
proof that the physical refresh deadline will be met. These are selector tests,
not a DXGI/DWM/driver/monitor runtime test or a reproduction of the original fault.
