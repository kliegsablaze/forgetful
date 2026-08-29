# Forgetful - Design Document

Reflects the shipped implementation as of v0.5.1.

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
recording became a manual gesture instead of level-triggered, overdub
shipped, the flavor knobs became turn-based rather than randomized-then-live,
and the whole UI vocabulary went through a poetic-naming pass. It has been
kept current since; Erase, Drive and Darken were all removed later, and
Trim became START and END. The sections below describe what is actually
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

**Seven pages, navigated via jog wheel scroll:** Main, Sound, Loop A-D, and
Glitch. Each is its own `ui_hierarchy` level, so each gets its own name and a
real section break on the bank bar. The root page is always titled "Main" by
the shared page planner whatever label is declared (fleet-wide convention).

### Main (level `root`)

| Knob | Key | Label | Behavior |
| --- | --- | --- | --- |
| 1 | `input_routing` | **Send** | `enum` `A`/`B`/`C`/`D`, default `A`. Which loop the live input feeds. |
| 2 | `master_loops_overview` | **ECHO** | Read-only (`access: "read"`). One character per loop in A/B/C/D order on a single line: `-` idle/forgotten, `R` recording, `O` overdubbing, `F` frozen, else a digit. The digit counts **9 down to 1 and never 0** — `0` and `O` are the same glyph in this font, so a loop one tick from death would look like one being overdubbed. |
| 3 | `master_record` | **REC** | Trigger (`access: "write"`). See "Recording, catching, overdubbing". |
| 4 | `master_freeze` | **Freeze** | Trigger (`access: "write"`). Toggles `frozen` on the routed loop. |
| 5-8 | `loopX_speed` | **A**-**D** | `enum`, eight options in knob order: `2x 1/4x 1/2x [1x] -1x -1/2x -1/4x -2x`. The four negatives play backwards. See "Speed and FREQ". |

### Sound (level `sound`)

| Knob | Key | Label | Behavior |
| --- | --- | --- | --- |
| 1-4 | `loopX_volume` | **A**-**D** | 0-2 (0-200%), default 0.8. Drawn as faders — the host's `detectFader` matches the `_volume` key. |
| 5-8 | `loopX_tone` | **A**-**D** | -1..+1, default 0 (bypass). Left low-passes, right high-passes. Sits AFTER the write-back and BEFORE the wash and crackle, so it is reversible and surface noise stays bright with the filter closed. |

### Loop A-D (levels `loopA`..`loopD`)

| Knob | Key | Label | Behavior |
| --- | --- | --- | --- |
| 1 | `loopX_decay_rate` | **Age** | 3-300 seconds, default 300. How long the loop takes to fade to silence. The clock every rate knob runs against. |
| 2 | `loopX_start` | **START** | 0-1, default 0. Where the loop begins in the take. |
| 3 | `loopX_end` | **END** | 0-1, default 1. Where it ends. Together these give any WINDOW of the take, which one bipolar Trim could not. |
| 4 | `loopX_state` | **ECHO** | Read-only, the same single character as Main's overview, for this loop alone. |
| 5 | `loopX_freq` | **FREQ** | -12..+12 semitones, default 0. Fine varispeed. Renders as `FRQ` — the host's fleet-wide `WORD_ABBREV`. |
| 6 | `loopX_wow` | **Warp** | 0-1, default 0. Wow/flutter/drift on the read head. |
| 7 | `loopX_send` | **Space** | 0-1, default 0. Post-fader send into the shared FDN reverb. |
| 8 | `loopX_dust` | **DUST** | -1..+1, default 0. Both surface noises on one bipolar knob — see "DUST". |

`loopX_chaos` (VINYL) and `loopX_hiss` remain declared in `chain_params` but
are **not on any page**: DUST writes both. They stay declared so they remain
LFO and CC targets and can still be driven individually.

### Glitch (level `glitch`)

End-of-chain step sequencer of destructive effects, after the send reverb and
before the output limiter. Top row is the sequencer (`mix`, `step`, `odds`,
`size`), bottom row the character (`kind`, `reach`, `pitch`, `width`).
`kind` is Tumble / Stutter / Rewind / Tape / Gate / Crush. Mix defaults to 0
and `odds` is a probability, so BOTH must be up before anything happens —
which is what makes the page safe to leave loaded.

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
or hiss coloring state at all - only a new take resets those.
Routing away from an overdubbing loop stops the overdub, for the same reason
routing away from a Recording loop closes it: the shared dry input now
belongs to whatever loop Send points at next, so an orphaned overdub can't be
left silently writing the new loop's audio into the old one's buffer.

Buffer-full (60s) remains an automatic safety-cap close during Recording, so
forgetting to press REC again can't record forever. It's the one automatic
close condition left; everything else about starting, stopping, and
overdubbing is a deliberate press.

## Flavor knobs: turn-based, not automatic

Warp, Hiss and VINYL share one mechanic ("v2", replacing an earlier
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

**While frozen**, every write to any flavour knob starts a short, fixed 150ms glide (`FROZEN_GLIDE_SECONDS`) instead
of either the remaining-time ramp (meaningless while memory isn't draining)
or an instant jump (reads as a snap, not a knob being played). Retriggered on
every write, so turning the physical knob at a normal pace - a steady stream
of small writes - feels like continuous live tracking. It still marks the
knob `touched`, so unfreezing "resumes normal mode with the new initial
value at whatever the frozen turns left it at" - the next turn after
unfreezing ramps from there, not from 0.

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

## Hiss and VINYL are one knob now: DUST

Both are the same failure of the same medium and were nearly always reached
for together, so they share one bipolar control. LEFT leads with VINYL,
RIGHT with Hiss, and past halfway each brings the other in behind it:

```
     -1.0        -0.5         0        +0.5        +1.0
  VINYL 1.0   VINYL 0.5       -            -      VINYL 0.5
  Hiss  0.5   Hiss  0        ---     Hiss  0.5    Hiss  1.0
```

so a full turn either way is all of one and HALF the other, never one alone.
DUST *writes* the two underlying params rather than replacing them.

Hiss is scaled by `age` so it builds as the take dies, but with a floor
(`HISS_AGE_FLOOR`) so the knob does something the moment it is turned.
Without the floor, hiss at full knob on a fresh take measured quieter than
VINYL (22.8 against 35.1) and the right half of DUST did nothing audible
until the loop was already well gone.

## Freeze

`master_freeze` toggles `frozen` on the routed loop. While frozen: memory
decay suspends, so the loop keeps looping
audibly at whatever degradation it had already reached, indefinitely. The
flavour knobs stay fully live during freeze (see "Flavor knobs" above) -
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
LOOPING ──(master_record fired)──> LOOPING with overdubbing=1
  ──(master_record fired again)──> LOOPING
LOOPING ──(memory reaches 0)──> FORGOTTEN ──(auto, same block)──> IDLE
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

### Signal flow per sample, per LOOPING loop

The order below is the audio path, not the knob order.

1. Read-head advance, modulated by Warp (`applied_wow`) and scaled by the
   signed rate — Speed's octave, FREQ's semitones and the direction ramp all
   fold into one multiplier. The window is `[trim_lo, trim_hi)`, derived from
   START and END, and the join is crossfaded at whichever edge is being
   approached.
2. If overdubbing: mix live dry input into the buffer at the read position,
   walking in the direction of travel.
3. Interpolated buffer read.
4. Hiss: highpass-coloured noise, scaled by `applied_hiss * HISS_CEILING *
   (HISS_AGE_FLOOR + (1-floor) * age)` — present from the start, louder as
   the take dies.
5. VINYL: click/pop envelope added, volume and density from
   `applied_crackle`.
6. Gate: the quiet material goes first, threshold rising with age squared.
7. Level regulator: per-block gain solve on the mean square, clamped, so the
   recursion neither dies nor runs away.
8. **Write-back** into the buffer — this is the recursion, and everything
   above it compounds pass over pass. Gated on `medium_active` and
   `!frozen`.
9. Tone (Sound page): after the write-back, so it is reversible, and before
   the crackle so surface noise stays bright.
10. Level scaling by `memory`.
11. (Not per-loop) `loopX_volume`, applied once after summing all four.

Everything from step 9 down is output only and never written back.

After the four loops are summed: the shared FDN send reverb (run
unconditionally, so a tail keeps ringing after Space is pulled back), then
the Glitch page, then the output limiter and soft clip. Dry input passes
through unconditionally in every state and is NOT glitched — it is the live
performance walking through, not a memory of it.

### Capability flags
`audio_in`, `audio_out`, `chainable: true`, `component_type: "audio_fx"`.
Fully standard `chain_params`-driven module, no raw MIDI dependency.
