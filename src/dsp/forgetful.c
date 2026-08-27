/*
 * Forgetful — one live input, four tape memories, all of them forgetting.
 *
 * Build step 3 (docs/plans/forgetful-design.md, Build/Test Plan #3):
 * generalizes the single-LoopEngine build (step 2) to all four loops plus
 * the shared input router (Master page). Each loop's own record/degrade/
 * playback behavior is unchanged from step 2 — this step adds:
 *
 *   - a shared `input_routing` selector (A/B/C/D — no OFF/unrouted state,
 *     removed 2026-08-25 once recording became manual; see the ROUTE_A
 *     comment): only the currently routed loop can ever be RECORDING; the
 *     other three loops progress on their own (LOOPING keeps decaying,
 *     FORGOTTEN keeps flushing) regardless of routing.
 *   - routing-change-closes-recording: moving `input_routing` away from a
 *     loop that is currently RECORDING closes it immediately (in
 *     set_param, synchronously), via the same close_recording() path as
 *     master_record's second press and the buffer-full close.
 *
 * Recording is a MANUAL gesture (Master page's `master_record` trigger),
 * not level-detection — a level-based auto-trigger shipped first and was
 * replaced on-device feedback 2026-08-25: real playing levels didn't
 * reliably cross any single fixed threshold, and the trailing
 * silence-timeout baked an audible pause into the start of every loop.
 * First `master_record` press while routed and IDLE/FORGOTTEN starts
 * RECORDING; second press closes it (see close_recording, set_param).
 * Buffer-full (60s) remains as an automatic safety cap.
 *   - dry-once, wet-summed-across-four mixing: dry is read once and added
 *     once per sample (see mix_dry_wet), with all four loops' wet
 *     contributions — each scaled by that loop's own Master-page Volume
 *     knob — summed on top of it first.
 *
 * Mixing architecture (unchanged principle from step 2, docs/plans/
 * forgetful-design.md "Signal flow per block"): dry input passes through
 * UNCONDITIONALLY in every state, for every loop. This is an inline
 * audio_fx sitting between the synth and the output, so silence as *the*
 * output in any non-LOOPING state would mute the whole track.
 *
 * Timing model: unchanged — one shared, monotonically increasing
 * total_frames counter drives the forgotten-display window for all four
 * loops. No syscalls in the hot path.
 *
 * Page layout (5 pages x 8 knobs): declared as five named ui_hierarchy
 * levels — root (Master) plus loopA..loopD, each reached via a nav entry in
 * root's "params" — rather than one flat 40-key array. A flat array's
 * overflow auto-splits into "Main-2".."Main-5" continuation pages with no
 * separators on the bank bar; on-device feedback 2026-08-24. Separate levels
 * give each loop page its own name (just "A".."D", 2026-08-25 — was "Loop
 * A".."Loop D", trimmed in the same poetic-naming pass as ECHO/HOLD) and a
 * real section break, same mechanism moog/minijv use for "Filter"/
 * "Oscillator 1". The
 * root page itself is always titled "Main" by the page planner regardless of
 * any label declared here (fleet-wide convention). Unused knob slots (was
 * "reserved" dummy params, Master 7-8 and each loop's 7) are now bare ""
 * entries in knobs[] — true empty grid space, not a param.
 *
 *   Page 0 (Master, level "root"):  input_routing, master_loops_overview,
 *                      master_record, master_freeze, loopA_volume..loopD_volume
 *   Page 1-4 (levels "loopA".."loopD"): loopX_decay_rate (Age),
 *                      loopX_saturation (Drive), loopX_state (State),
 *                      loopX_erase, loopX_wow (Warp), loopX_hf_loss (Darken),
 *                      loopX_chaos (VINYL), loopX_hiss — order set 2026-08-25
 *                      on explicit on-device request (Age, Drive, State,
 *                      Erase, Warp, Darken, VINYL, Hiss; supersedes an
 *                      earlier same-day reorder). Wire keys are unaffected —
 *                      this is purely knobs[]/chain_params declaration order.
 *
 * settings-schema.json isn't wired yet; constants below hardcode the
 * design doc's stated defaults, same as step 2.
 *
 * Build step 4 (docs/plans/forgetful-design.md, Build/Test Plan #4): the
 * status-text half. `loopX_status` (previously a bare state-name stub) now
 * builds the full state line the design doc specifies — "Looping - NN%
 * (word)" while looping, via memory_word()'s bucket mapping, "Listening..."
 * / "Recording" / "Forgotten" otherwise. ASCII punctuation throughout
 * (hyphen, three periods), not an em-dash or the Unicode ellipsis, matching
 * the Erase trigger's idle-spelling call ("-", not "—").
 *
 * The Master page's "status overview" turned out not to be expressible as
 * text paired with each volume knob's cell: a chain_params entry only ever
 * annotates its OWN knob, never a neighbor's, and the knob-grid's value
 * cell (~30px at the 4x5 font — CELL_W/LABEL_CHARS in render_page_movy.mjs)
 * has room for roughly 6-8 characters, nowhere near a 23-character
 * "A:74% B:Rec C:-- D:12%" string. `master_loops_overview` ("Status" on the
 * Master page, access "read") is the fit-the-budget replacement: one glyph
 * per loop in A/B/C/D order, no separators — '-' idle/forgotten, 'R'
 * recording, else a memory decile digit (e.g. "7R-1"). Declared as an enum,
 * not a string — see the chain_params comment at its declaration below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

#define NUM_LOOPS 4

/* No OFF/unrouted state — removed 2026-08-25. Recording is a manual gesture
 * (master_record), not level-triggered, so there is no longer a safety
 * reason to be able to point the input at nothing: not pressing record
 * already means nothing gets recorded, regardless of where Route sits.
 * Route is always exactly one of the four loops, matching LOOP_LETTERS'
 * indexing directly — ROUTE_A..D equal the loop's own array index, so
 * `s->loops[s->input_routing]` is always the routed loop with no offset. */
#define ROUTE_A    0
#define ROUTE_B    1
#define ROUTE_C    2
#define ROUTE_D    3

static const char LOOP_LETTERS[NUM_LOOPS] = { 'A', 'B', 'C', 'D' };
static const char *LOOP_PREFIXES[NUM_LOOPS] = { "loopA_", "loopB_", "loopC_", "loopD_" };
static const char *ROUTE_LABELS[NUM_LOOPS] = { "A", "B", "C", "D" };

#define SAMPLE_RATE      44100
#define BUFFER_SECONDS   60                             /* settings-schema default */
#define BUFFER_CAPACITY  (SAMPLE_RATE * BUFFER_SECONDS)

#define MIN_RECORDED_MS       50    /* floor: discard a too-short take rather than loop a click */
#define FORGOTTEN_DISPLAY_MS  400   /* how long the UI may still report
                                     * "Forgotten" after the engine has
                                     * already reset to IDLE */
#define OVERDUB_MAX_FILL      64 /* cap on the gap-fill span, so a read-head
                                  * jump can never smear one input sample
                                  * across the whole take. */
#define OVERDUB_FADE_SECONDS  0.008f /* 8ms in/out ramp on the overdub write
                                      * gain. Long enough to kill the step
                                      * edge, short enough that the front of
                                      * a note you punch in on survives. */
#define ERASE_FADE_SECONDS    10.0f /* erase on a LOOPING loop fades out over
                                      * this long, then clears — not an
                                      * instant cut. Idle/Recording/Forgotten
                                      * still clear immediately, since there
                                      * is either nothing playing to fade or
                                      * (Recording) an unfinished take being
                                      * discarded rather than a loop being
                                      * "let go of". */

#define MIN_RECORDED_FRAMES      ((int)(MIN_RECORDED_MS * SAMPLE_RATE / 1000))
#define FORGOTTEN_DISPLAY_FRAMES ((uint64_t)(FORGOTTEN_DISPLAY_MS * SAMPLE_RATE / 1000))

#define TIME_NOT_SET UINT64_MAX

#define PI_F 3.14159265358979323846f

/* ---- Flavour arrival --------------------------------------------------
 * Every flavour is worth hearing the moment you turn it AND worth more as
 * the take ages. Scaling straight by `age` (2026-08-27) got the second
 * half right and destroyed the first: on a fresh loop the multiplier is
 * zero, so Warp and Darken did precisely nothing at the moment you were
 * listening for them, which is what "doing nothing I can hear" was.
 * FLAVOUR_IMMEDIATE is how much of the knob you get straight away; the
 * rest arrives by the time the take is FLAVOUR_AGE_FULL gone. */
#define FLAVOUR_IMMEDIATE  0.45f
#define FLAVOUR_AGE_FULL   0.50f

#define WOW_RATE_HZ        0.7f
#define FLUTTER_RATE_HZ    7.0f

/* Flavor timing model v2, replaced 2026-08-25 (superseding the v1 "constant
 * shared rate" scheme from earlier the same day): Warp, Darken, Hiss and
 * VINYL are now TURN-BASED, not automatic. Each has a `flavor_ramp_t`
 * (target + step + touched) instead of a plain float:
 *
 *   - The FIRST set_param write to one of these keys since the take started
 *     (flavor_ramp_t.touched == 0) jumps `applied_X` straight to the new
 *     value with no ramp at all — "the first turn sets the current value".
 *   - Every write after that starts a fresh LINEAR ramp from wherever
 *     `applied_X` currently sits to the new target (flavor_ramp_set_param
 *     computes a fresh `.step`), timed to land exactly when THIS loop's
 *     memory would reach 0 — memory*decay_rate seconds from the moment of
 *     the turn, not decay_rate itself, so a turn made 90% of the way
 *     through a fade takes proportionally less time to arrive than one made
 *     right after recording. Nothing moves on its own between turns.
 *   - Every take starts completely untouched (reset_take/close_recording
 *     zero every flavor_ramp_t, same as applied_X) — a knob you dialed in on
 *     a previous take has no effect on the next one until you turn it again.
 *
 * Drive (saturation) is the ONE exception, deliberately: it keeps the v1
 * behavior — `applied_saturation` auto-chases toward the live `saturation`
 * knob value at the constant decay_rate-derived rate computed inline in the
 * LOOPING case, same as the day-one design. Only Warp/Darken/Hiss/VINYL got
 * the turn-based rework.
 *
 * HISS_CHASE_RATE_SCALE (the v1 per-flavor rate override that made hiss
 * chase 0.75x as fast as everyone else) is GONE — it was a workaround for
 * v1's single shared automatic rate. Under v2 the user directly controls
 * hiss's pacing by how they turn the knob, so a hardcoded slowdown no longer
 * has a place.
 *
 * Frozen is a THIRD mode layered on top (2026-08-25, "turning the flavour
 * knobs should change live the effect [while frozen]" — corrected same day:
 * "shouldn't snap instantly... let me control the value like I would
 * normally, ramp up or down the scale of the knob, no snapping"). Every
 * write to any of the five flavor knobs — including Drive — while
 * `loop->frozen` is set starts a SHORT, FIXED-DURATION glide (see
 * FROZEN_GLIDE_SECONDS) from wherever `applied_X` currently sits to the new
 * value, instead of either the v2 remaining-time ramp (meaningless here —
 * memory isn't draining) or an instant jump (reads as a snap/click, not a
 * knob being "played"). Retriggered on every write, same as the v2 ramp, so
 * turning the physical knob at a normal pace — a steady stream of small
 * writes — feels like continuous live tracking, not a stepped chase. It
 * still marks the ramp `touched`, so unfreezing "resumes normal mode...
 * with the new initial value... at the value set while frozen" — the next
 * turn after unfreezing ramps from wherever the frozen glide left it, not
 * from 0. See flavor_ramp_set_param's `frozen` argument and the
 * `saturation`/`saturation_glide_step` handling for Drive's parallel path
 * (it has no flavor_ramp_t, so it needs its own one-off step holder). */
#define FROZEN_GLIDE_SECONDS 0.15f

/* ---- Warp: tape that cannot hold its speed ---------------------------
 * Three things at once, because two sines read as a chorus pedal rather
 * than as tape: a slow WOW, a fast FLUTTER, and a DRIFT that wanders at
 * random. The drift is what stops it sounding periodic — a real transport
 * sags unpredictably, and a listener locks onto anything that repeats.
 *
 * Flutter is scaled by the square of the knob so it only shows up once
 * you are past halfway: gentle settings are a slow sag, extreme settings
 * are a machine coming apart. Full depth is ~7.5% ≈ 1.25 semitones. */
#define WOW_MOD_DEPTH      0.030f
#define FLUTTER_MOD_DEPTH  0.020f
#define DRIFT_MOD_DEPTH    0.025f
#define DRIFT_HOLD_SECONDS 0.9f   /* how often the drift picks a new target */
#define DRIFT_GLIDE_SECONDS 0.35f /* how long it takes to get there */

#define HISS_CEILING          0.040f /* Raised back up 2026-08-27, when hiss
                                      * stopped being constant and started
                                      * scaling with `age`: this is now the
                                      * level at FULL decay with the knob
                                      * maxed, not the level all the way
                                      * through. Was 0.045 -> 0.02925 ->
                                      * 0.014625, two halvings chasing a
                                      * problem that was placement, not gain. */
/* One-pole coefficient for the hiss-coloring highpass (see the hiss stage
 * comment) — ~1kHz-ish corner, low enough to strip the noise's rumble,
 * high enough to keep it sounding like noise rather than a whistle. */
#define HISS_COLOR_COEFF       0.15f
#define SATURATION_MAX_DRIVE  9.0f

/* VINYL — replaces the old "Glitch" chaos-gate (a dropout mute) entirely:
 * a vinyl-sim crackle (SP-404-style) rather than a tape glitch. Modeled as
 * a Poisson click/pop process — the standard technique for this texture —
 * mixed in ADDITIVELY alongside Hiss (see the LOOPING case), not as a mute.
 * Two densities layered: frequent, quiet "dust" clicks (the constant
 * surface texture) and rare, louder "pop" transients (occasional surface
 * damage) — a single density reads as either constant static or isolated
 * ticks at every setting, never a believable vinyl surface. Each trigger
 * adds an exponentially-decaying envelope value on top of whatever is
 * still ringing from a prior trigger (CRACKLE_ENV_DECAY), so a single
 * trigger reads as an actual "tick" instead of a single-sample digital
 * click, and overlapping ticks are possible and sound natural rather than
 * retriggering cleanly like a synth voice would.
 *
 * Two-stage knob mapping, requested 2026-08-25 ("too intense" at the old
 * single-curve mapping, and the gain/density constants are toned down
 * across the board on top of it): applied_crackle (0..1, via the v2 ramp
 * above) drives VOLUME linearly across its WHOLE range, but density
 * (trigger probability) sits at a fixed low baseline for the bottom half
 * and only climbs during the top half, reaching its ceiling — itself only
 * half of what the old single-stage mapping used at knob=1 — exactly at
 * applied_crackle=1. So the first half is "how loud are the (sparse,
 * constant-density) clicks", the second half is "loud, AND getting
 * busier". See the LOOPING case's VINYL stage for the actual volume/
 * density split. First-pass constants, not confirmed by listening. */
/* ---- VINYL dropouts: the medium losing material ----------------------
 * Crackle adds noise ON TOP of the take. Dropouts take the take AWAY, and
 * that is the half that actually sounds like the Disintegration Loops: a
 * tape shedding oxide does not get noisier, it gets HOLES, and what you
 * end up listening to is the gaps between what is left.
 *
 * These are written INTO the buffer, not applied at the output, so they
 * accumulate: every pass can punch a new hole and deepen an old one, and
 * none of it comes back. That is the point, and it is the only
 * irreversible thing on the module besides Erase — Freeze stops them,
 * exactly as it stops decay, so a frozen take stays as ruined as it was.
 *
 * The cursor advances one sample per tick INDEPENDENTLY of the read head.
 * Attenuating "wherever idx0 is" would leave gaps whenever Warp pushes
 * speed above 1 and the read head skips an index — and a single
 * un-attenuated sample in the middle of a hole is an impulse, i.e. a
 * click, which is exactly the bug already fixed once in the overdub
 * write. A private cursor cannot skip. */
#define DROPOUT_MAX_RATE_HZ   3.0f   /* events/sec at full VINYL, fully aged */
#define DROPOUT_MIN_MS        8.0f
#define DROPOUT_MAX_MS       70.0f
#define DROPOUT_DEPTH         0.12f  /* gain at the deepest point of a hole */

#define CRACKLE_DUST_MAX_PROB 0.02f    /* per-sample trigger probability at density_factor=1 */
#define CRACKLE_POP_MAX_PROB  0.0006f
#define CRACKLE_ENV_DECAY     0.75f    /* per-sample envelope decay -> ~15-sample tick tail */
#define CRACKLE_DUST_GAIN     0.015f   /* was 0.05 -> 0.03 -> this (2026-08-25: "way too loud" — halved again) */
#define CRACKLE_POP_GAIN      0.075f   /* was 0.35 -> 0.15 -> this (2026-08-25: "way too loud" — halved again) */
#define CRACKLE_BASELINE_DENSITY_FRAC 0.35f /* density_factor held here across applied_crackle in [0, 0.5] */
#define CRACKLE_MAX_DENSITY_FRAC      0.5f  /* density_factor reached at applied_crackle=1 */

/* Darken's reverb wash (replaced the plain LP filter 2026-08-25 — "still not
 * clear what Darken is doing... a wash of reverb that gets darker over
 * time"). Scaled-down Schroeder-Moorer reverb, same algorithm and comb/
 * allpass tuning family as src/modules/audio_fx/freeverb/freeverb.c (a
 * proven, already-tuned design in this codebase) but 4 combs + 2 allpasses
 * per channel instead of freeverb's 8+4 — one full freeverb-sized reverb
 * PER LOOP, times up to four loops simultaneously, would be a real chunk of
 * the ~900us/frame realtime budget; this is roughly a quarter of that cost
 * for a texture that only needs to read as "a wash", not a concert hall.
 * WET AMOUNT, internal DAMPING (how dark the tail sounds) and now FEEDBACK
 * (how long the tail rings out — added 2026-08-25: "it needs to increase in
 * decay as I increase the knob, into a wall of reverb") are ALL driven by
 * the same applied_hf_loss chase — the wash gets more present, darker, AND
 * longer together as the knob rises, so a maxed-out Darken piles up into a
 * dense, near-infinite wash rather than a short dark room. Feedback maps
 * through the exact same formula freeverb.c uses for its own room_size
 * (feedback = room_size*0.28+0.7), treating applied_hf_loss as that
 * room_size: REVERB_FEEDBACK_MIN (0.70, freeverb's room_size=0 floor) up to
 * REVERB_FEEDBACK_MAX (0.98, freeverb's room_size=1 ceiling — audibly a
 * "wall", not literally infinite, since a true 1.0 feedback comb never
 * decays at all). */
#define REVERB_NUM_COMBS     4
#define REVERB_NUM_ALLPASS   2
/* ---- Darken: the top end going --------------------------------------
 * The name says darkening, so the primary effect is now a real lowpass —
 * four cascaded one-poles, 24 dB/oct, swept EXPONENTIALLY from bypass
 * down to DARKEN_FC_MIN. Exponential because that is what makes the knob
 * scale evenly: half way is a musical octave-ish midpoint, not already in
 * the mud. A single 6 dB/oct pole was the original implementation here
 * and was reported as inaudible; replacing it with reverb alone answered
 * "wall" but not "darken", and left the bottom half of the travel doing
 * nothing you could name.
 *
 * The reverb wash is still there, but as the TOP of the range only: past
 * DARKEN_WASH_KNEE it blends in over the filtered signal, so maximum is
 * still the dark wall asked for on 2026-08-27 while everything below it
 * is audibly, gradually losing brightness. The wash is fed the FILTERED
 * signal, so the tail is as dark as the loop is. */
#define DARKEN_POLES         4
#define DARKEN_FC_MAX    18000.0f
#define DARKEN_FC_MIN      200.0f
#define DARKEN_WASH_KNEE     0.55f
#define DARKEN_DAMPING       0.55f /* comb damping at full wet — how dark the
                                    * wash goes. Was 0.4; raised 2026-08-27
                                    * with the age curve, since "Darken" at
                                    * maximum should be genuinely hard to
                                    * pick the source out of. */
#define DARKEN_AGE_FULL      0.50f /* Darken reaches its knob setting by the
                                    * time the take is this far gone, not at
                                    * memory==0. Scaling it by raw `age`
                                    * meant full wet arrived exactly as the
                                    * loop vanished, so the knob's top half
                                    * was never actually audible — reported
                                    * 2026-08-27 as Darken not being
                                    * pronounced enough at maximum. Reaching
                                    * the destination half way also leaves a
                                    * long plateau of full wash, which is
                                    * the part that sounds like the record. */
#define REVERB_FEEDBACK_MIN  0.70f
#define REVERB_FEEDBACK_MAX  0.98f

#define DEFAULT_LOOP_VOLUME  0.8f

typedef enum {
    LOOP_IDLE = 0,
    LOOP_RECORDING,
    LOOP_LOOPING,
    LOOP_FORGOTTEN
} loop_state_t;

typedef struct {
    int16_t l, r;
} frame16_t;

/* Comb/allpass delay lengths (samples at 44100Hz) — the first 4 comb and
 * first 2 allpass lengths from freeverb.c's own tuning tables, R channel
 * offset +23 samples for stereo decorrelation, same trick freeverb.c uses. */
static const int reverb_comb_tuning_l[REVERB_NUM_COMBS] = { 1116, 1188, 1277, 1356 };
static const int reverb_comb_tuning_r[REVERB_NUM_COMBS] = {
    1116 + 23, 1188 + 23, 1277 + 23, 1356 + 23
};
static const int reverb_allpass_tuning_l[REVERB_NUM_ALLPASS] = { 556, 441 };
static const int reverb_allpass_tuning_r[REVERB_NUM_ALLPASS] = { 556 + 23, 441 + 23 };

/* Heap-allocated (not a fixed MAX_DELAY array like freeverb.c's — this runs
 * up to 4x concurrently, one per loop, so exact-sized buffers matter). */
typedef struct {
    float *buf;
    int size;
    int idx;
    float filterstore;
} reverb_comb_t;

typedef struct {
    float *buf;
    int size;
    int idx;
} reverb_allpass_t;

static int reverb_comb_alloc(reverb_comb_t *c, int size) {
    c->buf = (float *)calloc((size_t)size, sizeof(float));
    if (!c->buf) return -1;
    c->size = size;
    c->idx = 0;
    c->filterstore = 0.0f;
    return 0;
}

static int reverb_allpass_alloc(reverb_allpass_t *a, int size) {
    a->buf = (float *)calloc((size_t)size, sizeof(float));
    if (!a->buf) return -1;
    a->size = size;
    a->idx = 0;
    return 0;
}

/* damp1/damp2 are passed in fresh each call (not cached on the comb) since
 * Darken's damping is time-varying here, unlike freeverb.c's per-block
 * constant. */
static inline float reverb_comb_process(reverb_comb_t *c, float input, float feedback, float damp1, float damp2) {
    float output = c->buf[c->idx];
    c->filterstore = (output * damp2) + (c->filterstore * damp1);
    c->buf[c->idx] = input + (c->filterstore * feedback);
    if (++c->idx >= c->size) c->idx = 0;
    return output;
}

static inline float reverb_allpass_process(reverb_allpass_t *a, float input) {
    float bufout = a->buf[a->idx];
    float output = -input + bufout;
    a->buf[a->idx] = input + (bufout * 0.5f);
    if (++a->idx >= a->size) a->idx = 0;
    return output;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Turn-based ramp state for one of the four v2 flavor knobs (Warp/Darken/
 * Hiss/VINYL) — see the flavor timing model v2 comment at HISS_CHASE_RATE_
 * SCALE's old location. `target` is what get_param reports (the knob's own
 * position, independent of how far the ramp has actually gotten); `step` is
 * a snapshot taken at the moment of the most recent set_param, not
 * recomputed afterward; `touched` gates the "first turn jumps instantly"
 * rule and resets to 0 every take. */
typedef struct {
    float target;
    float step;
    int   touched;
} flavor_ramp_t;

/* Handles one set_param write to a v2 flavor knob (suffix wow/hf_loss/hiss/
 * chaos). `*applied` is the loop's own applied_X field for this flavor.
 * `memory`/`decay_rate` come from the owning loop_engine_t at the moment of
 * the call — remaining_seconds = memory*decay_rate is "how much time
 * actually remains for the loop to fade to silence right now", which is
 * why this takes memory rather than just decay_rate: a turn made partway
 * through a fade gets less time to arrive than one made right after
 * recording, even if decay_rate itself hasn't changed. A turn while the
 * loop isn't LOOPING (memory stale/0 from a previous take, or never set)
 * degrades gracefully — remaining_samples floors at 1, so the ramp is
 * effectively instant, which is a reasonable answer to "how long should
 * this take when there's no fade in progress at all".
 *
 * `frozen` (2026-08-25, corrected same day — see FROZEN_GLIDE_SECONDS):
 * while true, every write starts a short FIXED-duration glide toward the
 * new value instead of the remaining-time ramp — checked BEFORE the
 * touched/untouched split below, so a frozen turn on a never-touched knob
 * also glides rather than jumping. Still marks `touched`, so once unfrozen
 * the value just glided to is the new baseline the NEXT (post-unfreeze)
 * turn ramps from — "resume normal mode... with the new initial value...
 * at the value set while frozen". */
static void flavor_ramp_set_param(flavor_ramp_t *ramp, float *applied, float new_value, float memory, float decay_rate, int frozen) {
    new_value = clampf(new_value, 0.0f, 1.0f);
    ramp->target = new_value;
    if (frozen) {
        ramp->touched = 1;
        float glide_samples = FROZEN_GLIDE_SECONDS * (float)SAMPLE_RATE;
        ramp->step = fabsf(new_value - *applied) / glide_samples;
    } else if (!ramp->touched) {
        ramp->touched = 1;
        *applied = new_value;
        ramp->step = 0.0f;
    } else {
        float remaining_samples = memory * decay_rate * (float)SAMPLE_RATE;
        if (remaining_samples < 1.0f) remaining_samples = 1.0f;
        ramp->step = fabsf(new_value - *applied) / remaining_samples;
    }
}

/* Everything genuinely per-loop. Timing decisions inside a loop_engine_t
 * compare against the OWNING inst_t's shared total_frames (passed in), not a
 * private copy — all four loops process the same blocks in lockstep, so one
 * shared clock is correct. */
typedef struct {
    /* state machine */
    loop_state_t state;

    /* live knob params (chain_params: loopX_*). decay_rate's UNIT changed
     * from "repeats until forgotten" to "seconds until forgotten" (see the
     * LOOPING case in v2_process_block) — the key name is unchanged to
     * avoid a wire-format churn, but the value is now a duration, not a
     * count. `saturation` (Drive) is the one flavor knob still a plain
     * float — it keeps the v1 auto-chase behavior (see the flavor timing
     * model v2 comment). Warp/Darken/Hiss/VINYL moved to flavor_ramp_t
     * below; their wire keys (loopX_wow/hf_loss/hiss/chaos) are unchanged —
     * "chaos" carries the same wire-compatibility note as decay_rate: the
     * C name and algorithm behind it are now VINYL crackle, not the old
     * chaos-gate dropout (see CRACKLE_DUST_MAX_PROB). */
    float decay_rate;
    float saturation;
    /* Drive's own one-off glide step, used ONLY while frozen (see
     * FROZEN_GLIDE_SECONDS and loop_set_param's "saturation" branch) — set
     * fresh on every frozen write. Drive has no flavor_ramp_t to hold this
     * (it stays the plain v1 auto-chase knob when unfrozen), so it gets its
     * own single field rather than a whole struct for one number. */
    float saturation_glide_step;

    flavor_ramp_t wow_ramp;
    flavor_ramp_t hf_loss_ramp;
    flavor_ramp_t hiss_ramp;
    flavor_ramp_t crackle_ramp;

    /* Currently-audible value for each of the five flavor knobs — what
     * every DSP stage actually reads, not the raw knob/target value. Drive
     * chases this toward `saturation` at a constant rate every sample (see
     * the LOOPING case); the other four are advanced by chase() using their
     * own flavor_ramp_t's `.step`, which only changes on a set_param write. */
    float applied_wow, applied_hf_loss, applied_hiss, applied_saturation, applied_crackle;

    /* buffer & playback */
    frame16_t *buffer;
    int        capacity_frames;
    int        write_head;
    int        recorded_length;
    double     read_head;
    float      wow_phase;
    float      flutter_phase;

    /* degradation */
    float memory;
    int   dropout_left, dropout_len, dropout_cursor;
    float darken_lp_l[DARKEN_POLES], darken_lp_r[DARKEN_POLES];
    /* per-block, hoisted out of the sample loop: powf/expf once per block
     * per loop rather than 128 times. The chase behind them moves over
     * seconds, so block granularity is inaudible. */
    float darken_a, darken_wash, darken_damp1, darken_fb, warp_amt, dropout_p;
    float warp_drift, warp_drift_target;
    int   warp_drift_countdown;
    float hiss_lp_l, hiss_lp_r;  /* hiss-coloring filter state, see HISS_COLOR_COEFF */
    float crackle_env;  /* VINYL click/pop envelope, see CRACKLE_DUST_MAX_PROB */
    int   frozen;  /* master_freeze: memory stops draining, loop keeps
                    * playing at whatever character it already reached */
    int   overdubbing;  /* master_record's second toggle while LOOPING (not
                          * RECORDING) — see the master_record set_param
                          * handler and the LOOPING case's overdub-write
                          * block. Does NOT touch memory, the flavor knobs,
                          * or reverb/hiss state — only reset_take() (a real
                          * erase, not overdub) resets those. */
    float overdub_gain;  /* what the overdub write is actually scaled by,
                          * chasing `overdubbing` over OVERDUB_FADE_SECONDS
                          * instead of stepping to it. See the overdub-write
                          * block: the toggle is a button press landing on an
                          * arbitrary sample, and writing full-amplitude
                          * input from that sample on puts a step edge INTO
                          * the take, which then clicks once per pass
                          * forever. */
    int   overdub_last_idx; /* last buffer index the overdub wrote, or -1.
                          * With Warp up, `speed` != 1 so consecutive output
                          * samples can land on the same idx0 (writing the
                          * same frame twice — doubling it) or skip past one
                          * (leaving a gap the write never fills). Both are
                          * broadband edges. See the overdub-write block. */

    /* Darken's reverb wash — see the REVERB_NUM_COMBS comment. Buffers are
     * heap-allocated per instance (v2_create_instance) sized exactly to
     * reverb_comb_tuning_l/r, not a fixed MAX_DELAY. */
    reverb_comb_t    comb_l[REVERB_NUM_COMBS], comb_r[REVERB_NUM_COMBS];
    reverb_allpass_t allpass_l[REVERB_NUM_ALLPASS], allpass_r[REVERB_NUM_ALLPASS];

    /* erase fade-out: an ADDITIONAL output gain, independent of memory,
     * ramping 1.0 -> 0.0 over ERASE_FADE_SECONDS once erase fires on a
     * LOOPING loop; the buffer clears when it reaches 0 (see the erase
     * handler and the LOOPING case's fade block). Memory decay is
     * suspended while erasing — same reasoning as `frozen` — so the
     * loop's own aging can't race the fade to silence. */
    int   erasing;
    float erase_fade_gain;

    /* display-only: set the instant memory hits 0; lets the UI see
     * "Forgotten" for a short window even though `state` has already
     * reset to IDLE (see loop_display_state_name). */
    uint64_t forgotten_at;

    /* noise generator — independent per loop so hiss/crackle textures never
     * perturb each other across loops */
    uint32_t rng_state;
} loop_engine_t;

/* free(NULL) is a no-op, so this is safe on a loop whose allocation failed
 * partway through reverb_alloc_loop (calloc'd inst_t means every unset
 * .buf starts NULL) as well as on a fully-allocated one. */
static void reverb_free_loop(loop_engine_t *loop) {
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        free(loop->comb_l[i].buf);
        free(loop->comb_r[i].buf);
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        free(loop->allpass_l[i].buf);
        free(loop->allpass_r[i].buf);
    }
}

/* Allocates every comb/allpass buffer for one loop. On failure partway
 * through, frees whatever this call already allocated before returning -1
 * — the caller still owns freeing any OTHER loops it already succeeded on. */
static int reverb_alloc_loop(loop_engine_t *loop) {
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        if (reverb_comb_alloc(&loop->comb_l[i], reverb_comb_tuning_l[i]) != 0 ||
            reverb_comb_alloc(&loop->comb_r[i], reverb_comb_tuning_r[i]) != 0) {
            reverb_free_loop(loop);
            return -1;
        }
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        if (reverb_allpass_alloc(&loop->allpass_l[i], reverb_allpass_tuning_l[i]) != 0 ||
            reverb_allpass_alloc(&loop->allpass_r[i], reverb_allpass_tuning_r[i]) != 0) {
            reverb_free_loop(loop);
            return -1;
        }
    }
    return 0;
}

typedef struct {
    loop_engine_t loops[NUM_LOOPS];

    /* shared time base, compared against by every loop's forgotten-display
     * window */
    uint64_t total_frames;

    /* Master page */
    int   input_routing;            /* ROUTE_A..ROUTE_D — always a valid loop index */
    float loop_volume[NUM_LOOPS];
} inst_t;

/* ---- small helpers ---- */

/* Moves `current` toward `target` by at most `step` (either direction) — the
 * per-sample update for every applied_* flavor amount. Turning a knob live
 * only moves `target`; `current` still glides there at the same fixed rate,
 * so the sound never jumps. */
static float chase(float current, float target, float step) {
    if (current < target) {
        current += step;
        if (current > target) current = target;
    } else if (current > target) {
        current -= step;
        if (current < target) current = target;
    }
    return current;
}

/* Adds a wet sample (float, roughly -1..1) onto a RAW int16 dry sample and
 * clamps in integer space. Deliberately not dry_float+wet_float->int16: an
 * int16->float->int16 round-trip on the dry term (/32768 then *32767, the
 * same asymmetric convention freeverb.c uses) is off by up to 1 LSB, which
 * would quietly break "dry passes through unconditionally" for any loop
 * that contributes wet=0. Bit-exact there is the whole point, not an
 * approximation of it. */
static int16_t mix_dry_wet(int16_t dry, float wet) {
    int32_t out = (int32_t)dry + lroundf(wet * 32767.0f);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

/* xorshift32 — fast, deterministic-enough noise/dice source; not for crypto */
static uint32_t rng_next(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

/* uniform float in [-1, 1] */
static float rng_bipolar(uint32_t *seed) {
    return ((float)(rng_next(seed) & 0xFFFFFF) / (float)0xFFFFFF) * 2.0f - 1.0f;
}

/* uniform float in [lo, hi] */
static float rng_range(uint32_t *seed, float lo, float hi) {
    float u = (float)(rng_next(seed) & 0xFFFFFF) / (float)0xFFFFFF;
    return lo + u * (hi - lo);
}

/* Zeroes a loop's reverb tail — every comb/allpass buffer, filterstore and
 * write index — so a fresh take never inherits the previous take's wash. */
static void reverb_clear_loop(loop_engine_t *loop) {
    for (int i = 0; i < REVERB_NUM_COMBS; i++) {
        memset(loop->comb_l[i].buf, 0, sizeof(float) * (size_t)loop->comb_l[i].size);
        memset(loop->comb_r[i].buf, 0, sizeof(float) * (size_t)loop->comb_r[i].size);
        loop->comb_l[i].idx = loop->comb_r[i].idx = 0;
        loop->comb_l[i].filterstore = loop->comb_r[i].filterstore = 0.0f;
    }
    for (int i = 0; i < REVERB_NUM_ALLPASS; i++) {
        memset(loop->allpass_l[i].buf, 0, sizeof(float) * (size_t)loop->allpass_l[i].size);
        memset(loop->allpass_r[i].buf, 0, sizeof(float) * (size_t)loop->allpass_r[i].size);
        loop->allpass_l[i].idx = loop->allpass_r[i].idx = 0;
    }
}

/* Clears everything about the current take. Shared by: IDLE->RECORDING entry,
 * discarding a too-short take, FORGOTTEN->IDLE, and a confirmed erase (both
 * the instant-clear and the fade-out's completion — see loop_set_param's
 * "erase" branch and the LOOPING case's erase-fade block). This is also
 * where Warp/Darken/Hiss/VINYL's flavor_ramp_t reset to "minimum, untouched"
 * (2026-08-25: "erasing a loop... resets the values of all the flavours to
 * initial") — every genuine erase completion routes through here, so no
 * separate reset call is needed at the erase call sites themselves. This is
 * deliberately NOT called for an overdub (a second master_record press
 * while LOOPING, see set_param) — overdubbing adds to the existing take
 * without touching flavor state at all, only a real erase resets it. */
static void reset_take(loop_engine_t *loop) {
    loop->write_head = 0;
    loop->recorded_length = 0;
    loop->read_head = 0.0;
    loop->erasing = 0;
    loop->erase_fade_gain = 1.0f;
    loop->overdubbing = 0;
    loop->overdub_gain = 0.0f;
    loop->overdub_last_idx = -1;
    loop->applied_wow = loop->applied_hf_loss = loop->applied_hiss = 0.0f;
    for (int k = 0; k < DARKEN_POLES; k++) loop->darken_lp_l[k] = loop->darken_lp_r[k] = 0.0f;
    loop->dropout_left = loop->dropout_len = 0;
    loop->dropout_cursor = 0;
    loop->warp_drift = loop->warp_drift_target = 0.0f;
    loop->warp_drift_countdown = 0;
    loop->applied_saturation = loop->applied_crackle = 0.0f;
    loop->wow_ramp = loop->hf_loss_ramp = loop->hiss_ramp = loop->crackle_ramp = (flavor_ramp_t){0};
    loop->saturation_glide_step = 0.0f;
    reverb_clear_loop(loop);
}

/* Shared by all three close triggers: a manual master_record press, buffer-
 * full, and a routing change away from this loop (see set_param's
 * "input_routing" and "master_record"). */
/* One overdub frame, added into the take and clipped in int32 space —
 * same convention as mix_dry_wet. Split out because the overdub write has
 * to be able to fill a span of frames, not just the one under the read
 * head; see the overdub-write block. */
static inline void overdub_add(loop_engine_t *loop, int idx, int16_t l, int16_t r) {
    int32_t nl = (int32_t)loop->buffer[idx].l + (int32_t)l;
    int32_t nr = (int32_t)loop->buffer[idx].r + (int32_t)r;
    if (nl > 32767) nl = 32767; else if (nl < -32768) nl = -32768;
    if (nr > 32767) nr = 32767; else if (nr < -32768) nr = -32768;
    loop->buffer[idx].l = (int16_t)nl;
    loop->buffer[idx].r = (int16_t)nr;
}

static void close_recording(loop_engine_t *loop) {
    if (loop->write_head < MIN_RECORDED_FRAMES) {
        /* too short to be a usable take — discard, don't loop a click */
        loop->state = LOOP_IDLE;
        reset_take(loop);
        return;
    }
    loop->recorded_length = loop->write_head;
    loop->read_head = 0.0;
    loop->memory = 1.0f;
    loop->applied_wow = loop->applied_hf_loss = loop->applied_hiss = 0.0f;
    for (int k = 0; k < DARKEN_POLES; k++) loop->darken_lp_l[k] = loop->darken_lp_r[k] = 0.0f;
    loop->dropout_left = loop->dropout_len = 0;
    loop->dropout_cursor = 0;
    loop->warp_drift = loop->warp_drift_target = 0.0f;
    loop->warp_drift_countdown = 0;
    loop->applied_saturation = loop->applied_crackle = 0.0f;
    loop->wow_ramp = loop->hf_loss_ramp = loop->hiss_ramp = loop->crackle_ramp = (flavor_ramp_t){0};
    loop->saturation_glide_step = 0.0f;
    loop->hiss_lp_l = loop->hiss_lp_r = 0.0f;
    loop->crackle_env = 0.0f;
    loop->frozen = 0;
    loop->overdubbing = 0;
    loop->overdub_gain = 0.0f;
    loop->overdub_last_idx = -1;
    reverb_clear_loop(loop);
    /* Every genuinely NEW take (this function only ever closes a fresh
     * LOOP_RECORDING, never fires for an overdub — see the "overdub" branch
     * of set_param's master_record handler) starts Drive at 0 (auto-chases
     * back toward its own knob value, unchanged v1 behavior) and Warp/
     * Darken/Hiss/VINYL fully untouched (flavor_ramp_t reset above) —
     * "starts clean", each flavor doing nothing until the user turns it.
     * Nothing here is randomized (removed on-device feedback 2026-08-25: a
     * loop landing on a random Chaos/Hiss ceiling per take was jarring and
     * inconsistent take to take). */
    loop->state = LOOP_LOOPING;
}

/* ---- instance lifecycle ---- */

static void init_loop(loop_engine_t *loop, uint32_t rng_seed) {
    memset(loop, 0, sizeof(*loop));
    /* Age starts FULL (300s, the max — was 180s/3min); Warp/Darken/Hiss/
     * VINYL start at minimum, UNTOUCHED (flavor_ramp_t's zeroed target/step/
     * touched from the memset above is exactly that — nothing to set here).
     * Drive (saturation) is the one flavor knob that keeps a nonzero
     * baseline default, since it's still the v1 auto-chase-from-0 knob, not
     * a v2 ramp — see the flavor timing model v2 comment. */
    loop->decay_rate = 300.0f;
    loop->saturation = 0.25f;
    loop->erase_fade_gain = 1.0f;
    loop->forgotten_at          = TIME_NOT_SET;
    loop->rng_state = rng_seed;
}

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;

    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;

    /* distinct, nonzero seeds so each loop's noise/randomization sequence
     * is independent of the others */
    static const uint32_t seeds[NUM_LOOPS] = {
        0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu
    };

    for (int i = 0; i < NUM_LOOPS; i++) {
        init_loop(&s->loops[i], seeds[i]);
        s->loops[i].buffer = (frame16_t *)calloc((size_t)BUFFER_CAPACITY, sizeof(frame16_t));
        if (!s->loops[i].buffer || reverb_alloc_loop(&s->loops[i]) != 0) {
            free(s->loops[i].buffer); /* no-op if it was the reverb alloc that failed */
            for (int j = 0; j < i; j++) {
                free(s->loops[j].buffer);
                reverb_free_loop(&s->loops[j]);
            }
            free(s);
            return NULL;
        }
        s->loops[i].capacity_frames = BUFFER_CAPACITY;
        s->loop_volume[i] = DEFAULT_LOOP_VOLUME;
    }

    s->input_routing = ROUTE_A;  /* default — no OFF state to start unrouted in */

    return s;
}

static void v2_destroy_instance(void *i) {
    inst_t *s = (inst_t *)i;
    if (!s) return;
    for (int li = 0; li < NUM_LOOPS; li++) {
        free(s->loops[li].buffer);
        reverb_free_loop(&s->loops[li]);
    }
    free(s);
}

/* ---- audio ---- */

/* How much of a flavour knob is in force right now: some of it the moment
 * you turn it, the rest arriving as the take ages. See FLAVOUR_IMMEDIATE. */
static inline float flavour_reach(float knob, float age) {
    float a = clampf(age / FLAVOUR_AGE_FULL, 0.0f, 1.0f);
    return clampf(knob, 0.0f, 1.0f) *
           (FLAVOUR_IMMEDIATE + (1.0f - FLAVOUR_IMMEDIATE) * a);
}

static void v2_process_block(void *instance, int16_t *lr, int frames) {
    inst_t *s = (inst_t *)instance;
    if (!s) return;

    /* Per-block prepass: anything needing powf/expf is computed once per
     * loop per block instead of once per sample. */
    for (int li = 0; li < NUM_LOOPS; li++) {
        loop_engine_t *loop = &s->loops[li];
        if (loop->state != LOOP_LOOPING) continue;
        float age = 1.0f - clampf(loop->memory, 0.0f, 1.0f);

        loop->warp_amt = flavour_reach(loop->applied_wow, age);
        loop->dropout_p = flavour_reach(loop->applied_crackle, age) *
                          (DROPOUT_MAX_RATE_HZ / SAMPLE_RATE);

        float d = flavour_reach(loop->applied_hf_loss, age);
        if (d <= 0.0001f) {
            loop->darken_a = 1.0f;          /* exact bypass */
        } else {
            float fc = DARKEN_FC_MAX * powf(DARKEN_FC_MIN / DARKEN_FC_MAX, d);
            loop->darken_a = 1.0f - expf(-2.0f * PI_F * fc / SAMPLE_RATE);
            if (loop->darken_a > 1.0f) loop->darken_a = 1.0f;
        }
        loop->darken_wash  = clampf((d - DARKEN_WASH_KNEE) /
                                    (1.0f - DARKEN_WASH_KNEE), 0.0f, 1.0f);
        loop->darken_damp1 = d * DARKEN_DAMPING;
        loop->darken_fb    = REVERB_FEEDBACK_MIN +
                             d * (REVERB_FEEDBACK_MAX - REVERB_FEEDBACK_MIN);
    }

    for (int i = 0; i < frames; i++) {
        int16_t raw_dry_l = lr[i * 2];
        int16_t raw_dry_r = lr[i * 2 + 1];

        float wet_total_l = 0.0f, wet_total_r = 0.0f;

        for (int li = 0; li < NUM_LOOPS; li++) {
            loop_engine_t *loop = &s->loops[li];
            float wet_l = 0.0f, wet_r = 0.0f;

            switch (loop->state) {
            case LOOP_IDLE: {
                /* Recording is a manual gesture now (Master page's REC
                 * trigger, in set_param) — an idle loop does nothing at all
                 * per-sample, routed or not. */
                break;
            }

            case LOOP_RECORDING: {
                /* Invariant: only the routed loop is ever RECORDING — a
                 * routing change away from a RECORDING loop closes it
                 * synchronously in set_param before selection can move
                 * (see the "input_routing" handler below), so this branch
                 * is never reached for an unrouted loop. */
                if (loop->write_head < loop->capacity_frames) {
                    loop->buffer[loop->write_head].l = raw_dry_l;
                    loop->buffer[loop->write_head].r = raw_dry_r;
                    loop->write_head++;
                }
                /* Buffer-full is the one remaining AUTOMATIC close — a safety
                 * cap so forgetting to press REC again cannot record forever.
                 * The deliberate close is master_record's second press, in
                 * set_param. */
                if (loop->write_head >= loop->capacity_frames) close_recording(loop);
                break;
            }

            case LOOP_LOOPING: {
                /* How far this take has decayed, 0 on a fresh one and 1 as
                 * it disappears. Warp, Darken and Hiss are all scaled by it
                 * (2026-08-27) so their knobs set where the loop ENDS UP
                 * rather than how it sounds right now: a new take plays
                 * back close to clean and falls apart as it ages, which is
                 * the arc the Disintegration Loops have and the old
                 * always-on-from-the-first-pass behaviour did not.
                 *
                 * Frozen loops hold their `age` too, since memory is what
                 * stops — a frozen take stays exactly as ruined as it was. */
                float age = 1.0f - clampf(loop->memory, 0.0f, 1.0f);

                /* Warp: wow + flutter + a random drift, see WOW_MOD_DEPTH.
                 * Flutter enters on the square of the knob so the bottom of
                 * the travel is a slow sag and only the top is a transport
                 * tearing itself apart. */
                if (loop->warp_drift_countdown <= 0) {
                    loop->warp_drift_target = rng_bipolar(&loop->rng_state);
                    loop->warp_drift_countdown =
                        (int)(SAMPLE_RATE * DRIFT_HOLD_SECONDS);
                }
                loop->warp_drift_countdown--;
                loop->warp_drift += (loop->warp_drift_target - loop->warp_drift) /
                                    (DRIFT_GLIDE_SECONDS * SAMPLE_RATE);

                float w = loop->warp_amt;
                float mod = (sinf(loop->wow_phase) * WOW_MOD_DEPTH +
                             loop->warp_drift * DRIFT_MOD_DEPTH) * w +
                            sinf(loop->flutter_phase) * FLUTTER_MOD_DEPTH * w * w;
                double speed = 1.0 + mod;

                loop->wow_phase += 2.0f * PI_F * WOW_RATE_HZ / SAMPLE_RATE;
                if (loop->wow_phase >= 2.0f * PI_F) loop->wow_phase -= 2.0f * PI_F;
                loop->flutter_phase += 2.0f * PI_F * FLUTTER_RATE_HZ / SAMPLE_RATE;
                if (loop->flutter_phase >= 2.0f * PI_F) loop->flutter_phase -= 2.0f * PI_F;

                /* interpolated buffer read (recorded_length > 0 is
                 * guaranteed while LOOPING — enforced by the
                 * MIN_RECORDED_FRAMES floor in close_recording) */
                int idx0 = (int)floor(loop->read_head);
                if (idx0 >= loop->recorded_length) idx0 %= loop->recorded_length;
                int idx1 = idx0 + 1;
                if (idx1 >= loop->recorded_length) idx1 = 0;
                float frac = (float)(loop->read_head - floor(loop->read_head));

                /* Overdub: master_record's second press while LOOPING (not
                 * RECORDING) layers new dry input into the EXISTING take at
                 * the loop's own playback position, rather than replacing
                 * it or being a no-op — the loop keeps looping/decaying/
                 * fading exactly as it already was, and does NOT touch the
                 * flavor knobs at all (see the master_record set_param
                 * handler); only a real erase resets those. Writes to idx0
                 * only (not interpolated across idx0/idx1) and hard-clips
                 * in int32 space, same convention as mix_dry_wet — simplest
                 * correct mixing, not spectrally shaped.
                 *
                 * Three things here exist to stop the overdub putting
                 * clicks into the take (reported on device 2026-08-27).
                 * All three write edges INTO the buffer, so they are
                 * permanent and repeat once per pass — which is what made
                 * them obvious:
                 *
                 *   1. `overdub_gain` ramps rather than switching. The
                 *      toggle lands on an arbitrary sample; writing full
                 *      input from that sample on is a step edge.
                 *   2. Every frame between the last written index and this
                 *      one is written, not just idx0. With Warp up, speed
                 *      != 1: at speed < 1 the same idx0 comes round twice
                 *      and got the input added TWICE; at speed > 1 an index
                 *      was skipped entirely, so a frame of the overdub was
                 *      simply missing. Both read as grit.
                 *   3. A skipped span is filled with the same sample rather
                 *      than left alone, so the overdub stays continuous
                 *      across the gap instead of stepping over it.
                 *
                 * The loop seam is NOT crossfaded — an overdub that runs
                 * past the end of the take wraps and lands on top of its own
                 * beginning, which is the behaviour a tape loop has. */
                {
                    float g_target = loop->overdubbing ? 1.0f : 0.0f;
                    float g_step   = 1.0f / (OVERDUB_FADE_SECONDS * SAMPLE_RATE);
                    if (loop->overdub_gain < g_target) {
                        loop->overdub_gain += g_step;
                        if (loop->overdub_gain > g_target) loop->overdub_gain = g_target;
                    } else if (loop->overdub_gain > g_target) {
                        loop->overdub_gain -= g_step;
                        if (loop->overdub_gain < g_target) loop->overdub_gain = g_target;
                    }
                }
                if (loop->overdub_gain > 0.0f) {
                    int16_t add_l = (int16_t)(raw_dry_l * loop->overdub_gain);
                    int16_t add_r = (int16_t)(raw_dry_r * loop->overdub_gain);
                    int from = loop->overdub_last_idx;
                    int span = (from < 0) ? 0
                             : (idx0 - from + loop->recorded_length)
                                   % loop->recorded_length;
                    /* span == 0 with a valid `from` means the read head has
                     * not reached a new frame yet (speed < 1) — the frame is
                     * already written, and adding to it again is exactly the
                     * doubling this block exists to stop. A span past the cap
                     * is a jump, not a skip: write only where we are. */
                    if (from < 0 || span > OVERDUB_MAX_FILL) {
                        overdub_add(loop, idx0, add_l, add_r);
                    } else {
                        for (int k = 1; k <= span; k++) {
                            overdub_add(loop, (from + k) % loop->recorded_length,
                                        add_l, add_r);
                        }
                    }
                    loop->overdub_last_idx = idx0;
                } else {
                    loop->overdub_last_idx = -1;
                }

                /* VINYL dropouts — see DROPOUT_MAX_RATE_HZ. Destructive, so
                 * the hole is there on every later pass too, and deeper each
                 * time one lands on top of another. */
                if (loop->dropout_left > 0) {
                    float pos = 1.0f - (float)loop->dropout_left / (float)loop->dropout_len;
                    /* 1 at both edges, 0 in the middle — a parabola rather
                     * than a cosine, to keep a transcendental out of the
                     * sample loop. The edges are what stop a hole clicking. */
                    float shape = 1.0f - 4.0f * pos * (1.0f - pos);
                    float g = DROPOUT_DEPTH + (1.0f - DROPOUT_DEPTH) * shape;
                    int c = loop->dropout_cursor;
                    loop->buffer[c].l = (int16_t)(loop->buffer[c].l * g);
                    loop->buffer[c].r = (int16_t)(loop->buffer[c].r * g);
                    loop->dropout_cursor = (c + 1) % loop->recorded_length;
                    loop->dropout_left--;
                } else if (!loop->frozen && !loop->erasing &&
                           rng_range(&loop->rng_state, 0.0f, 1.0f) < loop->dropout_p) {
                    float ms = DROPOUT_MIN_MS +
                               rng_range(&loop->rng_state, 0.0f, 1.0f) *
                               (DROPOUT_MAX_MS - DROPOUT_MIN_MS);
                    int len = (int)(ms * SAMPLE_RATE / 1000.0f);
                    if (len > loop->recorded_length / 2) len = loop->recorded_length / 2;
                    if (len > 0) {
                        loop->dropout_left = loop->dropout_len = len;
                        loop->dropout_cursor = idx0;
                    }
                }

                float raw_l = (loop->buffer[idx0].l * (1.0f - frac) + loop->buffer[idx1].l * frac) / 32768.0f;
                float raw_r = (loop->buffer[idx0].r * (1.0f - frac) + loop->buffer[idx1].r * frac) / 32768.0f;

                /* 1. Darken — a wash of reverb that gets more present,
                 * darker, AND longer as the knob rises, replacing the old
                 * one-pole LP filter (reported 2026-08-25 as "almost can't
                 * hear it even at maximum"). Scaled-down Schroeder-Moorer
                 * reverb (REVERB_NUM_COMBS combs + REVERB_NUM_ALLPASS
                 * allpass per channel) adapted from freeverb.c's proven
                 * 8+4 — halved because forgetful needs up to NUM_LOOPS of
                 * these running at once, unlike freeverb's single instance.
                 * wet-mix amount, damping (darkness) AND feedback (decay
                 * length — "into a wall of reverb", added 2026-08-25) are
                 * all driven by the SAME applied_hf_loss chase value, so
                 * presence/darkness/length build together off one knob.
                 * wet_amount=0 is an exact raw_l/raw_r passthrough. */
                /* (a) the lowpass — this is the part you hear across the
                 * whole travel. darken_a == 1 is an exact passthrough. */
                float dark_l = raw_l, dark_r = raw_r;
                if (loop->darken_a < 1.0f) {
                    for (int k = 0; k < DARKEN_POLES; k++) {
                        loop->darken_lp_l[k] += loop->darken_a * (dark_l - loop->darken_lp_l[k]);
                        dark_l = loop->darken_lp_l[k];
                        loop->darken_lp_r[k] += loop->darken_a * (dark_r - loop->darken_lp_r[k]);
                        dark_r = loop->darken_lp_r[k];
                    }
                }

                /* (b) the wash, over the top of the range only. Fed the
                 * filtered signal so the tail is as dark as the loop. */
                float filt_l, filt_r;
                if (loop->darken_wash > 0.0f) {
                    float damp1 = loop->darken_damp1;
                    float damp2 = 1.0f - damp1;
                    float rev_input = (dark_l + dark_r) * 0.5f;
                    float rev_l = 0.0f, rev_r = 0.0f;
                    for (int c = 0; c < REVERB_NUM_COMBS; c++) {
                        rev_l += reverb_comb_process(&loop->comb_l[c], rev_input, loop->darken_fb, damp1, damp2);
                        rev_r += reverb_comb_process(&loop->comb_r[c], rev_input, loop->darken_fb, damp1, damp2);
                    }
                    rev_l *= 1.0f / (float)REVERB_NUM_COMBS;
                    rev_r *= 1.0f / (float)REVERB_NUM_COMBS;
                    for (int a = 0; a < REVERB_NUM_ALLPASS; a++) {
                        rev_l = reverb_allpass_process(&loop->allpass_l[a], rev_l);
                        rev_r = reverb_allpass_process(&loop->allpass_r[a], rev_r);
                    }
                    filt_l = dark_l + (rev_l - dark_l) * loop->darken_wash;
                    filt_r = dark_r + (rev_r - dark_r) * loop->darken_wash;
                } else {
                    filt_l = dark_l;
                    filt_r = dark_r;
                }

                /* 2. Saturation (Warmth) — crossfade between the dry
                 * (filtered) signal and a FIXED full-drive tanh curve,
                 * scaled by sat_amount, rather than modulating drive itself.
                 * Modulating drive left a floor at drive=1 when sat_amount
                 * was 0, and tanh(x)/tanh(1) is not identity. The crossfade
                 * makes sat_amount=0 an exact filt_l/filt_r passthrough
                 * while sat_amount=1 reproduces the previous full-drive
                 * behavior unchanged. */
                float sat_amount = clampf(loop->applied_saturation, 0.0f, 1.0f);
                float drive = 1.0f + SATURATION_MAX_DRIVE;
                float driven_l = tanhf(filt_l * drive) / tanhf(drive);
                float driven_r = tanhf(filt_r * drive) / tanhf(drive);
                float sat_l = filt_l + (driven_l - filt_l) * sat_amount;
                float sat_r = filt_r + (driven_r - filt_r) * sat_amount;

                /* 3. Hiss — highpassed, not raw broadband noise. Reported
                 * from hardware 2026-08-25 as "just sounds like white
                 * noise": raw per-sample bipolar noise IS flat-spectrum
                 * white noise, which reads as static, not tape hiss. Tape
                 * hiss is brighter and thinner — mostly high frequencies,
                 * with the low end rolled off. Removing the noise's own low
                 * end gets there: track a slow lowpass of the noise
                 * (hiss_lp_l/r) and subtract it out, leaving only what the
                 * lowpass filtered away — the complementary-filter trick,
                 * applied to the NOISE source rather than to the signal
                 * (which the Darken stage above already handles). Pacing is
                 * entirely turn-driven now (flavor timing model v2, see
                 * HISS_CHASE_RATE_SCALE's old location) — Hiss has no
                 * special-cased rate of its own any more. */
                /* Hiss is the one flavour that is NOT part of the music,
                 * and that is the whole point of it. It used to be summed
                 * in here and then multiplied by `memory` along with
                 * everything else, so the noise floor faded out in lockstep
                 * with the take — at 20% memory you heard a quiet mix with
                 * its hiss intact, rather than a loop disappearing INTO
                 * hiss. Reported twice as "too loud", and halved twice
                 * (0.045 -> 0.029 -> 0.0146) without fixing it, because the
                 * complaint was never really about level: it was audible at
                 * the start, where there should be none, and gone by the
                 * end, where it should be all that is left.
                 *
                 * So it is kept out of the memory multiply below and scaled
                 * by `age` instead — silent on a fresh take, and the last
                 * thing standing on a dying one. HISS_CEILING went back up
                 * accordingly: it is now only ever reached at full decay
                 * with the knob at maximum. */
                float hiss_amount = clampf(loop->applied_hiss, 0.0f, 1.0f)
                                    * HISS_CEILING * age;
                float noise_l = rng_bipolar(&loop->rng_state);
                float noise_r = rng_bipolar(&loop->rng_state);
                loop->hiss_lp_l += HISS_COLOR_COEFF * (noise_l - loop->hiss_lp_l);
                loop->hiss_lp_r += HISS_COLOR_COEFF * (noise_r - loop->hiss_lp_r);
                float hiss_only_l = (noise_l - loop->hiss_lp_l) * hiss_amount;
                float hiss_only_r = (noise_r - loop->hiss_lp_r) * hiss_amount;
                float hiss_l = sat_l;
                float hiss_r = sat_r;

                /* 4. VINYL crackle — mixed in ADDITIVELY right alongside
                 * Hiss, not a post-hoc gate (see CRACKLE_DUST_MAX_PROB for
                 * why the old chaos-gate dropout was replaced). Two-stage
                 * knob mapping (2026-08-25): the bottom half of
                 * applied_crackle's range only raises VOLUME (density held
                 * at a fixed low baseline the whole time), and the top half
                 * continues raising volume while ALSO raising density up to
                 * its own ceiling — see the constant block's comment for
                 * why. Mono (one envelope added identically to both
                 * channels): a physical surface click is a single point in
                 * space, not independent per-channel noise. */
                float crackle_v = clampf(loop->applied_crackle, 0.0f, 1.0f);
                float crackle_volume = crackle_v;  /* linear across the whole knob range */
                float crackle_density;
                if (crackle_v <= 0.5f) {
                    crackle_density = CRACKLE_BASELINE_DENSITY_FRAC;
                } else {
                    float t = (crackle_v - 0.5f) / 0.5f;
                    crackle_density = CRACKLE_BASELINE_DENSITY_FRAC +
                        t * (CRACKLE_MAX_DENSITY_FRAC - CRACKLE_BASELINE_DENSITY_FRAC);
                }
                loop->crackle_env *= CRACKLE_ENV_DECAY;
                if (rng_range(&loop->rng_state, 0.0f, 1.0f) < crackle_density * CRACKLE_DUST_MAX_PROB) {
                    loop->crackle_env += rng_bipolar(&loop->rng_state) * CRACKLE_DUST_GAIN;
                }
                if (rng_range(&loop->rng_state, 0.0f, 1.0f) < crackle_density * CRACKLE_POP_MAX_PROB) {
                    loop->crackle_env += rng_bipolar(&loop->rng_state) * CRACKLE_POP_GAIN;
                }
                float crackle_l = hiss_l + loop->crackle_env * crackle_volume;
                float crackle_r = hiss_r + loop->crackle_env * crackle_volume;

                /* 5. Level scaling — this loop's own contribution fades with
                 * memory, and independently with erase_fade_gain (1.0
                 * unless an erase is in progress — see the erase handler
                 * and step 7 below). Two separate multipliers rather than
                 * folding erase into memory itself: memory also drives every
                 * flavor's applied_* target above via the chase, and erasing
                 * is a deliberate "let go of this now" fade-to-silence, not
                 * an accelerated version of the loop's own aging. */
                wet_l = (crackle_l * loop->memory + hiss_only_l) * loop->erase_fade_gain;
                wet_r = (crackle_r * loop->memory + hiss_only_r) * loop->erase_fade_gain;

                /* 6. Master-page volume is applied once, after summing all
                 * four loops' wet below — not here per-loop-in-isolation,
                 * since it's the same multiply either way and this keeps
                 * the per-loop block symmetric with step 2. */

                /* advance + wrap */
                loop->read_head += speed;
                if (loop->read_head >= loop->recorded_length) {
                    loop->read_head -= loop->recorded_length;
                }

                /* Continuous, wall-clock decay: memory drains at a constant
                 * rate over decay_rate SECONDS, one sample at a time,
                 * regardless of recorded_length or how often the loop wraps.
                 * Previously this only stepped ONCE PER WRAP (1/decay_rate
                 * per repeat), which coupled total disintegration time to
                 * how long the take was — a 1s blip and an 8s take at the
                 * "same" setting vanished at wildly different real-world
                 * speeds — and staircased visibly/audibly for a long take,
                 * since every flavor's own chase target is scaled by the
                 * live knob value directly, not by memory. Per-sample decay
                 * makes the loop's own fade smooth, independent of recording
                 * length.
                 *
                 * Warp/Darken/Hiss/VINYL are advanced using their OWN
                 * flavor_ramp_t's `.step` — a value computed once at the
                 * moment of the most recent set_param, not here (see
                 * flavor_ramp_set_param and the flavor timing model v2
                 * comment) — so their pacing is entirely turn-driven, not
                 * tied to this shared step at all. All five still read their
                 * PRIOR applied_* value before this decrement, same as
                 * memory, so "current sample sees the value before its own
                 * step" stays consistent across the board.
                 *
                 * master_freeze suspends MEMORY DECAY and Drive's normal
                 * auto-chase rate specifically — but NOT the flavor ramps
                 * themselves, which keep advancing every sample regardless
                 * of frozen, using whatever `.step` is currently set. While
                 * frozen that step is the short FROZEN_GLIDE_SECONDS glide
                 * a set_param write just computed (see flavor_ramp_set_param
                 * and the "saturation" branch of loop_set_param for Drive's
                 * parallel `saturation_glide_step`), so turning a knob while
                 * frozen is heard gliding in immediately rather than either
                 * snapping or waiting on the (suspended) long ramp. With no
                 * knob turned, `.step` is whatever it last settled at
                 * (typically 0, already at target), so this is a no-op —
                 * freezing alone doesn't move anything on its own. Erasing
                 * suspends everything here too, for the same reason as
                 * always: an erase in progress shouldn't race anything else
                 * to decide how the loop empties. */
                if (!loop->erasing) {
                    if (!loop->frozen) {
                        float step = 1.0f / (loop->decay_rate * SAMPLE_RATE);
                        loop->memory -= step;
                        loop->applied_saturation = chase(loop->applied_saturation, loop->saturation, step);
                    } else {
                        loop->applied_saturation = chase(loop->applied_saturation, loop->saturation, loop->saturation_glide_step);
                    }
                    loop->applied_wow     = chase(loop->applied_wow,     loop->wow_ramp.target,     loop->wow_ramp.step);
                    loop->applied_hf_loss = chase(loop->applied_hf_loss, loop->hf_loss_ramp.target, loop->hf_loss_ramp.step);
                    loop->applied_hiss    = chase(loop->applied_hiss,    loop->hiss_ramp.target,    loop->hiss_ramp.step);
                    loop->applied_crackle = chase(loop->applied_crackle, loop->crackle_ramp.target, loop->crackle_ramp.step);
                }
                if (loop->memory <= 0.0f) {
                    loop->memory = 0.0f;
                    loop->state = LOOP_FORGOTTEN;
                }

                /* 7. Erase fade-out: once armed (the erase handler), ramp
                 * erase_fade_gain to 0 over ERASE_FADE_SECONDS — step 5
                 * above already applies it every sample, so the loop
                 * audibly fades out in place — then clear for real. reset_
                 * take() also clears erasing/erase_fade_gain, so a fresh
                 * take (or the natural IDLE/FORGOTTEN cycle) never inherits
                 * a stale fade. */
                if (loop->erasing) {
                    loop->erase_fade_gain -= 1.0f / (ERASE_FADE_SECONDS * SAMPLE_RATE);
                    if (loop->erase_fade_gain <= 0.0f) {
                        loop->state = LOOP_IDLE;
                        reset_take(loop);
                        loop->memory = 0.0f;
                        loop->forgotten_at = TIME_NOT_SET;
                    }
                }
                break;
            }

            case LOOP_FORGOTTEN: {
                /* Silence flush + drop to IDLE, regardless of routing —
                 * "decay progresses regardless of routing" applies to
                 * forgetting too. See step 2's comment: collapsing this
                 * into the very next sample rather than holding a distinct
                 * state for a full callback is audibly identical, since
                 * LOOPING's own level-scaling already guarantees silence
                 * at the transition sample. */
                wet_l = wet_r = 0.0f;
                loop->forgotten_at = s->total_frames;
                loop->state = LOOP_IDLE;
                reset_take(loop);
                loop->memory = 0.0f;
                break;
            }
            }

            wet_total_l += wet_l * s->loop_volume[li];
            wet_total_r += wet_r * s->loop_volume[li];
        }

        lr[i * 2]     = mix_dry_wet(raw_dry_l, wet_total_l);
        lr[i * 2 + 1] = mix_dry_wet(raw_dry_r, wet_total_r);
    }

    s->total_frames += (uint64_t)frames;
}

/* Simple JSON number extraction (matches freeverb's helper) */
static int json_get_float(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = atof(p);
    return 0;
}

/* Memory-percentage-to-word mapping (design doc "Screen / Feedback"). Takes
 * the already-rounded display percentage (0..100), not raw float memory —
 * `1.0f / decay_rate` isn't exactly representable, so repeated subtraction
 * lands memory a hair under a clean boundary (0.899999976f instead of
 * 0.90f). Bucketing on that raw float while printing a separately-rounded
 * "%.0f%%" let the two disagree: "90% (Fading)", contradicting the doc's own
 * "90-100% = Vivid" range. Deriving both the printed number and the word
 * from one rounded int keeps them self-consistent by construction. Only
 * meaningful while LOOPING: memory is always in (0, 1] there — the instant
 * it reaches 0, process_block flips state to LOOP_FORGOTTEN in the same
 * block, so the 0%/idle case is handled by state, not by this bucket. */
static const char *memory_word(int pct) {
    if (pct >= 90) return "Vivid";
    if (pct >= 40) return "Fading";
    if (pct >= 10) return "Hazy";
    return "Almost gone";
}

/* The on-screen state line for a loop's own page (design doc "Screen /
 * Feedback"): "Ready", "Recording", "Looping - NN% (word)", or
 * "Forgotten". Distinct from `state` itself: FORGOTTEN reads back here for
 * FORGOTTEN_DISPLAY_MS after the engine has already reset to IDLE, since the
 * engine's own instant reset (LOOP_FORGOTTEN in process_block) would
 * otherwise make FORGOTTEN unobservable to any UI poll landing between
 * process_block calls. Reachable via get_param("loopX_status").
 * total_frames lives on inst_t, not loop_engine_t, but the comparison only
 * needs the DELTA since forgotten_at (itself stamped from that same shared
 * counter) — so the caller passes the current total_frames in rather than
 * this function reaching for it. */
static int loop_status_text(const loop_engine_t *loop, uint64_t total_frames, char *buf, int len) {
    if (loop->forgotten_at != TIME_NOT_SET &&
        total_frames - loop->forgotten_at < FORGOTTEN_DISPLAY_FRAMES) {
        return snprintf(buf, len, "Forgotten");
    }
    if (loop->erasing) {
        return snprintf(buf, len, "Erasing...");
    }
    switch (loop->state) {
        case LOOP_IDLE:      return snprintf(buf, len, "Ready");
        case LOOP_RECORDING: return snprintf(buf, len, "Recording");
        case LOOP_LOOPING: {
            int pct = (int)lroundf(loop->memory * 100.0f);
            return snprintf(buf, len, "Looping - %d%% (%s)", pct, memory_word(pct));
        }
        case LOOP_FORGOTTEN: return snprintf(buf, len, "Forgotten");
    }
    return snprintf(buf, len, "Ready");
}

/* Master page's ECHO readout (access "read"): one character per loop in
 * A/B/C/D order. '-' for idle/forgotten, 'R' for recording, 'O' for
 * overdubbing, 'F' for frozen (2026-08-25 — all three checked before the
 * decile digit, since each of those is also LOOPING and would otherwise
 * just show its stuck-or-ordinary percentage with no visual cue at all),
 * otherwise a single digit giving memory rounded DOWN to the nearest 10%
 * (e.g. 74% -> '7'). Declared as an enum (see the chain_params comment at
 * its declaration), which renders as two lines of up to 3 characters each.
 *
 * The '_' at the midpoint is a deliberate, load-bearing separator, not a
 * decoration: font5x3.mjs's enumSquareLines() treats "_" as a hard line
 * break, forcing a clean A+B / C+D split every time. Without it the renderer
 * falls back to its generic word-wrap, which only breaks on a "-" sitting
 * between two alphanumeric characters — so the split landed in a different,
 * sometimes uneven place depending on which glyphs happened to be adjacent
 * (e.g. "9R--" -> "9R"/"--", but "9999" -> "999"/"9", and "R-R-" -> "R"/"R"
 * with the two idle loops silently dropped, since '-' next to '-' is not an
 * alphanumeric boundary and never split at all). Reported from hardware
 * 2026-08-24 as a bad look, not a bug in the data. */
static char loop_status_char(const loop_engine_t *loop, uint64_t total_frames) {
    int forgotten_display = (loop->forgotten_at != TIME_NOT_SET &&
        total_frames - loop->forgotten_at < FORGOTTEN_DISPLAY_FRAMES);
    if (forgotten_display || loop->state == LOOP_IDLE || loop->state == LOOP_FORGOTTEN) {
        return '-';
    }
    if (loop->state == LOOP_RECORDING) {
        return 'R';
    }
    if (loop->overdubbing) {
        return 'O';
    }
    /* An erase outranks frozen: erasing a frozen loop is the one case
     * where `frozen` is still set but the thing you want to watch is the
     * fade, not the freeze. Counts down on erase_fade_gain rather than
     * memory, so the digit runs the 10s of ERASE_FADE_SECONDS instead of
     * the minutes of decay_rate — the same countdown, sped up to the time
     * it now actually takes to go. Without this the character sat on
     * whatever it read when erase fired and never moved again. */
    if (loop->erasing) {
        int e = (int)(loop->erase_fade_gain * 10.0f);
        if (e > 9) e = 9;
        if (e < 0) e = 0;
        return (char)('0' + e);
    }
    if (loop->frozen) {
        return 'F';
    }
    int decile = (int)(loop->memory * 10.0f);
    if (decile > 9) decile = 9;
    if (decile < 0) decile = 0;
    return (char)('0' + decile);
}

static int master_loops_overview_text(const inst_t *s, char *buf, int len) {
    char code[NUM_LOOPS];
    for (int i = 0; i < NUM_LOOPS; i++) {
        code[i] = loop_status_char(&s->loops[i], s->total_frames);
    }
    return snprintf(buf, len, "%c%c_%c%c", code[0], code[1], code[2], code[3]);
}

/* Returns the loop index (0..3) and the suffix after "loopX_" if `key`
 * names a per-loop param, or NULL if `key` isn't one (including Master-page
 * keys that merely start with "loop" — see the "loopX_volume" note below). */
static const char *loop_key_suffix(const char *key, int *loop_index_out) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        size_t plen = strlen(LOOP_PREFIXES[i]);
        if (strncmp(key, LOOP_PREFIXES[i], plen) == 0) {
            *loop_index_out = i;
            return key + plen;
        }
    }
    return NULL;
}

static void loop_set_param(loop_engine_t *loop, const char *suffix, const char *val) {
    if (strcmp(suffix, "decay_rate") == 0) {
        loop->decay_rate = clampf((float)atof(val), 3.0f, 300.0f);
    } else if (strcmp(suffix, "wow") == 0) {
        flavor_ramp_set_param(&loop->wow_ramp, &loop->applied_wow, (float)atof(val), loop->memory, loop->decay_rate, loop->frozen);
    } else if (strcmp(suffix, "hf_loss") == 0) {
        flavor_ramp_set_param(&loop->hf_loss_ramp, &loop->applied_hf_loss, (float)atof(val), loop->memory, loop->decay_rate, loop->frozen);
    } else if (strcmp(suffix, "hiss") == 0) {
        flavor_ramp_set_param(&loop->hiss_ramp, &loop->applied_hiss, (float)atof(val), loop->memory, loop->decay_rate, loop->frozen);
    } else if (strcmp(suffix, "saturation") == 0) {
        /* Drive isn't a flavor_ramp_t (still the v1 auto-chase knob — see
         * the struct comment), but while frozen it gets the same short-glide
         * treatment as the other four (FROZEN_GLIDE_SECONDS), via its own
         * one-off `saturation_glide_step` — the LOOPING case's per-sample
         * update reads this instead of the normal decay_rate-derived rate
         * whenever the loop is frozen. */
        loop->saturation = clampf((float)atof(val), 0.0f, 1.0f);
        if (loop->frozen) {
            float glide_samples = FROZEN_GLIDE_SECONDS * (float)SAMPLE_RATE;
            loop->saturation_glide_step = fabsf(loop->saturation - loop->applied_saturation) / glide_samples;
        }
    } else if (strcmp(suffix, "chaos") == 0) {
        /* wire key stays "chaos" (see the crackle field comment on
         * loop_engine_t) — this is VINYL's live value. */
        flavor_ramp_set_param(&loop->crackle_ramp, &loop->applied_crackle, (float)atof(val), loop->memory, loop->decay_rate, loop->frozen);
    } else if (strcmp(suffix, "erase") == 0) {
        /* gesture-test's pattern: fire on anything that isn't the idle
         * spelling. Single click, no confirm — was double-click-confirm
         * per the design doc, removed 2026-08-25: touching the knob AND
         * jog-clicking it is already a deliberate two-part gesture nothing
         * else can produce by accident, so a second click on top of that
         * was redundant caution, not real safety.
         *
         * A LOOPING loop fades out over ERASE_FADE_SECONDS instead of
         * cutting instantly — the actual clear happens once the fade
         * reaches 0 (see the LOOPING case). Idle/Recording/Forgotten clear
         * immediately: there's either nothing playing to fade, or
         * (Recording) an unfinished take being discarded outright rather
         * than a loop being "let go of". A press that lands mid-fade is a
         * no-op — it doesn't restart the fade or double-speed it. */
        if (strcmp(val, "-") != 0 && strcmp(val, "0") != 0) {
            if (loop->state == LOOP_LOOPING) {
                if (!loop->erasing) {
                    loop->erasing = 1;
                    loop->erase_fade_gain = 1.0f;
                }
            } else {
                loop->state = LOOP_IDLE;
                reset_take(loop);
                loop->memory = 0.0f;
                loop->forgotten_at = TIME_NOT_SET;
            }
        }
    }
}

static int loop_get_param(const loop_engine_t *loop, uint64_t total_frames, const char *suffix, char *buf, int len) {
    if (strcmp(suffix, "decay_rate") == 0)  return snprintf(buf, len, "%.1f", loop->decay_rate);
    if (strcmp(suffix, "wow") == 0)         return snprintf(buf, len, "%.3f", loop->wow_ramp.target);
    if (strcmp(suffix, "hf_loss") == 0)     return snprintf(buf, len, "%.3f", loop->hf_loss_ramp.target);
    if (strcmp(suffix, "hiss") == 0)        return snprintf(buf, len, "%.3f", loop->hiss_ramp.target);
    if (strcmp(suffix, "saturation") == 0)  return snprintf(buf, len, "%.3f", loop->saturation);
    if (strcmp(suffix, "chaos") == 0)       return snprintf(buf, len, "%.3f", loop->crackle_ramp.target);
    if (strcmp(suffix, "erase") == 0) {
        /* "ERASE" at rest (2026-08-25, was always "-"), "ERASING n" while
         * one is running (2026-08-27), with the same digit ECHO shows for
         * this loop so the two readouts agree.
         *
         * The earlier note here ruled "ERASING" out on a 5-char budget.
         * That budget is LABEL_CHARS, which clips the grid LABEL under the
         * icon — it does not apply to the value: erase is access "write",
         * so its resting cell draws the trigger glyph and no text at all,
         * and the only place this string appears is the touched header,
         * which is a full 128px band. */
        if (loop->erasing) {
            int e = (int)(loop->erase_fade_gain * 10.0f);
            if (e > 9) e = 9;
            if (e < 0) e = 0;
            return snprintf(buf, len, "ERASING %d", e);
        }
        return snprintf(buf, len, "ERASE");
    }
    if (strcmp(suffix, "status") == 0) {
        return loop_status_text(loop, total_frames, buf, len);
    }
    if (strcmp(suffix, "state") == 0) {
        /* Single-character code for the loop-page grid — see the knob 7
         * comment at its chain_params declaration. loopX_status (above)
         * carries the full text ("Looping - 74% (Fading)") but was never
         * wired into chain_params/ui_hierarchy at all, so it has never been
         * shown anywhere on-device; this is the fix, at the width the grid
         * actually has room for. Same '-'/'R'/digit code and shared logic as
         * master_loops_overview's per-loop character. */
        return snprintf(buf, len, "%c", loop_status_char(loop, total_frames));
    }
    return -1;
}

static void v2_set_param(void *inst, const char *key, const char *val) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        float v;
        if (json_get_float(val, "input_routing", &v) == 0) {
            int r = (int)v;
            if (r >= ROUTE_A && r <= ROUTE_D) s->input_routing = r;
            /* A freshly-restored instance has nothing RECORDING yet, so
             * there's no close-on-change side effect to run here. */
        }
        for (int i = 0; i < NUM_LOOPS; i++) {
            char key_buf[32];
            snprintf(key_buf, sizeof(key_buf), "loop%c_volume", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loop_volume[i] = clampf(v, 0.0f, 1.0f);

            snprintf(key_buf, sizeof(key_buf), "loop%c_decay_rate", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].decay_rate = clampf(v, 3.0f, 300.0f);
            /* wow/hf_loss/hiss/chaos restore straight into `.target` — a
             * state load is a snapshot, not a live knob turn, so it
             * deliberately bypasses flavor_ramp_set_param's first-touch/
             * ramp logic entirely (leaving .step/.touched at whatever this
             * freshly-created instance already has, normally zeroed). */
            snprintf(key_buf, sizeof(key_buf), "loop%c_wow", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].wow_ramp.target = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_hf_loss", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].hf_loss_ramp.target = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_hiss", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].hiss_ramp.target = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_saturation", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].saturation = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_chaos", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].crackle_ramp.target = clampf(v, 0.0f, 1.0f);
        }
        return;
    }

    /* Master page — checked by exact key BEFORE the loopX_ prefix scan,
     * since "loopA_volume" etc. share the "loopA_" prefix with Loop A's
     * own params but are actually Master-page keys. */
    if (strcmp(key, "input_routing") == 0) {
        /* The host learns a module's enum wire format from what get_param
         * returns (see page_controller.mjs's learnEnumWireFormat): since
         * get_param reports a LABEL ("A"), the host writes labels back, not
         * indices. A bare atoi(val) here parsed every label as 0 (ROUTE_A
         * before the OFF-removal reindex; formerly ROUTE_NONE) — every real
         * on-device turn silently wrote the wrong route. Bench tests never
         * caught it because they drive set_param directly with index
         * strings, bypassing the host's write path entirely. Numeric
         * fallback kept for exactly that case (and for "99"/"-1"
         * out-of-range probes in test11). */
        int new_route = -1;
        for (int i = ROUTE_A; i <= ROUTE_D; i++) {
            if (strcmp(val, ROUTE_LABELS[i]) == 0) { new_route = i; break; }
        }
        if (new_route < 0) new_route = atoi(val);
        if (new_route >= ROUTE_A && new_route <= ROUTE_D && new_route != s->input_routing) {
            loop_engine_t *old_loop = &s->loops[s->input_routing];
            if (old_loop->state == LOOP_RECORDING) {
                /* "moving on is treated as I'm done with this one" — the
                 * same close path buffer-full and master_record's second
                 * press use */
                close_recording(old_loop);
            }
            /* An overdub in progress reads the SAME shared dry input every
             * loop does (there's no per-loop input isolation) — if Route
             * moves on without stopping it, the old loop would keep layering
             * whatever the NEWLY routed loop is now receiving into ITS OWN
             * buffer, silently mixing the wrong source in. "Moving on" ends
             * an overdub for the same reason it ends a RECORDING. */
            old_loop->overdubbing = 0;
            s->input_routing = new_route;
        }
        return;
    }
    if (strcmp(key, "master_record") == 0) {
        /* Manual record start/stop, replacing the old level-detection
         * auto-trigger — fires on any non-idle write, same "not the idle
         * spelling" convention as loopX_erase. Always targets whichever
         * loop Route currently points at — there is no unrouted state to
         * no-op against any more.
         *
         * IDLE/FORGOTTEN -> RECORDING on the first press. RECORDING ->
         * LOOPING (via close_recording, same path buffer-full and a routing
         * change use) on the second. LOOPING toggles `overdubbing` instead
         * of being a no-op (2026-08-25): layers new dry input into the
         * EXISTING take (see the LOOPING case's overdub-write block)
         * without touching memory, the flavor knobs, or reverb/hiss state
         * at all — only a real erase (reset_take) resets those. A second
         * press while overdubbing turns it back off, simply continuing to
         * play the loop as before. */
        if (strcmp(val, "-") != 0 && strcmp(val, "0") != 0) {
            loop_engine_t *loop = &s->loops[s->input_routing];
            if (loop->state == LOOP_IDLE || loop->state == LOOP_FORGOTTEN || loop->erasing) {
                /* A loop mid-erase-fade counts as available too — its
                 * content is already being let go of, so a fresh press
                 * just claims it immediately instead of waiting out the
                 * fade. reset_take() clears erasing/erase_fade_gain. */
                loop->state = LOOP_RECORDING;
                reset_take(loop);
                loop->forgotten_at = TIME_NOT_SET; /* new activity preempts the display window */
            } else if (loop->state == LOOP_RECORDING) {
                close_recording(loop);
            } else if (loop->state == LOOP_LOOPING) {
                loop->overdubbing = !loop->overdubbing;
            }
        }
        return;
    }
    if (strcmp(key, "master_freeze") == 0) {
        /* Toggles frozen on whichever loop Route currently points at — same
         * targeting convention as master_record. Meaningful only while
         * LOOPING (see the `frozen` guard in the LOOPING case), but the
         * flag itself is harmless to flip in any other state: it just sits
         * inert until that loop reaches LOOPING. */
        if (strcmp(val, "-") != 0 && strcmp(val, "0") != 0) {
            loop_engine_t *loop = &s->loops[s->input_routing];
            loop->frozen = !loop->frozen;
        }
        return;
    }
    if (strcmp(key, "loopA_volume") == 0) { s->loop_volume[0] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopB_volume") == 0) { s->loop_volume[1] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopC_volume") == 0) { s->loop_volume[2] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopD_volume") == 0) { s->loop_volume[3] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    /* master_loops_overview — access "read" — deliberately unhandled */

    int li;
    const char *suffix = loop_key_suffix(key, &li);
    if (suffix) loop_set_param(&s->loops[li], suffix, val);
}

static int v2_get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !buf) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, len, "Forgetful");

    /* Master page — same exact-match-before-prefix-scan ordering as set_param */
    if (strcmp(key, "input_routing") == 0) return snprintf(buf, len, "%s", ROUTE_LABELS[s->input_routing]);
    if (strcmp(key, "loopA_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[0]);
    if (strcmp(key, "loopB_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[1]);
    if (strcmp(key, "loopC_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[2]);
    if (strcmp(key, "loopD_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[3]);
    if (strcmp(key, "master_loops_overview") == 0) return master_loops_overview_text(s, buf, len);
    if (strcmp(key, "master_record") == 0) {
        /* Button renamed Rec->Catch->Moment->HOLD->REC (2026-08-25). The
         * readout vocabulary was rebuilt the same day around a different
         * rule than its predecessors (CATCH/LAYER/LIVE/BLISS, before that
         * CAPTURE/RELIVE/PLAYING, before that REC/OVERDUB/-): each word now
         * names what the NEXT press will DO, not what's happening right
         * now — a standard transport-button convention (a play/pause button
         * reads "Pause" while playing). So: REC while idle/forgotten (next
         * press starts recording), STOP while actually recording (next
         * press stops it), DUB while looping-and-not-overdubbing (next
         * press starts an overdub), PLAY while overdubbing (next press
         * stops it, back to plain playback). Entirely a function of current
         * state — no memory of how that state was reached needed, unlike a
         * "what just happened" framing would require. All four are well
         * under the 5-char on-device budget. */
        const loop_engine_t *loop = &s->loops[s->input_routing];
        if (loop->state == LOOP_RECORDING) return snprintf(buf, len, "STOP");
        if (loop->state == LOOP_LOOPING) {
            return snprintf(buf, len, loop->overdubbing ? "PLAY" : "DUB");
        }
        return snprintf(buf, len, "REC");
    }
    if (strcmp(key, "master_freeze") == 0) {
        /* Rebuilt 2026-08-25 onto the same "next press" convention as
         * master_record (see its comment): FREEZE while unfrozen (next
         * FROZEN while frozen, AGING while not — what the loop is DOING,
         * not what the next press would do.
         *
         * This deliberately breaks the next-action convention REC still
         * follows, on device feedback 2026-08-27. The two are not the same
         * kind of control: REC's cycle has four steps and no readout of its
         * own, so naming the next action is the only way to know where in
         * the cycle you are. Freeze is a two-state toggle whose state is
         * ALREADY on screen as ECHO's `F`, so next-action naming made the
         * header contradict the character right beside it — the loop
         * showing `F` had a knob saying AGING. */
        return snprintf(buf, len, s->loops[s->input_routing].frozen ? "FROZEN" : "AGING");
    }

    int li;
    const char *suffix = loop_key_suffix(key, &li);
    if (suffix) return loop_get_param(&s->loops[li], s->total_frames, suffix, buf, len);

    if (strcmp(key, "state") == 0) {
        char json[1024];
        int pos = snprintf(json, sizeof(json), "{\"input_routing\":%d", s->input_routing);
        for (int i = 0; i < NUM_LOOPS; i++) {
            pos += snprintf(json + pos, sizeof(json) - pos, ",\"loop%c_volume\":%.4f",
                             LOOP_LETTERS[i], s->loop_volume[i]);
        }
        for (int i = 0; i < NUM_LOOPS; i++) {
            const loop_engine_t *loop = &s->loops[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"loop%c_decay_rate\":%.4f,\"loop%c_wow\":%.4f,\"loop%c_hf_loss\":%.4f,"
                "\"loop%c_hiss\":%.4f,\"loop%c_saturation\":%.4f,\"loop%c_chaos\":%.4f",
                LOOP_LETTERS[i], loop->decay_rate, LOOP_LETTERS[i], loop->wow_ramp.target,
                LOOP_LETTERS[i], loop->hf_loss_ramp.target, LOOP_LETTERS[i], loop->hiss_ramp.target,
                LOOP_LETTERS[i], loop->saturation, LOOP_LETTERS[i], loop->crackle_ramp.target);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "}");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
    }

    if (strcmp(key, "chain_params") == 0) {
        char json[8192];
        int pos = snprintf(json, sizeof(json), "[");
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"key\":\"input_routing\",\"name\":\"Send\",\"type\":\"enum\","
              "\"options\":[\"A\",\"B\",\"C\",\"D\"],\"default\":0}");
        for (int i = 0; i < NUM_LOOPS; i++) {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",{\"key\":\"loop%c_volume\",\"name\":\"%c\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":%.2f,\"step\":0.01,\"unit\":\"%%\","
                  "\"display_format\":\"%%.0f\"}",
                LOOP_LETTERS[i], LOOP_LETTERS[i], (double)DEFAULT_LOOP_VOLUME);
        }
        /* "enum", not "string": a read-only string routes through the opaque
         * knob-widget renderer (drawOpaqueBox), a single truncated line ~2
         * characters wide — built for filepath tags ("kick_01.wav" -> "KI"),
         * not a 4-character live code. An enum (even non-divable, access
         * "read") routes through the enum-square renderer instead: two lines,
         * up to 3 characters each, comfortably fitting "7R-1" as "7R"/"1".
         * Confirmed on-device: declared as "string" this always showed "--"
         * regardless of state. */
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"master_loops_overview\",\"name\":\"ECHO\",\"type\":\"enum\","
              "\"options\":[\"-\"],\"access\":\"read\"}");
        /* Manual record start/stop — replaces the old level-detection
         * auto-trigger entirely (see the set_param comment). Same trigger
         * convention as loopX_erase: any write that isn't the idle spelling
         * fires it, so a normal knob nudge can never start/stop a recording
         * by accident. */
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"master_record\",\"name\":\"REC\",\"type\":\"enum\","
              "\"options\":[\"-\",\"REC!\"],\"access\":\"write\"}");
        /* Pause/resume decay in place — same write-only trigger convention,
         * so a nudge can't freeze a loop by accident either. */
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",{\"key\":\"master_freeze\",\"name\":\"Freeze\",\"type\":\"enum\","
              "\"options\":[\"-\",\"Freeze!\"],\"access\":\"write\"}");
        for (int i = 0; i < NUM_LOOPS; i++) {
            char c = LOOP_LETTERS[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                /* Range changed from 3..60 REPEATS to 3..300 SECONDS — see
                 * the decay_rate field comment on loop_engine_t and the
                 * LOOPING case in v2_process_block. Default raised
                 * 180->300 (2026-08-25: "AGE starts at full" — 300 IS the
                 * max, see init_loop).
                 *
                 * Knob order (2026-08-25, requested on-device — second
                 * reorder, supersedes the Age/Warp/VINYL/Hiss/Darken/Drive/
                 * Status/Erase layout from earlier the same day): Age,
                 * Drive, Status, Erase, Warp, Darken, VINYL, Hiss. Wire keys
                 * are unchanged — only declaration order moves.
                 *
                 * Warp/Darken/Hiss/VINYL default to 0 ("starts at minimum,
                 * untouched" — see flavor_ramp_t and init_loop): the
                 * `"default"` value here is cosmetic/documentation only
                 * (chain_params has no wire effect on the actual runtime
                 * default, which lives in init_loop), but is kept in sync
                 * with it so a UI reading this metadata doesn't lie. Drive
                 * (saturation) keeps a nonzero default (0.25) since it's
                 * still the v1 auto-chase knob, not a v2 ramp. */
                ",{\"key\":\"loop%c_decay_rate\",\"name\":\"Age\",\"type\":\"float\","
                  "\"min\":3,\"max\":300,\"default\":300,\"step\":1,\"unit\":\"s\"}"
                ",{\"key\":\"loop%c_saturation\",\"name\":\"Drive\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.25,\"step\":0.01}"
                /* Was an inert "reserved" placeholder, now the loop's own
                 * single-character state readout — '-'/R/digit, same code
                 * and meaning as ECHO's per-loop character on Master.
                 * loopX_status (the full "Looping - 74% (Fading)" text)
                 * still exists as a get_param key but was never declared
                 * here, so it has never actually been visible on-device —
                 * this is the fix, sized to what a knob cell can hold. Named
                 * "ECHO" (was "Memory", "State", "Status" — LABEL_CHARS=5
                 * in render_page_movy.mjs ruled "Memory" out), same poetic
                 * pass as Master's own master_loops_overview. */
                ",{\"key\":\"loop%c_state\",\"name\":\"ECHO\",\"type\":\"enum\","
                  "\"options\":[\"-\"],\"access\":\"read\"}"
                ",{\"key\":\"loop%c_erase\",\"name\":\"Erase\",\"type\":\"enum\","
                  "\"options\":[\"-\",\"Erase!\"],\"access\":\"write\"}"
                ",{\"key\":\"loop%c_wow\",\"name\":\"Warp\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01}"
                ",{\"key\":\"loop%c_hf_loss\",\"name\":\"Darken\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01}"
                ",{\"key\":\"loop%c_chaos\",\"name\":\"VINYL\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01}"
                ",{\"key\":\"loop%c_hiss\",\"name\":\"Hiss\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0,\"step\":0.01}",
                c, c, c, c, c, c, c, c);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "]");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        /* Five named levels, not one flat 40-key array: the page planner
         * (page_plan.mjs) auto-splits a single level's overflow knobs[] into
         * "Main-2".."Main-5" continuation pages, with no separators between
         * them on the bank bar — that is what a flat array actually got on
         * hardware. Reachable sibling levels (root's "params" nav edges,
         * below) each get their own named grid page(s) in the same linear
         * jog-scroll sequence, which is how modules like moog/minijv get
         * pages named "Filter"/"Oscillator 1" instead of "Main-2" etc. The
         * root page itself is always titled "Main" by the page planner
         * regardless of any label declared here — a deliberate fleet-wide
         * convention, not something a per-module label can override.
         *
         * "" in a knobs[] array is a real, load-bearing gap: keyOf() passes
         * an empty string through (only literal null is filtered), and both
         * the renderer and the knob-turn/click handlers treat a falsy key as
         * "nothing here" and bail before ever touching metaIndex — true blank
         * grid space, safely a no-op if physically turned. Master's top row
         * used to have two ("" was master_reserved_1/2, then one became
         * master_record, 2026-08-25 added master_freeze into the last one —
         * Master's top row is now fully occupied); each loop page's knob 7
         * (was "reserved") is still one. */
        char json[4096];
        int pos = snprintf(json, sizeof(json),
            "{\"modes\":null,\"levels\":{"
            "\"root\":{\"label\":\"Forgetful\","
              "\"knobs\":[\"input_routing\",\"master_loops_overview\",\"master_record\",\"master_freeze\","
                "\"loopA_volume\",\"loopB_volume\",\"loopC_volume\",\"loopD_volume\"],"
              "\"params\":["
                "{\"key\":\"input_routing\",\"label\":\"Send\"},"
                "{\"key\":\"master_loops_overview\",\"label\":\"ECHO\"},"
                "{\"key\":\"master_record\",\"label\":\"REC\"},"
                "{\"key\":\"master_freeze\",\"label\":\"Freeze\"},"
                "{\"key\":\"loopA_volume\",\"label\":\"A\"},"
                "{\"key\":\"loopB_volume\",\"label\":\"B\"},"
                "{\"key\":\"loopC_volume\",\"label\":\"C\"},"
                "{\"key\":\"loopD_volume\",\"label\":\"D\"},"
                "{\"level\":\"loopA\",\"label\":\"A\"},"
                "{\"level\":\"loopB\",\"label\":\"B\"},"
                "{\"level\":\"loopC\",\"label\":\"C\"},"
                "{\"level\":\"loopD\",\"label\":\"D\"}]}");
        for (int i = 0; i < NUM_LOOPS; i++) {
            char c = LOOP_LETTERS[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"loop%c\":{\"label\":\"%c\",\"knobs\":["
                  "\"loop%c_decay_rate\",\"loop%c_saturation\",\"loop%c_state\",\"loop%c_erase\","
                  "\"loop%c_wow\",\"loop%c_hf_loss\",\"loop%c_chaos\",\"loop%c_hiss\"]}",
                c, c, c, c, c, c, c, c, c, c);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "}}");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
    }

    return -1;
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    (void)g_host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    return &g_api;
}
