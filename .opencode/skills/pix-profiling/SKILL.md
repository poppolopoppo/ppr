---
name: pix-profiling
description: >
  GPU performance capture and analysis for app.game via pix-mcp: bounded
  profiling runs, GPU capture, frame triage, and D3D12 validation.
---

# PIX Profiling (GPU)

GPU-only profiling workflow for `app.game`. CPU timing captures are
explicitly out of scope: pixtool cannot open them programmatically
(`PIXTOOL6 - Unsupported file type`), so they are a manual PIX UI task.

## 0 Scope and prerequisites

- Use for: GPU pass costs, draw/barrier counts, frame structure, D3D12
  validation of `app.game` changes.
- Build: `msvc-profile` preset, `app.game` target (see `build-system` for
  configure/build ownership). The preset sets `USE_PIX=1` (PIX markers) and
  `PPR_LOGGING_LEVEL=EMPHASIS`.
- Check `pix_status` first: `pixtool.exe` must be found. GPU capture needs no
  admin. Capture *analysis* verbs (`save-event-list`, counters, screenshots,
  `run-debug-layer`) need Windows Developer Mode.
- Do not route elevation, UAC, scheduled tasks, or timing-capture automation
  through this skill. There is no supported programmatic timing-analysis path.

## 1 Bounded runs

The game loop exits only on window close, so profiling runs must bound
themselves. Flags live in the hand-rolled CLI loop in `game/main.cpp`
(supports `--flag value` and `--flag=value`; invalid numerics print
`[cli] ...` to stderr and exit `2`):

| Flag | Effect |
|---|---|
| `--auto-exit S` (alias `--exit-after S`) | Wall-clock exit after S seconds via `TimerManager::scheduleAfter` in `TurboLarbin::initialize()`; graceful `requestExit()`, never `std::exit` |
| `--profile-csv N` (+ `--profile-skip K`, `--profile-label`, `--profile-edits`, `--profile-pan`, `--profile100`) | Existing CSV profiling machinery; `--profile-csv N` also `requestExit()`s after N measured frames |
| `--no-focus-throttle` | Bare flag; also **auto-implied** by `--auto-exit`, `--profile-csv`, `--profile100` |

Focus rule: an unfocused window is throttled to 5 fps
(`ApplicationEditor::onMainWindowFocused_` → `setBackgroundPriority`).
Profiling/bounded runs disable this automatically; a bare interactive run
left unfocused will take ~5x longer wall-clock. `setTargetFrameRate(0)` does
not defeat the background throttle — only the opt-out does.

## 2 Capture

One-shot GPU capture of a single frame (pixtool closes the app promptly,
so no exit flag is needed here):

```js
pix_gpu_capture_launch({
  exe_path: "E:\\Code\\ppr\\out\\build\\msvc-profile\\game\\app.game.exe",
  frames: 1,
  output_path: "E:\\Code\\ppr\\out\\pix-manual\\gpu-test.wpix",
})
```

Constraints learned the hard way:

- Do **not** pass dash-prefixed args (`--auto-exit`, …) to the launch:
  pixtool rejects `--command-line` values starting with `-`/`+` and may
  ignore them. For captures that need more than one frame, stage a bounded
  run separately instead.
- Save captures under `out/pix-manual/` (git-ignored scratch). Reference
  run from this repo: 11 draws, 77 events, `GridPass → DrawInstanced`
  ~101µs dominating, UI pass ~24µs.

## 3 Analyze (in order)

1. `pix_analyze_frame({capture_path, include_counters: true})` — first
   triage: draw/dispatch/copy counts, render-target changes, barriers, and
   `top_events` ranked by GPU time. Without counters there is no timing
   column.
2. `pix_get_event_list({capture_path, limit, response_format: "summary"})` —
   paginated D3D12 call stream (`Queue ID, Parent, Name, Global ID`);
   hierarchical (`ppr → GridPass → state setup → Draw`). Save full CSV via
   `output_path` for large frames.
3. `pix_list_counters({capture_path})` — available GPU counters
   (EOP timings, IA/PS invocations, samples, …).
4. `pix_run_analysis({capture_path})` — D3D12 debug-layer replay; gate on
   `error_count == 0`.

Known gap: `pix_get_screenshot` replay can time out on some captures
(observed 2× on an 11-draw frame). For pixels, open the `.wpix` in PIX UI
(`pix_open_capture`) instead of retrying indefinitely.

## 4 Regress

`pix_compare_captures({capture_a, capture_b})` — size/modification
comparison for before/after change validation. Keep baseline captures next
to the change under test; do not commit `.wpix` files.

## 5 Troubleshooting

- App never exits: no bound flag passed, or an old binary predating
  `--auto-exit` is running — check `Get-Process -Name app.game`.
- Run takes ~5x expected: window lost focus without a profiling flag;
  rerun with any flag from §1.
- `PIXTOOL6 - Unsupported file type`: a timing capture was passed to a
  GPU-analysis verb — out of scope, use PIX UI.
- Empty `top_events`: rerun step 1 with `include_counters: true`.
