# Forgetful - Design Document (v8 - reflects the shipped v1 implementation)

**Module ID:** `forgetful`
**Component type:** `audio_fx` (chainable - lives in a Signal Chain fx1/fx2 slot,
uses only the standard knob-turn + jog-page-scroll interaction, no raw MIDI /
custom touch gestures needed)
**One-line pitch:** One live input, sent by hand into whichever of four tape
memories you choose, each one already forgetting itself the moment you move
on - drifting out of tune, going dark, hissing, breaking up, until it's gone.
Mix the four together and you're performing with your own recent past, a
little more blurred each time you glance back at it.

This document was rewritten 2026-08-25 against the actual shipped `forgetful.c`
after an extended on-device tuning session diverged the implementation from
the original v7 plan on nearly every axis: routing lost its `None` state,
recording became a manual gesture instead of level-triggered, erase lost its
double-click confirm in favor of a 10-second fade, overdub shipped, the flavor
knobs became turn-based rather than randomized-then-live, Darken became a
reverb wash, Glitch became VINYL crackle, and the whole UI vocabulary went
through a poetic-naming pass. The sections below describe what is actually
running, not what was planned. History worth knowing is called out inline
rather than kept in a separate "changed in vN" scaffold, since there's no
longer a next planned version to diff against. This module was developed
inside the main [Schwung](https://github.com/charlesvestal/schwung) repo and
split out into its own repo once v1 stabilized; see that repo's history up to
the split for the day-by-day trail, and this repo's own history afterward.

## Concept

One live input. Four small tape memories. You decide, moment to moment,
which one is currently listening - turn Send to point the input at A, play,
turn it to B, keep playing while A quietly starts forgetting itself
underneath, bring in C, glance back and mix in what's left of A. Nothing you
send into stays sharp for long: every flavor knob starts at zero the instant
a loop closes, and only builds as you turn it - so a loop's character is
something you actively play into it over its lifetime, not something
imposed on you at random.

## Interaction Model

**Five pages, navigated via jog wheel scroll:** Main (Master), then A, B, C, D.
The root page is always titled "Main" by the shared page planner regardless
of any label declared here (fleet-wide convention); the four loop pages are
titled just their own letter.

### Page 0 - Main

| Knob | Key | Label | Behavior |
| --- | --- | --- | --- |
| 1 | `input_routing` | **Send** | `enum`, options `A`/`B`/`C`/`D`, no `None` - default `A`. Determines which loop the live input currently feeds. Divable (touch + jog-click opens a scrolling picker); turning also steps it one option at a time. |
| 2 | `master_loops_overview` | **ECHO** | Read-only (`access: "read"`) compact status readout, one character per loop in A/B/C/D order: `-` idle/forgotten, `R` recording, `O` overdubbing, `F` frozen, else a digit (memory rounded down to the nearest 10%) - e.g. `7R-1` means A is 70-79% remembered, B is recording, C is idle, D is 10-19% remembered. Declared as an `enum` (not `string`) so it renders through the enum-square glyph renderer, which gives it two lines of 3 characters instead of a single ~2-character truncated line. |
| 3 | `master_record` | **REC** | Trigger (`access: "write"`). See "Recording, catching, overdubbing" below. |
| 4 | `master_freeze` | **Freeze** | Trigger (`access: "write"`). Toggles `frozen` on the routed loop. |
| 5 | `loopA_volume` | **A** | Continuous, 0-1, default 0.8 |
| 6 | `loopB_volume` | **B** | Continuous, 0-1, default 0.8 |
| 7 | `loopC_volume` | **C** | Continuous, 0-1, default 0.8 |
| 8 | `loopD_volume` | **D** | Continuous, 0-1, default 0.8 |

No blank/reserved knobs on this page - all eight are real params.

### Page 1-4 - A / B / C / D

| Knob | Key | Label | Behavior |
| --- | --- | --- | --- |
| 1 | `loopX_decay_rate` | **Age** | Continuous, 3-300 (seconds), default 300 (full/max). How long the loop takes to fade to silence, linearly, once it starts LOOPING. Also the clock the five flavor knobs' turn-based ramps are timed against - see "Flavor knobs" below. |
| 2 | `loopX_saturation` | **Drive** | Continuous, 0-1, default 0.25. Tape warmth/saturation. The one flavor knob that is NOT turn-based - see below. |
| 3 | `loopX_state` | **ECHO** | Read-only, same single-character code as Main's `master_loops_overview`, for this one loop. |
| 4 | `loopX_erase` | **Erase** | Trigger. See "Erase" below. |
| 5 | `loopX_wow` | **Warp** | Continuous, 0-1, default 0 (untouched). Wow/flutter pitch modulation of the read head. |
| 6 | `loopX_hf_loss` | **Darken** | Continuous, 0-1, default 0 (untouched). Drives a reverb wash - see "Darken" below. |
| 7 | `loopX_chaos` | **VINYL** | Continuous, 0-1, default 0 (untouched). Vinyl surface crackle - see "VINYL" below. Wire key is still `chaos` (unchanged since the Glitch-era chaos-gate this replaced, to avoid autosave churn); the C field is named `crackle`. |
| 8 | `loopX_hiss` | **Hiss** | Continuous, 0-1, default 0 (untouched). Tape hiss. |

Knob order here (Age, Drive, ECHO, Erase, Warp, Darken, VINYL, Hiss) is the
result of two separate on-device reordering requests and does not mirror the
signal's own processing order (Warp -> Darken -> Drive -> Hiss/VINYL mixed
in) or any other principle beyond "this is the layout that was asked for."

## Recording, catching, overdubbing

There is one shared live input. Only the loop currently selected by Send can
receive it. Recording is entirely **manual** - there is no level-triggered
auto-record and no silence timeout. (An earlier level-detection auto-trigger
shipped first and was replaced: real playing levels didn't reliably cross any
single fixed threshold, and a trailing silence-timeout baked an audible pause
into the start of every loop.)

`master_record`'s trigger cycles the routed loop through four gestures, and
its live readout names **what the next press will do** (a transport-button
convention, like a play/pause button reading "Pause" while playing) rather
than what's currently happening - this is deliberate and entirely a function
of current state, so it needs no memory of how that state was reached:

| Loop state | Readout | Next press does this |
| --- | --- | --- |
| Idle / Forgotten | **REC** | Starts recording |
| Recording | **STOP** | Closes the take, loop starts LOOPING |
| Looping, not overdubbing | **DUB** | Starts overdubbing |
| Looping, overdubbing | **PLAY** | Stops overdubbing, back to plain playback |

**Overdubbing** layers new dry input into the *existing* buffer at the
loop's own current playback position (nearest sample, hard-clipped int16 sum
- not interpolated across the fractional read position, so with Warp active
the mapping between "what you played" and "where it landed" gets uneven,
since a modulated read speed can revisit or skip buffer positions across a
pass). Overdubbing does **not** touch memory, the flavor knobs, reverb tail,
or hiss coloring state at all - only a real erase resets those (see below).
Routing away from an overdubbing loop stops the overdub, for the same reason
routing away from a Recording loop closes it: the shared dry input now
belongs to whatever loop Send points at next, so an orphaned overdub can't be
left silently writing the new loop's audio into the old one's buffer.

Buffer-full (60s) remains an automatic safety-cap close during Recording, so
forgetting to press REC again can't record forever. It's the one automatic
close condition left; everything else about starting, stopping, and
overdubbing is a deliberate press.

## Erase

Single click, no confirm (an earlier double-click-confirm was removed:
touching the knob AND jog-clicking it is already a deliberate two-part
gesture, so a second click on top of that was redundant caution rather than
real safety). Fires on any non-idle-spelling write, same convention as every
other trigger in this module.

- **Idle / Recording:** clears instantly. Idle has nothing to fade; Recording
  is an unfinished take being discarded outright, not a loop being let go of.
- **Looping (frozen or not):** fades out over 10 seconds
  (`ERASE_FADE_SECONDS`) via a per-loop `erase_fade_gain` multiplier (1.0 ->
  0.0) applied on top of the ordinary memory-based level, then clears for
  real once the gain reaches 0. This runs identically whether the loop is
  frozen or not - freezing suspends memory decay and the flavor ramps, not
  the erase fade, which has its own independent guard. ECHO shows the loop's
  ordinary state characters throughout the fade (`F` if frozen, a memory
  digit otherwise) and flips to `-` the instant it clears. A fresh `master_record`
  press claims a loop mid-erase-fade immediately, rather than waiting out the
  fade.

The `loopX_erase` knob's own live readout is always **ERASE** (not
state-aware - the loop's own status line already carries "Erasing..." as an
unconstrained full-text readout for that case, and "ERASING" itself is too
long for the 5-character budget these short readouts live under - see
"Naming, and why some labels are longer than others" below).

## Flavor knobs: turn-based, not automatic

Warp, Darken, Hiss, and VINYL share one mechanic ("v2", replacing an earlier
"v1" scheme where every knob auto-chased toward its raw value at a constant,
Age-derived rate the instant a loop closed). Each is backed by a small
`flavor_ramp_t { target, step, touched }`, not a plain float:

- **First turn since the take started** jumps the audible value straight to
  what you set, with no ramp at all.
- **Every turn after that** starts a fresh linear ramp from wherever the
  audible value currently sits to the new target, timed to land exactly when
  *this loop's own remaining time until silence* runs out - `memory *
  decay_rate` seconds from the moment of that turn, not `decay_rate` itself.
  A turn made 90% of the way through a fade arrives proportionally sooner
  than one made right after recording. Nothing moves between turns.
- **Every take starts completely untouched** - a knob dialed in on a
  previous take has no effect on the next one until you turn it again.

**Drive is the one exception**, deliberately: it keeps the older
auto-chase-toward-its-live-value behavior at a constant `decay_rate`-derived
rate, not the turn-based scheme.

**While frozen**, every write to any of the five flavor knobs (Drive
included) starts a short, fixed 150ms glide (`FROZEN_GLIDE_SECONDS`) instead
of either the remaining-time ramp (meaningless while memory isn't draining)
or an instant jump (reads as a snap, not a knob being played). Retriggered on
every write, so turning the physical knob at a normal pace - a steady stream
of small writes - feels like continuous live tracking. It still marks the
knob `touched`, so unfreezing "resumes normal mode with the new initial
value at whatever the frozen turns left it at" - the next turn after
unfreezing ramps from there, not from 0.

## Darken: a reverb wash, not a filter

Darken used to be a simple one-pole lowpass and was reported as "almost
inaudible even at maximum" - a straight lowpass on a tape loop just reads as
"slightly duller." It now drives a scaled-down Schroeder-Moorer reverb (4
combs + 2 allpass per channel per loop, adapted from Schwung's own
`freeverb.c` audio_fx module, which uses 8+4 - halved because up to four of
these can run concurrently, one per loop, against a single freeverb
instance). Three things scale together off the one `applied_hf_loss` value:
wet-mix amount, internal damping (how dark the tail sounds), and feedback
(how long the tail rings - "into a wall of reverb" at high settings, mapped
through freeverb's own `room_size -> feedback` formula, `feedback = 0.70 +
wet*0.28`). At `applied_hf_loss = 0` this is an exact dry passthrough.

## VINYL: crackle, not glitch

VINYL replaced an earlier "Glitch" control that gated the whole loop's
output to silence at random intervals (a chaos-gate dropout) - reported as
producing unintended silences from ordinary Move surface input, and
generally not reading as musical. It's now a Poisson click/pop process (the
standard technique for vinyl-surface noise): a "dust" density (frequent,
quiet) and a "pop" density (rare, louder), both mixed in additively
alongside Hiss rather than muting anything. The knob has a two-stage
mapping: the bottom half only raises volume (density held at a fixed low
baseline the whole time); the top half continues raising volume while also
raising density up to its own ceiling. Gain and density constants have been
cut twice since the initial rebalance in response to on-device "way too
loud" reports.

## Hiss

A highpassed noise (a slow lowpass of raw bipolar noise, subtracted back out
- the complementary-filter trick, applied to the noise source rather than
the signal) rather than raw broadband noise, which read as flat static
rather than tape hiss. Ceiling has been cut twice from its initial value in
response to on-device loudness reports.

## Freeze

`master_freeze` toggles `frozen` on the routed loop. While frozen: memory
decay and Drive's normal auto-chase both suspend, so the loop keeps looping
audibly at whatever degradation it had already reached, indefinitely. The
five flavor knobs stay fully live during freeze (see "Flavor knobs" above) -
freeze is meant as a way to park a loop's aging and sculpt its character by
hand, not to silence knob input. Readout follows the same "what will the
next press do" convention as `master_record`: **FREEZE** while unfrozen
(next press freezes it), **AGING** while frozen (next press unfreezes it,
resuming aging).

## Naming, and why some labels are longer than others

The button/knob NAME text (e.g. "Send", "REC", "Age") is capped at 5
characters (`LABEL_CHARS` in Schwung's `render_page_movy.mjs`) - this is a
hard, measured limit in the host. The live VALUE text shown in the header
while a knob is held (REC/STOP/DUB/PLAY, FREEZE/AGING, the ECHO characters,
etc.) turned out to share that same 5-character budget in practice,
discovered the hard way across several rounds: PLAYING (7), RELIVE/AGEING (6
each), and CAPTURE (7, still unconfirmed as of this writing - the next word
to check if it's ever reported truncating) all read as cut off on-device
even though nothing in `forgetful.c` enforces that limit for VALUE text the
way `LABEL_CHARS` enforces it for NAME text on the host side. Every current
value word is 5 characters or fewer.

Several rounds of on-device naming feedback moved a few things repeatedly:
the record button went Rec -> Catch -> Moment -> HOLD -> REC; its readout
vocabulary was rebuilt at least three times, most recently onto the "what
will the next press do" convention above (see git history for the earlier
CAPTURE/RELIVE/LIVE/BLISS and REC/OVERDUB/-/PLAYING iterations, none of
which are live any more). Master's status overview and each loop's `state`
knob both went Status -> State -> Memory -> ECHO. Loop page titles went from
"Loop A".."Loop D" to just "A".."D". None of this affects wire keys - only
declaration order, display names, and get_param's readout text moved; every
rename was confined to string literals in `chain_params`/`ui_hierarchy`/
`get_param`.

## State Machine (per loop, x4 independent instances)

```
IDLE ──(master_record fired while routed here)──> RECORDING
RECORDING ──(master_record fired again, OR buffer_seconds reached,
             OR Send moves away from this loop)──> LOOPING
LOOPING ──(memory reaches 0)──> FORGOTTEN ──(auto, same block)──> IDLE
LOOPING ──(loopX_erase fired)──> LOOPING with erasing=1, fading
  ──(erase_fade_gain reaches 0)──> IDLE (hard clear)
IDLE / RECORDING ──(loopX_erase fired)──> IDLE (instant clear)
```

`frozen` and `overdubbing` are orthogonal boolean flags that only have any
effect while `state == LOOPING`; they are not states of their own. Decay,
the flavor ramps, and forgetting all progress regardless of Send's current
position - a loop that isn't routed keeps aging exactly as if it were.

## Screen / Feedback

Main's ECHO readout and each loop's own `state` knob share one compact
per-loop code (see Page 0 table above). Each loop's `loopX_status`
(unconstrained full text, not shown on a knob cell but readable via
get_param) still carries `Ready` / `Recording` / `Looping - NN% (word)` /
`Forgotten` / `Erasing...`; the memory-percentage-to-word mapping is
unchanged: 90-100% "Vivid", 40-89% "Fading", 10-39% "Hazy", 1-9% "Almost
gone".

## DSP Architecture

One plugin instance internally manages four independent `loop_engine_t`s
plus one shared input router (`input_routing`) and one shared block-scoped
dry passthrough.

### Signal flow per block, per LOOPING loop
1. Wow/flutter-modulated read-head advance (`applied_wow` driven speed
   modulation).
2. If overdubbing: mix live dry input into the buffer at the nearest sample
   to the read position.
3. Interpolated buffer read.
4. Darken: reverb wash (wet/damp/feedback all from `applied_hf_loss`).
5. Drive: crossfade between the Darken-stage output and a fixed full-drive
   tanh curve, scaled by `applied_saturation`.
6. Hiss: highpass-colored noise added, scaled by `applied_hiss`.
7. VINYL: click/pop envelope added, volume and density both driven by
   `applied_crackle` via the two-stage mapping.
8. Level scaling: multiply by `memory` and `erase_fade_gain`.
9. (Not per-loop) Master-page `loopX_volume`, applied once after summing all
   four loops' wet contributions.
10. Chaos gate is gone (see "VINYL" above) - nothing left in this list
    multiplicatively silences the loop's output at random.
11. Read-head wrap; memory decay and the five flavor-knob ramps advance
    together, guarded by `!erasing` (and, for memory/Drive specifically, also
    `!frozen`) - see "Flavor knobs" and "Freeze" above.
12. Erase-fade-gain progression, guarded only by `erasing` - runs regardless
    of `frozen`.

Dry input passes through unconditionally in every state, added once per
sample; all four loops' wet contributions (each independently scaled by that
loop's Master-page volume) sum on top of it.

### Capability flags
`audio_in`, `audio_out`, `chainable: true`, `component_type: "audio_fx"`.
Fully standard `chain_params`-driven module, no raw MIDI dependency.

## Per-Module Settings (`settings-schema.json`) — still deferred

Unchanged from the original plan: no chain-embedded `audio_fx` module in
Schwung has a working path to read `config.json` at runtime
(`create_instance` runs on the SPI callback thread, where file I/O is
forbidden; the JS-side `host_read_file` path doesn't run for chain-embedded
FX at all). Every tunable in this module remains a hardcoded C constant.
Fixing this properly needs a `chain_host.c` change on the Schwung side
(reading `config.json` at `dlopen`/`create_instance` time, off the realtime
path) that's out of scope for this module and hasn't been picked up.

## Status

v1 is implemented, bench-tested (`tests/test_forgetful_loopengine.c`, run
via `tests/run.sh`), and has been through many rounds of on-device tuning
covering routing, recording, overdub, erase/freeze interaction, all five
flavor knobs, and UI naming.

## Open Questions / Risks

- **CAPTURE-length readout words**: the 5-character value-text budget was
  discovered empirically, one truncated word at a time, not from a documented
  limit. Any future readout word needs checking against it before shipping.
- **Overdub + Warp**: known rough edge, not fixed - overdubbing writes to the
  nearest sample at the loop's current read position, which drifts unevenly
  relative to what was actually played once Warp's speed modulation is
  nonzero.
- **Erase + Freeze interaction**: verified correct via direct API-level
  testing (erase fades over the full 10s while frozen, ECHO clears to `-` at
  the end) after being reported as "doesn't do anything" on-device; root
  cause of the on-device symptom, if it's still occurring, has not been
  found - it isn't reproducible at the DSP level, so it would be in
  Schwung's UI/gesture dispatch layer, which hasn't been investigated.
  Revisit if it recurs.
- **A direct Main <-> loop-page navigation shortcut** (jog-click with no
  knob touched) was investigated and found to be a moderate, not small,
  change: a knob-less jog-click already opens a fleet-wide "Sections" picker
  today (jog to scroll every level, click to jump), and a true one-click
  jump would need a new opt-in field in Schwung's shared `ui_hierarchy`
  contract, consulted by `src/shared/param_pages/page_input.mjs` before it
  falls back to that picker - shared host infrastructure, not something this
  module's own repo can implement alone. Deliberately not implemented
  without that design work happening on the Schwung side.
- **`settings-schema.json`**: still fully deferred, same reasoning as before
  the rewrite - see "Per-Module Settings" above.

## Build/Test Plan

All steps from the original plan are complete: single-`LoopEngine` DSP,
generalization to four loops plus the shared router, full `chain_params`/
`ui_hierarchy` declarations, bench coverage (18 tests as of this writing:
shape, manual record/overdub, decay timing including continuous per-sample
decay, erase fade-out, routing, status-line formatting, parameter clamping,
state round-trip, saturation passthrough, freeze, the v2 flavor-ramp
first-touch/second-touch mechanic), and extensive on-device tuning. Not yet
packaged as a release tarball or submitted to the module catalog.
