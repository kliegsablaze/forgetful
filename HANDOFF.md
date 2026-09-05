# Forgetful — maintainer handoff

Four independent tape memories on one live input, each decaying on its own
clock. A Schwung `audio_fx` module for Ableton Move.

This is what you need to know to pick it up. For what the controls *do*, read
the [manual](https://kliegsablaze.github.io/forgetful/); for why the DSP is
shaped the way it is, read [DESIGN.md](DESIGN.md). This file is about working
on it.

**As of v0.6.1** — deployed to a device and pushed to `main`, but **not tagged
and not released**: the catalog still serves 0.5.4. Since 0.5.4 this adds the
module-drawn widgets and cards, Age extending a memory's life, the reversed
window, and DUST's three further surfaces. None of it has been judged by ear.
Do that before `release.sh`.

---

## 1. Working on it

Two tracks, deliberately separate. Nothing on the first reaches anybody else.

### Local — your Move

```bash
./scripts/install.sh          # --no-reload to skip the restart
```

Builds if `dist/` is behind `src/`, copies to the Move over SSH, restarts
schwung, and **refuses to report success until the inode the process has
mapped matches the one on disk**. Touches nothing else — no catalog, no
GitHub, no release.

### Release — everybody else

```bash
./scripts/release.sh
```

The only thing in the repo that reaches Schwung Manager. Fetches and rebases
onto origin, re-runs the suite *after* the rebase, refuses a dirty tree or an
existing tag, then pushes the branch **before** the tag. Run it when you decide
to ship, never as a side effect.

### Versioning

Bump the patch version in `src/module.json` as you go. The number Schwung
Manager shows for an installed module is read from the device's own
`module.json`, not from the catalog — if you don't bump it, the Manager
confidently reports a version that isn't what's running.

A hand-deployed version sits *above* the released one, so the Store offers no
update and doesn't nag. The flip side: a version deployed and never tagged
leaves the device quietly ahead of everyone. Tag what you deploy.

---

## 2. What's where

| Path | Lines | What |
|---|---|---|
| `src/dsp/forgetful.c` | 3037 | The entire module — DSP, state machine, parameter contract, page declarations |
| `tests/test_forgetful_loopengine.c` | 2657 | Black-box bench driving the real v2 plugin API. Tests 0–41 |
| `docs/index.html` | 740 | User manual, with interactive reconstructions of the Move screen |
| `DESIGN.md` | 301 | Interaction model and DSP rationale |
| `src/canvas.js` | — | The in-grid custom widgets (Schwung 1.2 `drawCell`): Main's ECHO tanks (with their own 3x5 font), and START/END as one window |
| `src/cards.js` | — | The cards raised while Age, a Speed or FREQ is turned (`card_script`) |
| `src/help.json` | — | On-device help. Eight topics, hard 20-character line limit |
| `src/module.json` | — | id, version, `component_type: audio_fx`, capabilities |
| `scripts/` | — | `build.sh` (Docker cross-compile), `install.sh`, `release.sh`, `Dockerfile` |

---

## 3. How it works

One plugin instance holds four independent loop engines, a shared reverb and a
glitch stage.

### The idea that explains the rest

What comes off the tape each pass is **written back onto it**. The next pass
reads the damaged version and damages it again. Nothing is scripted against a
timer — the compounding *is* the evolution. That single decision is why a knob
here sets a *rate* rather than a sound, and why the damage cannot be undone.

### Signal flow, per sample, per LOOPING loop

```
read head          Warp modulation x signed rate (Speed octave
                   + FREQ semitones + direction ramp), inside
                   the [START, END) window, join crossfaded
  |
overdub            live input mixed in, walking in the
                   direction of travel
  |
Hiss               highpassed noise, scaled by age with a floor
VINYL              click/pop envelope
gate               the quiet material goes first
regulator          per-block gain solve on the mean square
  |
WRITE BACK ------- the recursion. everything above compounds.
  |
Tone               after the write-back, so it stays reversible
memory             level scaling
  |
--- the four loops sum here ---
  |
FDN reverb         Space sends; runs unconditionally so tails ring
Glitch             end-of-chain step sequencer
limiter + clip     output stage
```

Dry input passes through unconditionally in every state and is **not**
glitched — it is the live performance walking through, not a memory of it.

### Pages

Seven `ui_hierarchy` levels, so each page gets its own name and a real section
break on the bank bar.

| Level | Knobs 1–4 | Knobs 5–8 |
|---|---|---|
| `root` | Send · ECHO · REC · Freeze | speed A–D |
| `sound` | volume A–D | tone A–D |
| `loopA`–`loopD` | Age · START · END · ECHO | FREQ · Warp · Space · DUST |
| `glitch` | Mix · Step · Odds · Size | Kind · Reach · Pitch · Width |

`loopX_chaos` (VINYL) and `loopX_hiss` are still declared but are **not on any
page** — DUST writes both. They stay declared so they remain LFO and CC targets.

### What the module draws itself

Since 0.5.5, four cells are not the host's built-in widgets:

| Surface | Where | File |
|---|---|---|
| `custom:fg` widget | `master_loops_overview` on Main — four tanks, filled by memory, lettered with each loop's state as a negative | `src/canvas.js` |
| `custom:fg` widget | `loopX_start` + `loopX_end`, one viz group over both cells | `src/canvas.js` |
| card | `loopX_decay_rate` (Age) — the decay slope on a fixed 300s axis | `src/cards.js` |
| card | `loopX_speed` — the rate ladder and a direction arrow | `src/cards.js` |
| card | `loopX_freq` — semitones against the centre detent | `src/cards.js` |

Both are declared in `chain_params` and nowhere else: the chain host serves
that from the DSP's `get_param`, so a declaration added only to `module.json`
would never appear. Neither costs a knob slot — the widget registry loads
`canvas.js` purely because `chain_params` names a `custom:` kind, so no
`type: "canvas"` parameter is needed. Requires Schwung **1.2**; on anything
older both simply do not draw and the built-in widgets show instead, which is
why `min_host_version` stays at 0.12.1.

**A module gets ONE widget kind.** `registerWidget` is called once with
`canvas_overlay.widgetKind`, so both widgets share `custom:fg` and `drawCell`
branches on the viz group id.

---

## 4. Traps

The expensive things. Each cost real time to find, and most look like something
else.

### Deploying

**Copying the file is not deploying it.** The shim holds the `.so` `dlopen`'d
for as long as the module sits in a slot, and the atomic rename gives the new
file its own inode — so the process keeps executing the old one, from a file
that no longer has a name. A Move sat on the previous build through *four*
deploys, reporting the new version in `module.json` (which is just a file)
while running old code. "Swap the slot away and back" did not take.
`install.sh` now restarts and verifies the mapped inode; trust its output, not
the file's mtime.

**Never `scp` straight onto the live path.** `scp` opens the destination
`O_TRUNC` and rewrites in place, truncating the file under a live mapping.
That is a hard SIGSEGV taking Move's whole audio process down. It took three of
them, one per upload, to spot the pattern. Stage beside the target and `mv`.

**The Manager's version is the device's, not the catalog's.** For an *installed*
module it renders the version read off the Move; only modules you have not
installed show the catalog's. Seeing your dev version there does not mean
anything was published — check `release.json` and the tags.

### The parameter contract

**A `%` unit means a FRACTION.** The UI multiplies by 100 for display. Declare
`0..1`, not `0..100`. Getting this wrong put *"Trim, 5000%"* on the device and
every contract-level check passed — it was found only in screen-reader
announcements in the debug log. `test33` now fails any `%` param with `max > 2`.

**`get_param` has three answers, not two.** Text is an answer; `""` means
served-but-empty; `-1` means *the read did not complete*, and the host
**retries it**. Returning `-1` for a key you simply do not have logged 19,913
`param_giveup` events in one session, about one a second, on a screen where an
IPC read costs more than redrawing the whole display.

**The host learns an enum's wire format from `get_param`.** Return labels and
the host writes labels back. Accept labels *before* falling back to an index —
`atoi("1x")` is 1, which silently selects the wrong option on every real knob
turn while bench tests that write indices pass happily.

**`chain_params` is built into a fixed stack buffer** — currently ~8.1 KB into
12288 bytes, having gone 7276 -> 8071 when the 1.2 widget and card
declarations landed. `snprintf` truncates **silently** and the module simply
stops having a parameter contract. `test0` asserts real headroom so it fails
while there is still room to fix it.

The fixture's own buffer has to track it. `test0` read the contract into
`cp[8192]`, which the declarations came within 121 bytes of overrunning — and
`get_param` returns -1 rather than truncating, so the failure would have been
"chain_params readable", a message that says nothing about what is wrong. It
is `cp[12288]` now, matching `forgetful.c`. Grow both together.

### The draw surfaces

**A widget that does not draw looks exactly like one that does not exist.**
An unregistered `custom:` kind, a typo in the kind name, a `canvas.js` that
failed to load, a host older than 1.2, and a widget retired after throwing
(one strike, and it stays disabled for the session) all produce the SAME
screen: a correct page drawn with the built-in widgets. Nothing is missing,
nothing is highlighted, nothing complains. The only thing that separates the
five is `debug.log`:

```
widgets: forgetful declares a custom viz kind; loading canvas.js
widgets: forgetful declares a custom viz kind but no usable drawCell (...)
widget custom:fg disabled after throw: ...
```

So "I looked at it and it was fine" is not evidence the widget works. Turn the
log on (`touch /data/UserData/schwung/debug_log_on`) before believing a deploy.

**`build.sh` forgetting a JS file is silent too**, for the same reason —
which is why the `cp` lines carry a comment saying so.

**A card cannot see its neighbour.** The drawer is handed `{w, h, name, value,
raw}` for one parameter and nothing else, with no `getParam` on the draw path.
That is why Speed and FREQ are two cards and not one tuning picture, and it is
not worth rediscovering by trying.

**`o.raw` may be null** — the module did not answer that read. Every drawer
prints `--` and stops. A bar at zero is indistinguishable from a genuine zero,
so a read that did not answer must never become a picture.

### The DSP added since 0.5.4

- **Age is time REMAINING, not a rate.** Turned up it restarts the clock and
  lifts `memory` back to full over `MEMORY_LIFT_SECONDS`; turned down it is a
  rate change and nothing else. Glided, not written: `memory` scales the
  loop's output level, so assigning it is a click.
- **The window has a direction.** `|END - START|` whichever way round, with
  its own glide (`WINDOW_FLIP_SECONDS`) through zero. It has its OWN direction
  state on purpose — `get_param("speed")` reads the sign of
  `speed_dir_target` to name the selected option, and the host learns the enum
  wire format from that read.
- **DUST's further reaches** (burst / dirt / grains) hang off DUST's own
  slewed left-travel, NOT `applied_crackle`. That variable is `loopX_chaos` —
  VINYL's parameter, and an LFO and CC target — so keying them off it changed
  what VINYL means and broke test23's premise. All three sit AFTER the
  write-back: a distortion inside the recursion squares itself every pass.

### Tests that cannot see the bug

This is the recurring failure mode in this project. Three separate times a test
passed against code that was visibly broken, because the fixture could not
express the fault.

**A 440 Hz sine hides both a filter and a click.** A pure tone has almost no
high-frequency content, so a lowpass barely moves any spectral measure — a
working filter looks broken and a broken one looks fine. And a sine's loop seam
happens to join at a similar phase, so deleting the reverse join crossfade
moved the worst sample step only 805 → 867. On a full-scale ramp the same
deletion reads 153 → 13384.

**`TEST_BUFFER_CAPACITY_FRAMES` is sixty seconds.** Fill the buffer, play for
ten seconds, and the read head never reaches the loop seam — so a join bug is
invisible in either direction. Record a short take and close it by hand when
the seam is what you are testing.

**The level regulator fakes your measurement.** Measuring the played value
range of a ramp said the START/END window was 20% too wide. It was not — the
regulator boosts a narrower window back up, so the measurement was reading
gain. Loop **period** is the regulator-proof measure.

**Four of the DUST assertions were blind, and each for a different reason.**
Worth reading before writing another: comparing total energy across knob
settings measured the crackle underneath, not the surface being named —
deleting the grains entirely left it green. Writing `loopX_chaos` AFTER
`loopX_dust` pins the click stage so two runs differ by exactly one surface,
and that is what made them real. A soft clipper cannot be measured by energy
at all (it takes peaks DOWN), nor by crest while clicks are audible (clicks
are added after the shaper and set the peak — crest went UP across the
threshold). And a threshold is only meaningful on the material it was
measured on: the grain-decay check read above its threshold on a 10Hz ramp
whether the envelope worked or not, and only discriminates on a sine.

**Some things are honestly not pinned, and should say so.** That the grain
envelope LENGTHENS with the knob reads within a few percent of the real thing
on energy, crest and block-energy variance alike. Rather than tune a brittle
threshold, the test says it is a judgement for the ear.

**Check the EXIT CODE when you mutate, not just for a `FAIL` line.** `-Werror`
turns some mutations into compile errors, and a grep for `^FAIL` then finds
nothing — which reads as "not caught" and is the same blind-pass shape as the
bug below.

**A green suite is not a green suite.** The runner grepped for a `PASS:` line
that printed unconditionally. Before that, `grep -c '^FAIL'` counted compile
errors as success — the suite had not compiled for some time and three real
failures were hiding, one of which had never once run. The binary now exits
non-zero and `set -e` makes the exit code authoritative.

### Things that look wrong and are not

- **`FRQ`, not `FREQ`** — the host's fleet-wide `WORD_ABBREV` maps
  `freq → FRQ` so every module's frequency control reads alike.
- **The FDN reverb runs unconditionally** — 3.8 µs/block of always-on cost, and
  it stays. Tails must keep ringing after Space is pulled back; gating it needs
  an energy tracker that could cut a tail short.
- **The module refuses to persist** — `get_param("state")` answers `"{}"` and
  `set_param("state")` is ignored, by design, so a take is always a
  performance. The cost is real: `<prefix>:state` is the same channel used by
  User Presets and chain patches, so neither can capture this module.
- **Warp and the surface noise have separate RNG streams** — they shared one,
  so gating the noise work also shifted Warp's drift. Keep them separate.
- **Flavour smoothing must never *complete*** — it was a linear ramp that
  finished in 40 ms and stopped dead, so between knob writes the value sat
  perfectly flat: 73% of blocks frozen, a staircase, and the corners are what
  the ear hears. It is a one-pole now; it never arrives, so it never stops.
- **The help file's 20-character limit is invisible** — the first draft had
  eleven lines at 21 characters. Validate `src/help.json` with a script, never
  by reading it.

---

## 5. Testing

```bash
bash tests/run.sh          # exits non-zero on failure
```

A black-box bench driving the real `create_instance` / `set_param` /
`process_block` API exactly as the chain host would. It builds with
`-Wall -Wextra -Werror`, which is stricter than `build.sh` — dead code the
module build accepts will fail here, and that is a feature.

**Mutate the code and confirm the test fails.** Every behavioural test in this
suite has been verified that way, and it has repeatedly caught tests that
passed for the wrong reason. Writing a test that goes green tells you almost
nothing; watching it go red when you break the thing tells you what it actually
pins.

Two harnesses beyond the suite:

- **Output fingerprinting.** Hash the rendered output across a battery of
  parameter settings before and after a change and require byte-identical
  results. That is how the optimisation pass proved it changed nothing.
- **Offline page rendering.** Schwung ships `tools/param-pages/preview.mjs`.
  Inject the module's live contract into `tests/fixtures/module-contracts.json`
  and render any page to PNG — far faster than a device round-trip, and it
  catches label truncation and cell overflow.

  **It does not load a module's `canvas.js`.** Widget registration lives in
  `shadow_ui.js`, which the preview never runs, so out of the box it renders
  Forgetful's two custom cells as the built-in widgets — the same picture as
  every failure mode above. Register the widget yourself first:

  ```js
  // preload.mjs, beside the schwung checkout
  import { readFileSync } from "node:fs";
  import { registerWidget } from "./src/shared/param_pages/widget_registry.mjs";
  (0, eval)(readFileSync("<forgetful>/src/canvas.js", "utf8"));
  const ov = globalThis.canvas_overlay;
  registerWidget(ov.widgetKind, { draw: ov.drawCell.bind(ov) });
  ```

  ```bash
  node --import ./preload.mjs tools/param-pages/preview.mjs forgetful \
      --layout movy --page 0
  ```

  Note the preview synthesises values (`tools/param-pages/fake_values.mjs`)
  with no idea what they mean — it hands START 0.696 and END 0.457, so the
  window correctly draws as two marks and no fill, and it hands the overview
  `"1"`, which is not a four-character code and correctly draws as four empty
  slots. Both are the widgets working. Patch `fakeValue` locally if you want
  to see them carrying real state.

  `resolveViz` is worth driving directly too: it answers the one question the
  pictures cannot, which is whether the host CLAIMS those cells at all, and
  it answers it both with the widget registered and without.

---

## 6. Open items

Nothing is blocking. These are the known loose threads.

- **Reset-to-default gesture.** Asked for, then deferred. Cannot live in the
  module — it never sees a knob touch. Needs a host change, and `default` is
  declared by ~330 params across the fleet and read by *nobody*, so whoever
  plumbs it gets the whole catalog at once. The gesture must not be a plain
  hold; that is taken.
- **The sound is unjudged by machine.** Everything verified here is measurable
  — ratios, click amplitudes, decay curves. Whether it sounds *good* has only
  ever been judged by ear on the device. Budget for that on any DSP change.
- **`min_host_version` is conservative.** Set to `0.12.1` by assumption, not
  measurement. It is a plain v2 `audio_fx` and may well run on older hosts.
  It stays there despite the 1.2 draw surfaces, deliberately: both degrade to
  the built-in widgets on an older host, so raising it would lock people out
  of a module that works for the sake of pictures they merely would not see.
- **Hiss floor is tunable.** `HISS_AGE_FLOOR` is 0.15. 0.20 is the next step up
  and the sensible ceiling — beyond that a fresh loop starts out noisier than a
  dead one, which inverts the idea.

---

## 7. Reference

### Constants worth knowing

| Name | Value | Governs |
|---|---|---|
| `FLAVOUR_SLEW_TAU_S` | 0.060 | One-pole time constant for Warp/DUST. Must not become a completing ramp |
| `HISS_AGE_FLOOR` | 0.15 | How present hiss is on a fresh take |
| `HISS_CEILING` | 0.020 | Hiss level at full decay, knob maxed. Reported "too loud" twice historically |
| `GATE_MIN_PER_PASS` | 0.85 | Floor of the VINYL gate. One of the two erosion dials |
| `SPEED_GLIDE_SECONDS` | 5.0 | Speed and direction glide. A direction flip passes through zero |
| `FREQ_GLIDE_SECONDS` | 0.05 | FREQ, deliberately fast — you tune by ear against another loop |
| `MAX_LOOP_VOLUME` | 2.0 | 200%. The declared max derives from this — do not hand-write it twice |

### Links and access

- Repo — <https://github.com/kliegsablaze/forgetful>
- Manual — <https://kliegsablaze.github.io/forgetful/>
- Host — <https://github.com/charlesvestal/schwung>, docs in `docs/MODULES.md`
  and `docs/PARAM_PAGES.md`
- Device — `ssh ableton@move.local`; root for `/proc` and service control
- Logs — `touch /data/UserData/schwung/debug_log_on`, then tail `debug.log`

**Never write to `/tmp` on the Move.** The root filesystem is ~463 MB and
usually full. Use `/data/UserData/` for everything.
