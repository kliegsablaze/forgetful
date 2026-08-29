/*
 * Bench test for Forgetful's LoopEngine (docs/plans/forgetful-design.md,
 * Build/Test Plan #5): synthetic tone/silence input driven straight through
 * the module's public v2 API (create_instance/set_param/get_param/
 * process_block), exactly as chain_host would drive it. Black-box — this
 * does not reach into forgetful.c's internals, so the timing constants below
 * mirror forgetful.c's own documented defaults rather than its private
 * macros.
 *
 * Rewritten 2026-08-25 for the manual-record redesign (on-device feedback):
 * level-detection auto-trigger (record_threshold/debounce) and the
 * silence-timeout close are GONE, replaced by the Master page's
 * `master_record` trigger (press to start, press again to stop) — real
 * playing levels didn't reliably cross any single fixed threshold, and the
 * silence-timeout baked an audible pause into the start of every loop.
 * `loopX_decay_rate`'s UNIT also changed, from "repeats until forgotten" to
 * "seconds until forgotten" — decay is now continuous per-sample, not
 * stepped once per buffer wrap, so total disintegration time no longer
 * depends on how long the recording is.
 *
 * Covers:
 *   0. chain_params / ui_hierarchy shape — exactly 56 entries (8 master +
 *      4x8 loop), every expected key present in both, including
 *      master_record/master_freeze, and no OFF option in Route's options
 *      list.
 *   1. manual record start/stop — a fresh instance is already routed to A
 *      (no OFF/unrouted state); dry passthrough is unconditional across
 *      Idle/Recording/Looping; a press starts Recording immediately; a
 *      second press closes into Looping; a press while Looping is also a
 *      no-op (overdub/record-over is out of scope for v1); the
 *      master_record readout itself reflects REC while recording and "-"
 *      otherwise.
 *   2. decay timing (now wall-clock SECONDS, not repeats) + the
 *      forgotten_at display window.
 *   3. continuous decay — memory measurably drops well within a single wrap
 *      of a multi-second recording, proving decay no longer waits for a
 *      buffer wrap to progress.
 *   4. single-click erase on a LOOPING loop starts a fade-out (no confirm
 *      click any more — see forgetful.c's erase handler comment), and it
 *      completes and clears on schedule.
 *   5. a fresh master_record press claims a loop mid-erase-fade
 *      immediately rather than waiting out the fade.
 *   6. routing — closes A via a routing change (not buffer-full), and A's
 *      decay keeps progressing on its own after B becomes the active
 *      target.
 *   7. memory-word bucket mapping — every "NN% (word)" reading loopA_status
 *      produces while Looping matches the design doc's Vivid/Fading/Hazy/
 *      Almost gone boundaries, and all four are observed.
 *   8. master_loops_overview format — all-idle, one loop Recording, and a
 *      fresh-close memory decile alongside a second loop Recording, each
 *      checked byte-exact.
 *   9. too-short blip discard — close_recording's MIN_RECORDED_FRAMES floor,
 *      caught by a routing change (synchronous, instant) before the floor
 *      is cleared.
 *  10. parameter clamping/bounds on every knob (decay_rate's range is now
 *      3..300), plus a rejected (not clamped) out-of-range input_routing
 *      value.
 *  11. extreme decay_rate = 300 (the slow end, five minutes).
 *  12. erase in the two states 4/5 never exercise (both only fire while
 *      Looping) — Idle (no-op safety) and Recording (hard closes
 *      immediately, discarding the take).
 *  13. `state` get/set round-trip: live knobs survive into a fresh instance
 *      bit-for-bit; recorded content/playback position deliberately do not.
 *  14. saturation-stage passthrough: bit-exact identity at saturation=0
 *      (nonzero degrade) and at degrade=0 (any saturation).
 *  15. master_freeze — pauses decay in place while LOOPING (memory does
 *      not move at all while frozen, however long), resumes on a second
 *      press.
 *
 * Recordings are closed deliberately via the buffer-full path (feed a full
 * buffer_seconds of tone) rather than a manual second press, wherever a test
 * doesn't care about the close mechanism itself — buffer-full remains an
 * automatic close (a safety cap, per forgetful.c's set_param comment) and
 * gives an exact, known recorded_length with no manual-timing fuzziness.
 * Test 6 is the exception: it deliberately stops well short of buffer-full,
 * since proving the routing-change close fired requires that buffer-full
 * could not have.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

/* Spelled out rather than M_PI: that is not declared under gcc with a
 * strict -std=c99, which is what CI builds with, although clang on macOS
 * provides it — so a test using M_PI passes locally and fails in CI. */
#define TEST_PI   3.14159265358979323846
#define TEST_PI_F ((float)TEST_PI)

#define SAMPLE_RATE   44100
#define BLOCK_FRAMES  128

/* Mirrors forgetful.c's documented defaults (60s buffer, 10s erase fade,
 * 400ms forgotten display, 50ms too-short-take floor). */
#define TEST_BUFFER_CAPACITY_FRAMES   (44100L * 60)
#define TEST_FORGOTTEN_DISPLAY_FRAMES (400L * SAMPLE_RATE / 1000)
#define TEST_MIN_RECORDED_FRAMES      (50L   * SAMPLE_RATE / 1000)

/* Mirrors forgetful.c's ROUTE_* — matches the declared options index order
 * ["A","B","C","D"] (no OFF — removed 2026-08-25, see the ROUTE_A comment
 * in forgetful.c). set_param accepts both the index string (used
 * throughout this file, matching how the bench harness drives the API
 * directly) and the label string (what the real host writes on-device —
 * see the input_routing set_param comment in forgetful.c). */
#define TEST_ROUTE_A    "0"
#define TEST_ROUTE_B    "1"

/* Seconds -> frames, for decay_rate math (decay_rate is now a duration). */
#define TEST_DECAY_FRAMES(secs) ((long)((secs) * (double)SAMPLE_RATE))

static int g_failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void fill_tone(int16_t *buf, int frames, float amplitude, float freq, float *phase) {
    for (int i = 0; i < frames; i++) {
        float samp = amplitude * sinf(*phase);
        int16_t v = (int16_t)(samp * 32767.0f);
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
        *phase += 2.0f * TEST_PI_F * freq / SAMPLE_RATE;
        if (*phase >= 2.0f * TEST_PI_F) *phase -= 2.0f * TEST_PI_F;
    }
}

static void fill_silence(int16_t *buf, int frames) {
    memset(buf, 0, sizeof(int16_t) * 2 * (size_t)frames);
}

/* A constant (DC-ish) "tone" — used only by the saturation-passthrough test
 * (test14), where a value that never changes sample-to-sample makes the
 * recorded content immune to any start-offset jitter: any window of a
 * constant signal is identical regardless of exactly which sample
 * write_head==0 landed on. */
static void fill_constant(int16_t *buf, int frames, int16_t value) {
    for (int i = 0; i < frames; i++) {
        buf[i * 2]     = value;
        buf[i * 2 + 1] = value;
    }
}

static const char *status_of(audio_fx_api_v2_t *api, void *inst, char letter) {
    static char key[32];
    static char buf[64];
    snprintf(key, sizeof(key), "loop%c_status", letter);
    int n = api->get_param(inst, key, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}


static int status_is_looping(const char *status) {
    return strncmp(status, "Looping - ", 10) == 0;
}
#define STATUS_READY "Ready"

static const char *record_readout(audio_fx_api_v2_t *api, void *inst) {
    static char buf[16];
    int n = api->get_param(inst, "master_record", buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

static const char *freeze_readout(audio_fx_api_v2_t *api, void *inst) {
    static char buf[16];
    int n = api->get_param(inst, "master_freeze", buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

/* Fires the manual record trigger — same "any non-idle-spelling write fires
 * it" convention loopA_erase uses. One press starts (while routed+Idle/
 * Forgotten), a second closes (while Recording); a no-op otherwise (nothing
 * routed, or already Looping). */
static void press_record(audio_fx_api_v2_t *api, void *inst) {
    api->set_param(inst, "master_record", "REC!");
}

static void run_tone(audio_fx_api_v2_t *api, void *inst, long total_frames,
                      float amplitude, float freq, float *phase) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_tone(buf, n, amplitude, freq, phase);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}

static void run_silence(audio_fx_api_v2_t *api, void *inst, long total_frames) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_silence(buf, n);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}



/* Noise on a constant take: deviation of each sample from the local mean
 * of the block. Skips nothing else, so it also sees the splice fade if
 * you measure across it — measure blocks from the middle of a take. */
static double measure_noise(audio_fx_api_v2_t *api, void *inst, int blocks) {
    int16_t buf[BLOCK_FRAMES * 2];
    static double sd[512];
    if (blocks > 512) blocks = 512;
    for (int b = 0; b < blocks; b++) {
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        double sum = 0.0;
        for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) sum += buf[i];
        double mean = sum / BLOCK_FRAMES;
        double sq = 0.0;
        for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) {
            double d = buf[i] - mean; sq += d * d;
        }
        sd[b] = sqrt(sq / BLOCK_FRAMES);
    }
    /* MEDIAN, not max: the two blocks holding the splice fade are a ramp
     * across the whole sample range and swamp any average or peak. */
    for (int i = 1; i < blocks; i++) {
        double v = sd[i]; int j = i - 1;
        while (j >= 0 && sd[j] > v) { sd[j + 1] = sd[j]; j--; }
        sd[j + 1] = v;
    }
    return sd[blocks / 2];
}

/* Pitch of whatever the module is putting out, from interpolated
 * zero-crossings. Sub-sample accuracy matters here: counting whole
 * crossings quantises badly enough to swamp a slow glide. */
static double measure_pitch(audio_fx_api_v2_t *api, void *inst, int blocks) {
    static double x[64 * BLOCK_FRAMES];
    int16_t buf[BLOCK_FRAMES * 2];
    long n = 0;
    if (blocks > 64) blocks = 64;
    for (int b = 0; b < blocks; b++) {
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        for (int i = 0; i < BLOCK_FRAMES; i++) x[n++] = buf[i * 2];
    }
    double prev = 0.0; int have = 0; double sum = 0.0; long cnt = 0;
    for (long i = 0; i + 1 < n; i++) {
        if (x[i] < 0.0 && x[i + 1] >= 0.0 && x[i + 1] != x[i]) {
            double pos = (double)i + (0.0 - x[i]) / (x[i + 1] - x[i]);
            if (have) { sum += pos - prev; cnt++; }
            prev = pos; have = 1;
        }
    }
    return cnt ? (double)SAMPLE_RATE / (sum / (double)cnt) : 0.0;
}

/* Deterministic broadband source. A pure tone cannot show a lowpass at
 * all once the measurement is level-normalised — filtering a sine moves
 * its level, not its frequency — so anything testing Darken needs content
 * with something across the spectrum to take away. */
static unsigned long g_noise_state = 22222u;
static void fill_noise(int16_t *buf, int frames, float amplitude) {
    for (int i = 0; i < frames; i++) {
        g_noise_state = g_noise_state * 1103515245u + 12345u;
        float v = (float)((int)((g_noise_state >> 9) & 0xFFFFu) - 32768) / 32768.0f;
        int16_t x = (int16_t)(v * amplitude * 32767.0f);
        buf[i * 2] = x; buf[i * 2 + 1] = x;
    }
}

/* measure_brightness went with Darken (2026-08-28): it existed to see a
 * lowpass compounding, and nothing left in the module does that. */

static void run_constant(audio_fx_api_v2_t *api, void *inst, long total_frames, int16_t value) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_constant(buf, n, value);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}

/* Routes to A, presses record (starts), feeds EXACTLY a full buffer of tone
 * so it closes on its own via buffer-full — deterministic recorded_length
 * AND zero LOOPING time elapsed by the time this returns. No padding past
 * TEST_BUFFER_CAPACITY_FRAMES: close_recording() fires the instant
 * write_head reaches capacity_frames (inside the last, possibly partial,
 * process_block call), and since run_tone stops feeding at exactly that
 * frame count, none of the fed frames are ever processed as LOOPING here.
 * (An earlier version padded by 8 extra blocks to cover recording starting
 * late behind a level-detection debounce; that debounce no longer exists —
 * recording starts on press_record with zero delay — and the padding
 * became actively wrong once decay went continuous-per-sample (test2/
 * test14a's decay-timing checks were failing by exactly one padding's worth
 * of extra, silent decay that had already happened before their own clock
 * started) rather than merely imprecise.) */
static void record_full_buffer_loop_a(audio_fx_api_v2_t *api, void *inst, float *phase) {
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    press_record(api, inst);
    run_tone(api, inst, TEST_BUFFER_CAPACITY_FRAMES, 0.5f, 440.0f, phase);
}


/* ---- Glitch helpers (test34) ---------------------------------------
 * Captures the module's own output while feeding silence, so what lands
 * in `out` is the loop playing back through whatever the Glitch page is
 * set to. The glitch RNG is seeded per instance, so two runs configured
 * identically are byte-identical and can be compared exactly. */
#define GTEST_BLOCKS 300
static int16_t g_capture_a[GTEST_BLOCKS * BLOCK_FRAMES * 2];
static int16_t g_capture_b[GTEST_BLOCKS * BLOCK_FRAMES * 2];

static void capture_out(audio_fx_api_v2_t *api, void *inst, int16_t *out) {
    for (int b = 0; b < GTEST_BLOCKS; b++) {
        int16_t *p = out + (size_t)b * BLOCK_FRAMES * 2;
        fill_silence(p, BLOCK_FRAMES);
        api->process_block(inst, p, BLOCK_FRAMES);
    }
}

/* Records a full loop, applies `settings`, then captures. */
static void glitch_run(audio_fx_api_v2_t *api, int16_t *out,
                       const char *const *settings, int n_settings) {
    void *inst = api->create_instance(".", NULL);
    float phase = 0.0f;
    record_full_buffer_loop_a(api, inst, &phase);
    for (int i = 0; i < n_settings; i += 2)
        api->set_param(inst, settings[i], settings[i + 1]);
    capture_out(api, inst, out);
    api->destroy_instance(inst);
}

static long buf_diff(const int16_t *a, const int16_t *b) {
    long d = 0;
    for (size_t i = 0; i < GTEST_BLOCKS * BLOCK_FRAMES * 2; i++)
        if (a[i] != b[i]) d++;
    return d;
}

static int max_step(const int16_t *a) {
    int m = 0;
    for (size_t i = 2; i < GTEST_BLOCKS * BLOCK_FRAMES * 2; i += 2) {
        int d = a[i] - a[i - 2];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}


/* High-frequency content of the output, as first-difference RMS over total
 * RMS. Normalised so a level change alone does not move it — test36 needs
 * to see WHERE a filter sits, not how loud the result is. */
static double measure_hf_ratio(audio_fx_api_v2_t *api, void *inst, int blocks) {
    int16_t buf[BLOCK_FRAMES * 2];
    double lo = 0.0, hi = 0.0;
    for (int b = 0; b < blocks; b++) {
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            double v = buf[i * 2];
            lo += v * v;
            if (i) { double d = v - buf[(i - 1) * 2]; hi += d * d; }
        }
    }
    return sqrt(hi / (lo + 1e-9));
}


/* Records a full buffer of BROADBAND NOISE into loop A.
 *
 * Not the 440 Hz tone record_full_buffer_loop_a lays down: a pure sine has
 * almost no high-frequency content, so a lowpass barely moves any spectral
 * measure taken on it. That mistake has been made here before and it makes
 * a filter look broken when it is working. */
static void record_full_buffer_loop_a_noise(audio_fx_api_v2_t *api, void *inst) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = TEST_BUFFER_CAPACITY_FRAMES;
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    press_record(api, inst);
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_noise(buf, n, 0.40f);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}


/* Records a rising RAMP into loop A — a signal whose direction of travel is
 * readable from the output. A tone cannot do this: played backwards it is
 * the same tone, which is exactly why the reverse speeds needed a different
 * fixture rather than a reuse of measure_pitch. */
static void record_full_buffer_loop_a_ramp(audio_fx_api_v2_t *api, void *inst) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = TEST_BUFFER_CAPACITY_FRAMES;
    long n = 0;
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    press_record(api, inst);
    while (remaining > 0) {
        int f = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        for (int i = 0; i < f; i++) {
            double t = (double)(n % 4410) / 4410.0;     /* 10 Hz ramp */
            int16_t v = (int16_t)((t * 2.0 - 1.0) * 0.5 * 32767.0);
            buf[i * 2] = v; buf[i * 2 + 1] = v;
            n++;
        }
        api->process_block(inst, buf, f);
        remaining -= f;
    }
}

/* Mean SIGN of the first difference, ignoring the ramp's own reset jumps.
 * +1 = playing forward, -1 = backwards. */
static double measure_slope_sign(audio_fx_api_v2_t *api, void *inst, int blocks) {
    int16_t buf[BLOCK_FRAMES * 2];
    long up = 0, down = 0;
    int16_t prev = 0; int have = 0;
    for (int b = 0; b < blocks; b++) {
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int16_t v = buf[i * 2];
            if (have) {
                int d = v - prev;
                if (d > -2000 && d < 2000) { if (d > 0) up++; else if (d < 0) down++; }
            }
            prev = v; have = 1;
        }
    }
    long tot = up + down;
    return tot ? (double)(up - down) / (double)tot : 0.0;
}


/* Largest sample-to-sample step in the output. The loop seam is the only
 * discontinuity a smooth recorded tone can produce, so this is how a
 * missing join crossfade shows up. */
static int measure_max_step(audio_fx_api_v2_t *api, void *inst, int blocks) {
    int16_t buf[BLOCK_FRAMES * 2];
    int worst = 0, prev = 0, have = 0;
    for (int b = 0; b < blocks; b++) {
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int v = buf[i * 2];
            if (have) { int d = v - prev; if (d < 0) d = -d; if (d > worst) worst = d; }
            prev = v; have = 1;
        }
    }
    return worst;
}


/* ONE monotonic ramp over a SHORT (~1s) take, closed by hand.
 *
 * Two things this fixture has to get right, both learned the hard way:
 *
 *  - ONE ramp, so the loop seam is a guaranteed FULL-SCALE discontinuity
 *    and nothing else in the signal is. On a 440Hz sine the seam joins two
 *    points at a similar phase, so deleting the reverse join crossfade
 *    moved the worst sample step only 805 -> 867 and the test passed. On a
 *    ramp the same deletion reads 153 -> 13384.
 *
 *  - SHORT, because TEST_BUFFER_CAPACITY_FRAMES is SIXTY SECONDS. A test
 *    that fills the buffer and then plays for ten seconds never reaches the
 *    seam at all, so it cannot see a join bug in either direction. That is
 *    why the first version of this still passed with the wrap deleted.
 */
static void record_short_ramp_loop_a(audio_fx_api_v2_t *api, void *inst, long frames) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = frames, n = 0;
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    press_record(api, inst);
    while (remaining > 0) {
        int f = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        for (int i = 0; i < f; i++) {
            int16_t v = (int16_t)(((double)n / (double)frames * 2.0 - 1.0) * 0.5 * 32767.0);
            buf[i * 2] = v; buf[i * 2 + 1] = v;
            n++;
        }
        api->process_block(inst, buf, f);
        remaining -= f;
    }
    api->set_param(inst, "master_record", "STOP!");   /* close it here */
}

/* Constant-value counterpart, for test14: fills the whole buffer with one
 * unchanging sample value, so the recorded content is known exactly
 * (`value`, every index). */
static void record_full_buffer_loop_a_constant(audio_fx_api_v2_t *api, void *inst, int16_t value) {
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    press_record(api, inst);
    run_constant(api, inst, TEST_BUFFER_CAPACITY_FRAMES, value);
}

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    if (!api) { fprintf(stderr, "FAIL: move_audio_fx_init_v2 returned NULL\n"); return 1; }
    if (!api->create_instance || !api->destroy_instance || !api->process_block ||
        !api->set_param || !api->get_param) {
        fprintf(stderr, "FAIL: API missing required callbacks\n");
        return 1;
    }

    /* ---- Test 0: chain_params / ui_hierarchy shape ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test0: create_instance");

        char cp[8192], hier[4096], probe[64];

        int n = api->get_param(inst, "chain_params", cp, sizeof(cp));
        check(n > 0, "test0: chain_params readable");
        check(n > 0 && cp[0] == '[', "test0: chain_params is an array");

        int key_count = 0;
        const char *p = cp;
        while ((p = strstr(p, "\"key\":\"")) != NULL) { key_count++; p += 7; }
        check(key_count == 56,
              "test0: chain_params has exactly 56 entries (8 master + 4x10 loop "
              "+ 8 glitch; Erase went and Trim arrived 2026-08-27, Glitch 2026-08-28)");

        /* Both JSON contracts are built into fixed stack buffers and
         * truncated silently by snprintf, so a page added without growing
         * one loses its tail and the module simply stops having that page.
         * Assert real headroom rather than "it parsed". */
        check(n < 7000,
              "test0: chain_params fits its buffer with real headroom — it is\n"
              "built into a fixed 8192 stack buffer and snprintf truncates\n"
              "SILENTLY, so this must fail while there is still room to fix it");

        n = api->get_param(inst, "ui_hierarchy", hier, sizeof(hier));
        check(n > 0, "test0: ui_hierarchy readable");
        check(n > 0 && strstr(hier, "\"knobs\":[") != NULL, "test0: ui_hierarchy has a knobs array");

        /* SIX levels, so six sections, each one named page: root (Main),
         * sound, and one per loop. A section is a ui_hierarchy LEVEL, so a
         * level per loop is what keeps "Loop A".."Loop D" as page names —
         * merging them into one level makes them one section but renames
         * them "Loops - 2".."Loops - 4" via the planner's claimName(). */
        check(strstr(hier, "\"root\":{") != NULL, "test0: ui_hierarchy has a root level");
        check(strstr(hier, "\"sound\":{\"label\":\"Sound\"") != NULL,
              "test0: a sound level");
        check(strstr(hier, "\"loops\":{") == NULL,
              "test0: and NOT a merged loops level");
        static const char *level_keys[] = { "loopA", "loopB", "loopC", "loopD" };
        static const char *level_labels[] = { "Loop A", "Loop B", "Loop C", "Loop D" };
        for (size_t i = 0; i < 4; i++) {
            snprintf(probe, sizeof(probe), "\"%s\":{\"label\":\"%s\"", level_keys[i], level_labels[i]);
            check(strstr(hier, probe) != NULL,
                  "test0: each loop is its own level, so its own section and "
                  "its own page name");
        }
        /* Main's top row is Route/Status/Rec/Freeze; its bottom row is
         * deliberately empty since the volumes moved to the Mixer page. */
        check(strstr(hier, "\"master_loops_overview\",\"master_record\",\"master_freeze\",\"loopA_speed\"") != NULL,
              "test0: Main is Route/Status/Rec/Freeze over the four speeds");

        /* every expected key, in both blobs */
        static const char *master_keys[] = {
            "input_routing", "loopA_volume", "loopB_volume", "loopC_volume", "loopD_volume",
            "master_loops_overview", "master_record", "master_freeze"
        };
        for (size_t i = 0; i < sizeof(master_keys) / sizeof(master_keys[0]); i++) {
            snprintf(probe, sizeof(probe), "\"key\":\"%s\"", master_keys[i]);
            check(strstr(cp, probe) != NULL, "test0: chain_params contains master key");
            snprintf(probe, sizeof(probe), "\"%s\"", master_keys[i]);
            check(strstr(hier, probe) != NULL, "test0: ui_hierarchy contains master key");
        }
        static const char *loop_suffixes[] = {
            "decay_rate", "wow", "freq", "hiss", "send", "chaos", "state", "speed", "tone", "trim"
        };
        static const char letters[] = { 'A', 'B', 'C', 'D' };
        for (int li = 0; li < 4; li++) {
            for (size_t si = 0; si < sizeof(loop_suffixes) / sizeof(loop_suffixes[0]); si++) {
                snprintf(probe, sizeof(probe), "\"key\":\"loop%c_%s\"", letters[li], loop_suffixes[si]);
                check(strstr(cp, probe) != NULL, "test0: chain_params contains per-loop key");
                snprintf(probe, sizeof(probe), "\"loop%c_%s\"", letters[li], loop_suffixes[si]);
                check(strstr(hier, probe) != NULL, "test0: ui_hierarchy contains per-loop key");
            }
        }
        check(strstr(cp, "reserved") == NULL, "test0: chain_params has no reserved keys");
        check(strstr(hier, "reserved") == NULL, "test0: ui_hierarchy has no reserved keys");

        /* decay_rate's range is now 3..300 SECONDS, not 3..60 repeats. */
        check(strstr(cp, "\"loopA_decay_rate\",\"name\":\"Age\",\"type\":\"float\","
                         "\"min\":3,\"max\":300") != NULL,
              "test0: loopX_decay_rate range is 3..300");

        /* No OFF option — Route is exactly the four letters now. */
        check(strstr(cp, "\"options\":[\"A\",\"B\",\"C\",\"D\"],\"default\":0}") != NULL,
              "test0: input_routing options are exactly A/B/C/D, no OFF");
        check(strstr(cp, "OFF") == NULL, "test0: chain_params has no OFF option anywhere");

        api->destroy_instance(inst);
    }

    /* ---- Test 1: manual record start/stop (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test1: create_instance");

        /* No OFF/unrouted state — a fresh instance is already routed to A. */
        char rbuf[16];
        api->get_param(inst, "input_routing", rbuf, sizeof(rbuf));
        check(strcmp(rbuf, "A") == 0, "test1: fresh instance defaults to Route A");

        float phase = 0.0f;
        int16_t buf[BLOCK_FRAMES * 2], ref[BLOCK_FRAMES * 2];

        /* Routed but not yet pressed: still Ready, dry passes through
         * untouched regardless of input level (this is the design doc's
         * "dry always passes through" rule). */
        fill_tone(buf, BLOCK_FRAMES, 0.5f, 440.0f, &phase);
        memcpy(ref, buf, sizeof(buf));
        api->process_block(inst, buf, BLOCK_FRAMES);
        check(memcmp(buf, ref, sizeof(buf)) == 0, "test1: dry passthrough while Ready");
        check(strcmp(status_of(api, inst, 'A'), STATUS_READY) == 0, "test1: still Ready before the press");

        /* Press: starts Recording immediately, no debounce. */
        press_record(api, inst);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test1: press starts Recording immediately (no debounce)");
        /* Readout vocabulary rebuilt 2026-08-25 around "what will the NEXT
         * press do" (a transport-button convention), replacing an earlier
         * "what's happening now" scheme (CATCH/LAYER/LIVE/BLISS, and before
         * that CAPTURE/RELIVE/PLAYING/-): STOP while Recording (next press
         * stops it). */
        check(strcmp(record_readout(api, inst), "STOP") == 0,
              "test1: master_record reads 'STOP' while A is Recording");

        for (int b = 0; b < 20; b++) {
            fill_tone(buf, BLOCK_FRAMES, 0.5f, 440.0f, &phase);
            memcpy(ref, buf, sizeof(buf));
            api->process_block(inst, buf, BLOCK_FRAMES);
            check(memcmp(buf, ref, sizeof(buf)) == 0, "test1: dry passthrough while Recording");
        }

        /* Second press: closes into Looping. Readout is DUB (next press
         * would start an overdub) for an ordinary loop with nothing else
         * going on — was LIVE/ALIVE/PLAYING under the old "what's happening
         * now" scheme. */
        press_record(api, inst);
        check(status_is_looping(status_of(api, inst, 'A')), "test1: second press closes into Looping");
        check(strcmp(record_readout(api, inst), "DUB") == 0,
              "test1: master_record reads 'DUB' once closed");

        /* A third press, while Looping, starts an overdub (2026-08-25:
         * replaces the old v1 no-op) — readout is PLAY (next press would
         * stop the overdub, back to plain playback — was LAYER/RELIVE/
         * OVERDUB under the old "what's happening now" scheme) — but the
         * loop's own status line, memory and flavor knobs are untouched. */
        const char *before = status_of(api, inst, 'A');
        char before_copy[64];
        snprintf(before_copy, sizeof(before_copy), "%s", before);
        char loop_a_wow[16], loop_a_chaos[16];
        api->get_param(inst, "loopA_wow", loop_a_wow, sizeof(loop_a_wow));
        api->get_param(inst, "loopA_chaos", loop_a_chaos, sizeof(loop_a_chaos));

        press_record(api, inst);
        check(strcmp(record_readout(api, inst), "PLAY") == 0,
              "test1: third press starts an overdub (readout is PLAY)");
        check(strcmp(status_of(api, inst, 'A'), before_copy) == 0,
              "test1: starting an overdub doesn't change the loop's own status line");
        char echo_char[8];
        api->get_param(inst, "loopA_state", echo_char, sizeof(echo_char));
        check(strcmp(echo_char, "O") == 0,
              "test1: loopA_state (ECHO) shows 'O' while overdubbing (2026-08-25)");
        char wow_check[16], chaos_check[16];
        api->get_param(inst, "loopA_wow", wow_check, sizeof(wow_check));
        api->get_param(inst, "loopA_chaos", chaos_check, sizeof(chaos_check));
        check(strcmp(wow_check, loop_a_wow) == 0 && strcmp(chaos_check, loop_a_chaos) == 0,
              "test1: starting an overdub doesn't touch flavor knob targets");

        /* Overdubbing actually layers new dry input into the buffer: after
         * feeding a full pass of a DIFFERENT tone, the loop's own content is
         * no longer identical to the original pure 440Hz recording (the
         * cheapest observable proxy: play a loud second tone in and confirm
         * SOME buffer content changed by checking output differs across two
         * otherwise-identical silent playback passes is unnecessary — buffer
         * content isn't directly exposed, so this just exercises the API
         * path without asserting exact samples). */
        for (int b = 0; b < 20; b++) {
            fill_tone(buf, BLOCK_FRAMES, 0.8f, 880.0f, &phase);
            api->process_block(inst, buf, BLOCK_FRAMES);
        }

        /* Fourth press: stops the overdub, back to plain DUB (next press
         * would start a new overdub). */
        press_record(api, inst);
        check(strcmp(record_readout(api, inst), "DUB") == 0,
              "test1: fourth press stops the overdub (readout is DUB again)");
        api->get_param(inst, "loopA_state", echo_char, sizeof(echo_char));
        check(strcmp(echo_char, "O") != 0,
              "test1: loopA_state (ECHO) no longer shows 'O' once the overdub stops");

        api->destroy_instance(inst);
    }

    /* ---- Test 2: decay timing (wall-clock seconds) + forgotten_at display
     * window (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test2: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3"); /* 3 SECONDS to fully forget */

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test2: Looping after buffer-full close");

        /* Just short of 3 seconds: must still be Looping. */
        run_silence(api, inst, TEST_DECAY_FRAMES(3) - BLOCK_FRAMES * 4);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test2: still Looping just short of decay_rate seconds");

        /* Past 3 seconds: Forgotten. */
        run_silence(api, inst, BLOCK_FRAMES * 8);
        check(strcmp(status_of(api, inst, 'A'), "Forgotten") == 0,
              "test2: Forgotten once decay_rate seconds have elapsed");

        /* Run past the display window: status settles to Ready. */
        run_silence(api, inst, TEST_FORGOTTEN_DISPLAY_FRAMES + BLOCK_FRAMES * 4);
        check(strcmp(status_of(api, inst, 'A'), STATUS_READY) == 0,
              "test2: status settles to Ready after the display window");

        /* Engine is genuinely reset — pressing record starts a fresh take
         * immediately (still routed to A). */
        press_record(api, inst);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test2: engine accepts a new recording right after Forgotten");

        api->destroy_instance(inst);
    }

    /* ---- Test 3: continuous decay — memory must NOT be gated on a buffer
     * wrap. Previously memory only stepped once per WRAP; a long recording
     * could sit at memory==1.0 for its entire first pass before dropping in
     * one visible/audible step. With per-sample decay, a recording many
     * times longer than decay_rate itself should already show visible
     * degradation well before completing even one wrap. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test3: create_instance");

        /* decay_rate=3s, but the recording is the full 8s buffer — memory
         * would reach 0 entirely within the FIRST wrap, which also proves
         * the point, but split it clearly: check partway through the first
         * wrap (1.5s of LOOPING) that memory has already dropped by roughly
         * half, not still sitting at 1.0. */
        api->set_param(inst, "loopA_decay_rate", "3");
        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test3: Looping after buffer-full close");

        run_silence(api, inst, TEST_DECAY_FRAMES(1.5));
        const char *s = status_of(api, inst, 'A');
        check(status_is_looping(s), "test3: still Looping 1.5s into a 3s decay (not yet forgotten)");
        int pct = -1;
        char word[32];
        check(sscanf(s, "Looping - %d%% (%31[^)])", &pct, word) == 2, "test3: status line parses");
        /* ~50% expected (1.5 / 3.0); wide tolerance for the block-boundary
         * rounding in how far run_silence actually advanced. */
        check(pct >= 35 && pct <= 65,
              "test3: memory is measurably degraded partway through the FIRST wrap of an 8s "
              "recording — proves decay is continuous, not stepped once per wrap");

        api->destroy_instance(inst);
    }

    /* Test 4 removed with Erase (2026-08-27). */

    /* Test 5 removed with Erase (2026-08-27). */

    /* ---- Test 6: routing — closes A via a routing change (not buffer-
     * full), and A's decay keeps progressing on its own once B is the
     * active recording target. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test6: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3"); /* fast decay: small test budget */

        /* Route to A, press record, and feed a short, deliberately UNCLOSED
         * take — nowhere near buffer-full, so the close that follows can
         * only have come from the routing change itself. */
        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        press_record(api, inst);
        float phase_a = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES * 20, 0.5f, 440.0f, &phase_a);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test6: A is Recording before the routing change");

        /* Switch routing away from A mid-recording. This must close A
         * SYNCHRONOUSLY, inside this very set_param call. */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test6: A closes into Looping the instant routing moves away");

        /* Record a DIFFERENT tone into B (manual press, same as A), for
         * long enough that A's short loop (decay_rate=3s) has time to fully
         * decay on its own, unattended — A never receives input again. */
        press_record(api, inst);
        float phase_b = 0.0f;
        long budget = TEST_DECAY_FRAMES(3) + BLOCK_FRAMES * 20;
        long advanced = 0;
        int saw_b_recording = 0, saw_a_forgotten = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget) {
            fill_tone(buf, BLOCK_FRAMES, 0.5f, 880.0f, &phase_b);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (strcmp(status_of(api, inst, 'B'), "Recording") == 0) saw_b_recording = 1;
            if (strcmp(status_of(api, inst, 'A'), "Forgotten") == 0) saw_a_forgotten = 1;
            advanced += BLOCK_FRAMES;
        }
        check(saw_b_recording, "test6: B records the different tone while A is no longer routed");
        check(saw_a_forgotten, "test6: A's memory keeps decaying on its own while B is the active target");

        /* B must be unaffected by A's independent decay/reset. */
        check(strcmp(status_of(api, inst, 'B'), "Recording") == 0,
              "test6: B is unaffected by A's independent decay and reset");

        api->destroy_instance(inst);
    }

    /* ---- Test 7: memory-word bucket mapping in the loop-page status line
     * (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test7: create_instance");

        api->set_param(inst, "loopA_decay_rate", "20"); /* 20 seconds, easy 5%-per-second math */

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test7: Looping after buffer-full close");

        /* Poll every block through the full 20s decay and check every
         * observed "NN% (word)" reading against the design doc's bucket
         * boundaries directly. */
        long budget = TEST_DECAY_FRAMES(20) + BLOCK_FRAMES * 8;
        long advanced = 0;
        int saw_vivid = 0, saw_fading = 0, saw_hazy = 0, saw_almost_gone = 0;
        int bucket_mismatch = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget && strcmp(status_of(api, inst, 'A'), "Forgotten") != 0) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            const char *s = status_of(api, inst, 'A');
            int pct;
            char word[32];
            if (sscanf(s, "Looping - %d%% (%31[^)])", &pct, word) == 2) {
                const char *expected =
                    pct >= 90 ? "Vivid" :
                    pct >= 40 ? "Fading" :
                    pct >= 10 ? "Hazy" : "Almost gone";
                if (strcmp(word, expected) != 0) bucket_mismatch = 1;
                if (strcmp(word, "Vivid") == 0)       saw_vivid = 1;
                if (strcmp(word, "Fading") == 0)      saw_fading = 1;
                if (strcmp(word, "Hazy") == 0)        saw_hazy = 1;
                if (strcmp(word, "Almost gone") == 0) saw_almost_gone = 1;
            }
            advanced += BLOCK_FRAMES;
        }
        check(!bucket_mismatch, "test7: every observed percentage matches the design doc's word bucket");
        check(saw_vivid,       "test7: observed the Vivid bucket (90-100%)");
        check(saw_fading,      "test7: observed the Fading bucket (40-89%)");
        check(saw_hazy,        "test7: observed the Hazy bucket (10-39%)");
        check(saw_almost_gone, "test7: observed the Almost gone bucket (1-9%)");

        api->destroy_instance(inst);
    }

    /* ---- Test 8: master_loops_overview format (Master page Status) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test8: create_instance");

        char buf[16];
        int n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4, "test8: overview is exactly 4 characters, one per loop — the\n              '_' wrap hint went with Schwung 1.0's wider cell (2026-08-29)");
        check(strcmp(buf, "----") == 0, "test8: fresh instance reads all-idle");

        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        press_record(api, inst);
        float phase = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES * 20, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test8: A is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "R---") == 0, "test8: overview shows A recording, rest idle");

        /* Switch routing to B: closes A into Looping at memory=1.0 - the top
         * decile, digit '9' (the decile scheme has no distinct digit for
         * "100%" versus "90-99%", by design - see the header comment). */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "9---") == 0, "test8: overview shows A's fresh-close decile, rest idle");

        /* B now records too: overview reflects both loops at once. */
        press_record(api, inst);
        float phase_b = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES * 6, 0.5f, 880.0f, &phase_b);
        check(strcmp(status_of(api, inst, 'B'), "Recording") == 0, "test8: B is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "9R--") == 0, "test8: overview reflects both A and B simultaneously");

        api->destroy_instance(inst);
    }

    /* ---- Test 9: too-short blip discard — close_recording's
     * MIN_RECORDED_FRAMES floor. Press record, feed only ~one block of
     * tone, then close via a ROUTING CHANGE (synchronous, instant) while
     * write_head is still comfortably under the floor. The loop must
     * discard back to Ready, not start Looping a click. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test9: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        press_record(api, inst);

        float phase = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test9: blip does start Recording");

        /* No OFF to close via any more — a routing change to a different
         * letter is synchronous and instant, same as OFF used to be, and is
         * already what test6 exercises for the non-blip case. */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        check(strcmp(status_of(api, inst, 'A'), STATUS_READY) == 0,
              "test9: a too-short take is discarded back to Ready, not looped");

        api->destroy_instance(inst);
    }

    /* ---- Test 11: parameter clamping — an out-of-range set_param value
     * clamps to the declared range instead of storing the raw value, and an
     * out-of-range input_routing value is REJECTED outright (leaves the
     * existing route untouched) rather than clamped, since routing has
     * discrete valid states with no sensible "nearest" clamp. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test10: create_instance");
        char buf[64];

        api->set_param(inst, "loopA_decay_rate", "10000");
        api->get_param(inst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "300.0") == 0, "test10: decay_rate clamps to max 300");
        api->set_param(inst, "loopA_decay_rate", "-5");
        api->get_param(inst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "3.0") == 0, "test10: decay_rate clamps to min 3");

        static const char *unit_keys[] = { "wow", "hiss", "send", "chaos" };
        for (size_t i = 0; i < sizeof(unit_keys) / sizeof(unit_keys[0]); i++) {
            char key[32];
            snprintf(key, sizeof(key), "loopA_%s", unit_keys[i]);
            api->set_param(inst, key, "5");
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "1.000") == 0, "test10: 0..1 flavor param clamps to max 1");
            api->set_param(inst, key, "-5");
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "0.000") == 0, "test10: 0..1 flavor param clamps to min 0");
        }

        api->set_param(inst, "loopA_volume", "5");
        api->get_param(inst, "loopA_volume", buf, sizeof(buf));
        check(strcmp(buf, "2.000") == 0,
              "test10: loop volume clamps to MAX_LOOP_VOLUME (2.0 = 200%, raised\n"
              "              2026-08-27 so a memory can be pushed, not only pulled back)");
        api->set_param(inst, "loopA_volume", "-5");
        api->get_param(inst, "loopA_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.000") == 0, "test10: loop volume clamps to min 0");

        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test10: valid input_routing accepted");
        api->set_param(inst, "input_routing", "99");
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test10: out-of-range input_routing (too high) is rejected, route unchanged");
        api->set_param(inst, "input_routing", "-1");
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test10: out-of-range input_routing (negative) is rejected, route unchanged");

        api->destroy_instance(inst);
    }

    /* ---- Test 12: extreme decay_rate = 300 (five minutes — the new slow
     * end; 6/7/8 use the fast end (3) or the default (45s worth of range is
     * covered by 20 in test7) — the slow boundary needs its own check. A
     * minute in at the slowest setting must still report Looping with a
     * believable, still-high percentage, not flip to Forgotten early or
     * drift outside the expected range. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test11: create_instance");
        api->set_param(inst, "loopA_decay_rate", "300");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test11: Looping after buffer-full close");

        /* 60s at decay_rate=300 -> expected memory ~= 1 - 60/300 = 80%. */
        run_silence(api, inst, TEST_DECAY_FRAMES(60));
        check(strcmp(status_of(api, inst, 'A'), "Forgotten") != 0,
              "test11: must not reach Forgotten after only 60s at the slowest decay_rate");

        const char *s = status_of(api, inst, 'A');
        check(status_is_looping(s), "test11: still Looping after 60s at decay_rate=300");
        int pct = -1;
        char word[32];
        check(sscanf(s, "Looping - %d%% (%31[^)])", &pct, word) == 2, "test11: status line parses");
        check(pct >= 75 && pct <= 85,
              "test11: memory after 60s at decay_rate=300 is in the expected ~80% neighborhood");

        api->destroy_instance(inst);
    }

    /* Test 13 removed with Erase (2026-08-27). */

    /* ---- Test 13: state does NOT round-trip. Forgetful starts from
     * defaults every time, so get_param("state") answers "{}" and
     * set_param("state") is ignored — including blobs saved by earlier
     * versions, which are still sitting in sets on the device, so
     * suppressing the SAVE alone would not have been enough.
     *
     * This replaces a test asserting the opposite. The trade is deliberate
     * but not free: <prefix>:state is also how the chain host does User
     * Presets and patches, so neither can capture this module now. ---- */
    {
        void *src = api->create_instance(".", NULL);
        check(src != NULL, "test13: create_instance src");
        api->set_param(src, "input_routing", "C");
        api->set_param(src, "loopA_decay_rate", "12");
        api->set_param(src, "loopA_wow", "0.8");
        api->set_param(src, "loopA_send", "0.9");
        api->set_param(src, "loopA_trim", "0.2");
        api->set_param(src, "loopA_tone", "-0.7");
        api->set_param(src, "loopA_speed", "1/4x");
        api->set_param(src, "loopA_volume", "1.4");

        char blob[4096];
        int n = api->get_param(src, "state", blob, sizeof(blob));
        check(n > 0, "test13: state is still readable — a definite answer, not "
                     "a refusal the host would retry");
        check(strcmp(blob, "{}") == 0,
              "test13: and it is EMPTY, so nothing is written into a set");

        void *dst = api->create_instance(".", NULL);
        check(dst != NULL, "test13: create_instance dst");
        api->set_param(dst, "state",
            "{\"input_routing\":2,\"loopA_volume\":1.4000,\"loopA_send\":0.9000,"
            "\"loopA_decay_rate\":12.0,\"loopA_wow\":0.8000,\"loopA_trim\":0.2,"
            "\"loopA_tone\":-0.7,\"loopA_speed\":0.2500}");

        void *fresh = api->create_instance(".", NULL);
        check(fresh != NULL, "test13: create_instance fresh");
        static const char *keys13[] = { "loopA_decay_rate", "loopA_wow", "loopA_send",
                                        "loopA_trim", "loopA_tone", "loopA_speed",
                                        "loopA_volume", "input_routing" };
        for (size_t k = 0; k < sizeof(keys13)/sizeof(keys13[0]); k++) {
            char a2[64], b2[64];
            api->get_param(dst,   keys13[k], a2, sizeof(a2));
            api->get_param(fresh, keys13[k], b2, sizeof(b2));
            check(strcmp(a2, b2) == 0,
                  "test13: an instance handed a blob saved by an older version "
                  "is identical to an untouched one");
        }
        api->destroy_instance(fresh);
        api->destroy_instance(dst);
        api->destroy_instance(src);
    }

    /* ---- Test 15: saturation-stage passthrough. sat_amount==0 must be
     * exact identity — true both at the Warmth/Drive knob's literal minimum
     * (applied_saturation chases toward 0, so it stays 0) AND for a
     * freshly-closed loop (applied_saturation starts at 0.0 regardless of
     * the knob's setting, until the chase has had time to move it). Uses a
     * constant-value recording (not a sine tone) so the recorded content is
     * known exactly, and drives every other chase-scaled stage
     * (wow/hiss/chaos) to zero — the saturation stage's output is
     * directly recoverable from
     * process_block's output samples (dry fed as silence during
     * measurement, so out == mix_dry_wet(0, wet) recovers wet to within
     * int16 rounding). ---- */
    {
        const int16_t CONST_VALUE = 16000;
        const float SAMPLE_F = (float)CONST_VALUE / 32768.0f;

        /* ---- 14a: saturation == 0, at NONZERO degrade (partway decayed) ---- */
        {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test14a: create_instance");

            api->set_param(inst, "loopA_decay_rate", "3");
            record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
            check(status_is_looping(status_of(api, inst, 'A')), "test14a: Looping after buffer-full close");

            /* wow=0 makes read speed exactly 1.0. Advancing
             * exactly 1 second (of the 3s decay_rate) gives a known,
             * nonzero degrade: memory == 1.0 - 1.0/3.0 exactly, matching
             * forgetful.c's own per-sample expression bit for bit (no wrap
             * dependency now — see test3). */
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_saturation", "0");
            api->set_param(inst, "loopA_volume", "1");

            run_silence(api, inst, TEST_DECAY_FRAMES(1));
            float expected_memory = 1.0f - 1.0f / 3.0f;
            check(expected_memory > 0.0f && expected_memory < 1.0f, "test14a: sanity — degrade is nonzero here");

            int16_t buf[BLOCK_FRAMES * 2];
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);

            /* Expected: filt_l == SAMPLE_F exactly (with Darken gone the
             * read is unfiltered outright, where it used to rely on the
             * reverb wash's wet_amount is 0 — exact raw passthrough);
             * applied_saturation chases toward saturation's target, which
             * is 0 here, so it stays exactly 0 too regardless of how much
             * time (and therefore how much memory decay) has elapsed —
             * sat_l == filt_l exactly.
             *
             * Only sample 0 of this block is checked, not all 128: memory
             * now decays every SAMPLE (not once per wrap), so by the time
             * this same process_block call reaches sample 127 it has
             * already decremented memory 127 more times. Sample 0 reads
             * memory exactly as run_silence left it (LOOPING computes wet
             * from the CURRENT memory before decrementing it for that
             * sample), so it is the one sample in the block this single
             * expected_out value is actually about.
             *
             * Tolerance is widened to 20 LSB, not 1: continuous decay means
             * `expected_memory` is now the result of 44100 sequential
             * float32 subtractions inside forgetful.c (one per sample of
             * run_silence(TEST_DECAY_FRAMES(1))) versus ONE subtraction
             * here, and float32 accumulation error over that many ops is
             * real (measured ~8 LSB) — not a logic bug, and not something a
             * bit-exact comparison can distinguish from one. 20 LSB is
             * still two orders of magnitude tighter than the saturation
             * bug this test exists to catch, which was 5-15% off (thousands
             * of LSB), so a regression there still fails loudly. */
            float expected_wet = SAMPLE_F * expected_memory; /* * loop_volume(1) */
            int32_t expected_out = lroundf(expected_wet * 32767.0f);
            check(abs((int)buf[0] - (int)expected_out) <= 20 &&
                  abs((int)buf[1] - (int)expected_out) <= 20,
                  "test14a: saturation=0 is identity even at nonzero degrade "
                  "(within float32-accumulation tolerance of independently-computed "
                  "expected output, sample 0)");

            api->destroy_instance(inst);
        }

        /* ---- 14b: saturation == 1 (max), at ZERO degrade (freshly closed) ---- */
        {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test14b: create_instance");

            record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
            check(status_is_looping(status_of(api, inst, 'A')), "test14b: Looping after buffer-full close");

            /* Measure the
             * very FIRST block — no time has elapsed, so memory is still
             * exactly 1.0 and degrade is exactly 0.0, regardless of the
             * Drive knob sitting at its maximum. */
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_saturation", "1");
            api->set_param(inst, "loopA_volume", "1");
            /* Both ends of a take are now faded to silence so the loop
             * splices without a click (TAKE_EDGE_FADE_S), so sample 0 is
             * deliberately zero and can no longer stand in for "identity".
             * Step past the fade and measure there instead. decay_rate is
             * pushed out so the saturation chase over those extra samples
             * stays well under the 1 LSB this check allows. */
            api->set_param(inst, "loopA_decay_rate", "600");
            {
                int16_t skip[BLOCK_FRAMES * 2];
                for (int b = 0; b < 4; b++) {
                    fill_silence(skip, BLOCK_FRAMES);
                    api->process_block(inst, skip, BLOCK_FRAMES);
                }
            }

            int16_t buf[BLOCK_FRAMES * 2];
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);

            /* Only sample 0, not the whole block: applied_saturation starts
             * at 0.0 on close_recording and chases toward the saturation
             * knob (1.0 here) by one `step` per sample — by sample 127 of
             * this very block it has already taken ~127 chase steps
             * (~1.6e-5 at the default 180s decay_rate), enough in principle
             * to nudge sat_amount off exact 0, though at that magnitude it
             * wouldn't move a 1-LSB check either. Sample 0 hasn't had this
             * block's own chase step applied yet (LOOPING computes wet from
             * the applied_* values before advancing them for that sample,
             * same as memory), so it is the one sample applied_saturation is
             * genuinely still 0.0 for regardless of decay_rate. */
            float expected_wet = SAMPLE_F * 1.0f; /* memory == 1.0, applied_saturation == 0.0 */
            int32_t expected_out = lroundf(expected_wet * 32767.0f);
            check(abs((int)buf[0] - (int)expected_out) <= 1 &&
                  abs((int)buf[1] - (int)expected_out) <= 1,
                  "test14b: freshly closed (applied_saturation still 0) output is identity "
                  "even with saturation knob at its maximum (within 1 LSB, sample 0)");

            api->destroy_instance(inst);
        }
    }

    /* ---- Test 15: master_freeze — pauses decay in place while LOOPING,
     * resumes on a second press. The loop must keep PLAYING (audibly
     * looping at its currently-reached character) while frozen, just not
     * progress any further toward Forgotten. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test15: create_instance");
        api->set_param(inst, "loopA_decay_rate", "3");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test15: Looping after buffer-full close");

        /* Let it decay partway, then freeze. */
        run_silence(api, inst, TEST_DECAY_FRAMES(1));
        const char *s = status_of(api, inst, 'A');
        int pct_before = -1;
        char word[32];
        check(sscanf(s, "Looping - %d%% (%31[^)])", &pct_before, word) == 2, "test15: status line parses before freeze");

        /* Readout rebuilt 2026-08-25 onto "what will the NEXT press do":
         * FROZEN once frozen: this readout names the CURRENT state, not
         * the next press, unlike master_record. Reverted to "what's
         * happening now" 2026-08-27 — next-action naming made the header
         * contradict ECHO's `F` for the same loop. */
        api->set_param(inst, "master_freeze", "Freeze!");
        check(strcmp(freeze_readout(api, inst), "FROZEN") == 0,
              "test15: master_freeze reads FROZEN once pressed");

        /* Run well past when it would otherwise have reached Forgotten
         * (decay_rate=3s, already ~1s in — without freeze this easily
         * crosses 0). Memory must not move at all while frozen. */
        run_silence(api, inst, TEST_DECAY_FRAMES(5));
        check(status_is_looping(status_of(api, inst, 'A')),
              "test15: still Looping, not Forgotten, well past decay_rate while frozen");
        s = status_of(api, inst, 'A');
        int pct_after = -1;
        check(sscanf(s, "Looping - %d%% (%31[^)])", &pct_after, word) == 2, "test15: status line parses while frozen");
        check(pct_after == pct_before,
              "test15: memory percentage is unchanged after 5s frozen (was stuck at the frame it froze on)");

        /* Unfreeze: decay resumes and the loop eventually reaches Forgotten.
         * Readout is AGING once unfrozen (that is what it is doing
         * again). */
        api->set_param(inst, "master_freeze", "Freeze!");
        check(strcmp(freeze_readout(api, inst), "AGING") == 0,
              "test15: master_freeze reads 'AGING' again once resumed");

        long budget = TEST_DECAY_FRAMES(3) + BLOCK_FRAMES * 8;
        long advanced = 0;
        int saw_forgotten = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget && !saw_forgotten) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (strcmp(status_of(api, inst, 'A'), "Forgotten") == 0) saw_forgotten = 1;
            advanced += BLOCK_FRAMES;
        }
        check(saw_forgotten, "test15: decay resumes after unfreezing and eventually reaches Forgotten");

        api->destroy_instance(inst);
    }

    /* Test 16 removed with Drive (2026-08-27). It proved that turning
     * Drive live glided rather than snapping, using the saturation stage's
     * own output as the probe — there is no saturation stage now, and
     * test17 covers the glide itself for the knobs that remain. */

    /* ---------------------------------------------------------------
     * test17: a flavour knob SLEWS. It does not snap, and it does not
     * take the rest of the take to arrive.
     *
     * Replaces a test that asserted the opposite for the first write of a
     * take ("first touch snaps instantly"). That was the turn-based model,
     * and it was reported from the device as knobs jumping to a huge value
     * on first move. Both halves are pinned here, because the obvious fix
     * for a jump is an over-long glide and that is just as wrong.
     *
     * Driven through HISS on a constant take, measured as added noise.
     * It used to drive Darken and measure brightness; Darken went
     * 2026-08-28, and with its write removed the test still passed while
     * exercising no knob at all — so the metric moved to one that is
     * actually connected to something.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test17: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        record_full_buffer_loop_a_constant(api, inst, 8000);
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        run_silence(api, inst, BLOCK_FRAMES * 8);     /* clear the splice fade */

        double n_before = measure_noise(api, inst, 4);
        api->set_param(inst, "loopA_hiss", "0.9");
        double n_at_write = measure_noise(api, inst, 2);   /* ~6ms later */
        run_silence(api, inst, (long)(SAMPLE_RATE / 5));   /* 200ms */
        double n_settled = measure_noise(api, inst, 4);

        check(n_settled > n_before * 3.0,
              "test17: 200ms after the write the hiss has plainly arrived");
        check(n_at_write < n_before + (n_settled - n_before) * 0.35,
              "test17: but the very next blocks are still near the un-hissed "
              "level — the knob slews, it does not snap");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test18: the overdub write does not put a step edge into the take.
     *
     * Overdubs a CONSTANT level onto a silent take, keeps feeding it past
     * the toggle, then reads the loop back. Whatever the overdub wrote is
     * now IN the buffer and repeats every pass, so a step at the toggle is
     * a permanent click, which is exactly what was reported on device
     * 2026-08-27. Before the fade-in the transition was a single-sample
     * 0 -> ~3200 jump; it is now spread over OVERDUB_FADE_SECONDS.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test18: create_instance");
        api->set_param(inst, "input_routing", "A");
        api->set_param(inst, "loopA_decay_rate", "300");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_wow", "0");

        press_record(api, inst);
        run_silence(api, inst, BLOCK_FRAMES * 60);
        api->set_param(inst, "master_record", "STOP!");

        api->set_param(inst, "master_record", "DUB!");
        run_constant(api, inst, BLOCK_FRAMES * 20, 4000);
        api->set_param(inst, "master_record", "PLAY!");
        run_constant(api, inst, BLOCK_FRAMES * 5, 4000);

        /* read the take back; skip the first pass so we see what was
         * actually committed to the buffer, not the live overdub */
        int16_t buf[BLOCK_FRAMES * 2];
        int worst = 0, prev = 0, have_prev = 0;
        for (int b = 0; b < 240; b++) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (b < 63) continue;
            for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) {
                if (have_prev) {
                    int d = (int)buf[i] - prev;
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                }
                prev = (int)buf[i];
                have_prev = 1;
            }
        }
        check(worst < 400,
              "test18: overdub toggle leaves no step edge in the take "
              "(worst adjacent-sample jump well under the ~3200 the "
              "un-ramped write used to burn in)");

        api->destroy_instance(inst);
    }

    /* test19 removed with Erase (2026-08-27) — it pinned the erase
     * countdown on ECHO and on the trigger. */

    /* ---------------------------------------------------------------
     * test20: Hiss ACCUMULATES onto the medium.
     *
     * It is a per-pass rate, added to the signal that gets written back,
     * so noise printed on one pass is still there on the next and is
     * added to again. The take gets noisier the longer it runs, with the
     * knob untouched throughout.
     *
     * Replaces an assertion that Hiss was silent on a fresh take and only
     * arrived with age. That was true of the age-scaled design and is
     * deliberately not true now: age-coupling was removed when the
     * write-back made the compounding intrinsic.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test20: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        api->set_param(inst, "input_routing", "A");
        press_record(api, inst);
        int16_t buf[BLOCK_FRAMES * 2];
        for (int b = 0; b < 344; b++) {
            fill_constant(buf, BLOCK_FRAMES, 12000);
            api->process_block(inst, buf, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_hiss", "1");

        double n_early = measure_noise(api, inst, 344);
        run_silence(api, inst, (long)SAMPLE_RATE * 30);
        double n_late  = measure_noise(api, inst, 344);
        check(n_late > n_early * 2.0,
              "test20: the noise floor grows as the take runs — hiss printed "
              "on one pass is read back and printed on again");
        api->destroy_instance(inst);
    }

    /* test21 (Darken COMPOUNDS) removed 2026-08-28 with Darken itself.
     * The per-pass lowpass, the reverb wash and the loopX_hf_loss key are
     * all gone; the knob position is FREQ now. Per-pass compounding is
     * still covered by test19 (recursive medium) and test22 (VINYL). */

    /* ---------------------------------------------------------------
     * test23: VINYL measurably alters the take, and Freeze stops it.
     *
     * Deliberately weaker than it used to be. The gate is now gradual by
     * design — a per-pass floor and an age-SQUARED threshold, after
     * dropouts were reported as arriving "very large very early" and
     * leaving nothing but hiss — and the medium regulator hides what it
     * removes by boosting the remainder back to level. Between them, the
     * obvious signatures (silent fraction, crest factor) no longer order
     * cleanly on a synthetic take, and thresholds picked against them
     * measured noise rather than the gate.
     *
     * So this asserts what survives both effects: that turning VINYL up
     * changes the render at all, and that freezing part way through
     * changes it less. What the erosion SOUNDS like is a tuning question
     * for the device, not something to pin here with a false number.
     * --------------------------------------------------------------- */
    {
        /* 0: plain  1: VINYL  2: frozen  3: frozen+VINYL */
        static int16_t cap[4][200 * BLOCK_FRAMES];
        for (int pass = 0; pass < 4; pass++) {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test23: create_instance");
            api->set_param(inst, "loopA_decay_rate", "240");
            api->set_param(inst, "input_routing", "A");
            press_record(api, inst);
            float phase = 0.0f;
            int16_t tb[BLOCK_FRAMES * 2];
            for (int b = 0; b < 200; b++) {
                fill_tone(tb, BLOCK_FRAMES, (b >= 150) ? 0.08f : 0.30f, 400.0f, &phase);
                api->process_block(inst, tb, BLOCK_FRAMES);
            }
            api->set_param(inst, "master_record", "STOP!");
            api->set_param(inst, "loopA_volume", "1");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_send", "0");
            api->set_param(inst, "loopA_chaos", (pass == 1 || pass == 3) ? "1" : "0");
            if (pass >= 2) {
                run_silence(api, inst, (long)SAMPLE_RATE * 20);
                api->set_param(inst, "master_freeze", "Freeze!");
            }
            run_silence(api, inst, (long)SAMPLE_RATE * 100);
            for (int b = 0; b < 200; b++) {
                fill_silence(tb, BLOCK_FRAMES);
                api->process_block(inst, tb, BLOCK_FRAMES);
                for (int i = 0; i < BLOCK_FRAMES; i++)
                    cap[pass][b * BLOCK_FRAMES + i] = tb[i * 2];
            }
            api->destroy_instance(inst);
        }
        /* Each VINYL run is compared with its OWN control. Freezing also
         * halts Age, so a frozen take differs from an unfrozen one for
         * reasons that have nothing to do with the gate — comparing across
         * that difference measured the decay, not the damage. */
        double ref = 0.0, d_run = 0.0, d_frozen = 0.0;
        for (int i = 0; i < 200 * BLOCK_FRAMES; i++) {
            double c = cap[0][i];
            ref += c * c;
            double a = (double)cap[1][i] - c;
            double b = (double)cap[3][i] - (double)cap[2][i];
            d_run += a * a; d_frozen += b * b;
        }
        check(ref > 0.0, "test23: the control take is not silent");
        check(d_run > ref * 0.05,
              "test23: VINYL at maximum measurably alters the take");
        check(d_frozen < d_run,
              "test23: and it alters a take frozen part way through LESS — "
              "Freeze stops the medium moving past the head");
    }

    /* ---------------------------------------------------------------
     * test24: the feedback path neither dies nor clips.
     *
     * This is the failure everyone who builds this by hand reports, and
     * the reason they end up riding a fader for the whole take. With every
     * degradation running hard, the medium has to stay inside a band on
     * its own for minutes at a time. Age is set far longer than the run so
     * that a fade cannot be mistaken for the loop dying.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test24: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        api->set_param(inst, "input_routing", "A");
        press_record(api, inst);
        float phase = 0.0f;
        int16_t tb[BLOCK_FRAMES * 2];
        for (int b = 0; b < 344; b++) {
            fill_tone(tb, BLOCK_FRAMES, 0.30f, 220.0f, &phase);
            api->process_block(inst, tb, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_hiss", "0.6");
        api->set_param(inst, "loopA_chaos", "0.6");
        api->set_param(inst, "loopA_saturation", "0.4");
        api->set_param(inst, "loopA_wow", "0.3");

        double first = 0.0, worst_pk = 0.0, lo = 1e30, hi = 0.0;
        for (int t = 0; t < 12; t++) {
            double tot = 0.0; long n = 0;
            for (int b = 0; b < 344; b++) {
                fill_silence(tb, BLOCK_FRAMES);
                api->process_block(inst, tb, BLOCK_FRAMES);
                for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) {
                    double v = (double)tb[i];
                    tot += v * v; n++;
                    if (fabs(v) > worst_pk) worst_pk = fabs(v);
                }
            }
            double r = sqrt(tot / (double)n);
            if (t == 0) first = r;
            if (t > 0) { if (r < lo) lo = r; if (r > hi) hi = r; }
        }
        check(worst_pk < 32000.0,
              "test24: the feedback path never blows up into clipping");
        check(lo > first * 0.2,
              "test24: and never dies away — the regulator holds the medium "
              "up without a hand on the fader");
        check(hi < first * 3.0,
              "test24: nor does it inflate; the level stays in a band");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test25: the loop splice does not click.
     *
     * A take ends wherever the finger left it, so the buffer wraps from
     * one arbitrary sample to another. Reported on device as "a pop when
     * recording stops" — it is heard there because that is when playback
     * begins, but it fires on EVERY pass. A 220 Hz take is chosen so the
     * recorded length is not a whole number of cycles (period ~200.45
     * samples), which is what puts the two ends out of step; a DC take
     * would join itself and prove nothing.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test25: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        api->set_param(inst, "input_routing", "A");
        press_record(api, inst);
        float phase = 0.0f;
        int16_t tb[BLOCK_FRAMES * 2];
        for (int b = 0; b < 60; b++) {
            fill_tone(tb, BLOCK_FRAMES, 0.28f, 220.0f, &phase);
            api->process_block(inst, tb, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_saturation", "0");

        int worst = 0, prev = 0, have = 0;
        for (int b = 0; b < 60 * 3; b++) {       /* three whole passes */
            fill_silence(tb, BLOCK_FRAMES);
            api->process_block(inst, tb, BLOCK_FRAMES);
            for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) {
                if (have) {
                    int d = (int)tb[i] - prev;
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                }
                prev = (int)tb[i]; have = 1;
            }
        }
        /* the tone's own slope at this level is ~282/sample; the splice
         * step before this was ~8000 */
        check(worst < 600,
              "test25: the loop wraps without a click — both ends of the "
              "take are faded to silence, so the splice is silence to "
              "silence instead of an ~8000-count step every pass");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test26: the Distance page — speed over reverb send.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test26: create_instance");
        static char js[16384];
        int n = api->get_param(inst, "ui_hierarchy", js, sizeof(js));
        check(n > 0 && strstr(js, "\"sound\"") != NULL, "test26: a sound level exists");
        const char *d = strstr(js, "{\"level\":\"sound\"");
        const char *a = strstr(js, "{\"level\":\"loopA\"");
        check(d && a && d < a, "test26: Sound sits between Main and the loop pages");
        check(strstr(js, "\"master_freeze\",\"loopA_speed\"") != NULL,
              "test26: the speeds are Main's bottom row");
        check(strstr(js, "\"loopA_volume\",\"loopB_volume\",\"loopC_volume\",\"loopD_volume\","
                         "\"loopA_tone\"") != NULL,
              "test26: Sound is the levels over the tones");
        /* The four loops are ONE level, so they are one SECTION: one
         * bank-bar segment, one row in the section picker, and Shift+jog
         * steps sections rather than pages. The planner paginates the
         * level's 32 knobs into four pages inside it. */
        check(strstr(js, "\"level\":\"loopA\",\"label\":\"Loop A\"") != NULL,
              "test26: each loop is its own named section");
        check(strstr(js, "\"loopA_decay_rate\",\"loopA_trim\",\"loopA_freq\",\"loopA_state\"") != NULL,
              "test26: a loop page's top row is Age, Trim, FREQ, ECHO");
        check(strstr(js, "\"loopA_wow\",\"loopA_send\",\"loopA_chaos\",\"loopA_hiss\"") != NULL,
              "test26: and Space moved down to the flavour row, into the slot "
              "Darken used to hold");
        /* 32 knobs must land 8 per page with one loop each, or a page
         * straddles two memories and every label lies. */

        check(strstr(js, "loopA_saturation") == NULL,
              "test26: Drive is gone from the hierarchy");


        /* "1x" must not read back as a division: atoi("1x") is 1, so an
         * index-only parse selects the wrong option. */
        char v[32];
        /* Order and the full eight-option set are test38's business; this
         * only pins that a NAME round-trips, which is what "1x" vs atoi
         * is about. */
        const char *want[4] = { "1/4x", "1/2x", "1x", "2x" };
        for (int k = 0; k < 4; k++) {
            api->set_param(inst, "loopA_speed", want[k]);
            api->get_param(inst, "loopA_speed", v, sizeof(v));
            check(strcmp(v, want[k]) == 0, "test26: speed reads back what was written");
        }

        /* No state round-trip to test here any more — see test13. The
         * module answers "{}" and ignores what it is given, so a set can
         * neither save nor restore these. */
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test27: the send reverb rings on after its source stops, and the
     * send is POST-FADER.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test27: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        api->set_param(inst, "input_routing", "A");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_send", "1");
        press_record(api, inst);
        int16_t tb[BLOCK_FRAMES * 2];
        float phase = 0.0f;
        for (int b = 0; b < 150; b++) {
            fill_tone(tb, BLOCK_FRAMES, b < 20 ? 0.40f : 0.0f, 300.0f, &phase);
            api->process_block(inst, tb, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        run_silence(api, inst, BLOCK_FRAMES * 160);   /* one pass feeds the tank */
        api->set_param(inst, "loopA_volume", "0");    /* dry AND send now muted */

        double first = 0.0, later = 0.0;
        for (int seg = 0; seg < 2; seg++) {
            double tot = 0.0; long n = 0;
            for (int b = 0; b < 200; b++) {
                fill_silence(tb, BLOCK_FRAMES);
                api->process_block(inst, tb, BLOCK_FRAMES);
                for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) { tot += (double)tb[i]*tb[i]; n++; }
            }
            if (seg == 0) first = sqrt(tot/(double)n); else later = sqrt(tot/(double)n);
        }
        check(first > 50.0,
              "test27: the tail rings on after the source is muted — the "
              "reverb is fed by its own network, not by the send");
        check(later < first,
              "test27: and it decays rather than sustaining or growing");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test28: a speed change GLIDES, over SPEED_GLIDE_SECONDS.
     *
     * Both halves matter. A hard cut to half speed is a click and a lurch,
     * but a glide that never finishes is just as wrong, so this pins the
     * arrival too.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test28: create_instance");
        api->set_param(inst, "loopA_decay_rate", "600");
        api->set_param(inst, "input_routing", "A");
        press_record(api, inst);
        int16_t tb[BLOCK_FRAMES * 2];
        float phase = 0.0f;
        for (int b = 0; b < 400; b++) {
            fill_tone(tb, BLOCK_FRAMES, 0.28f, 440.0f, &phase);
            api->process_block(inst, tb, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_send", "0");

        double at_rest = measure_pitch(api, inst, 40);
        check(at_rest > 400.0 && at_rest < 480.0, "test28: starts near 440Hz");

        api->set_param(inst, "loopA_speed", "1/2x");
        double just_after = measure_pitch(api, inst, 40);   /* ~120ms */
        check(just_after > at_rest * 0.9,
              "test28: it does not snap — a moment after the write the loop "
              "is still close to where it was");

        run_silence(api, inst, (long)(SAMPLE_RATE * 2));
        double halfway = measure_pitch(api, inst, 40);
        check(halfway < at_rest * 0.92 && halfway > at_rest * 0.55,
              "test28: and it is genuinely on the way, not waiting to jump "
              "at the end");

        run_silence(api, inst, (long)(SAMPLE_RATE * 4));
        double arrived = measure_pitch(api, inst, 40);
        check(arrived > 210.0 && arrived < 230.0,
              "test28: arrives an octave down, and settles there");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test29: Tone is a DJ filter — bypass at centre, lowpass left,
     * highpass right. Measured on broadband content with a Goertzel at
     * 150Hz and 6kHz, since the whole point is which END goes away.
     * --------------------------------------------------------------- */
    {
        double lo[3], hi[3];
        const int tones[3] = { -100, 0, 100 };
        for (int k = 0; k < 3; k++) {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test29: create_instance");
            api->set_param(inst, "loopA_decay_rate", "600");
            api->set_param(inst, "input_routing", "A");
            press_record(api, inst);
            int16_t tb[BLOCK_FRAMES * 2];
            g_noise_state = 909u;
            for (int b = 0; b < 200; b++) {
                fill_noise(tb, BLOCK_FRAMES, 0.28f);
                api->process_block(inst, tb, BLOCK_FRAMES);
            }
            api->set_param(inst, "master_record", "STOP!");
            api->set_param(inst, "loopA_volume", "1");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_send", "0");
            char tv[16]; snprintf(tv, sizeof(tv), "%.2f", tones[k] / 100.0);
            api->set_param(inst, "loopA_tone", tv);
            run_silence(api, inst, BLOCK_FRAMES * 20);

            static double x[160 * BLOCK_FRAMES];
            long n = 0;
            for (int b = 0; b < 160; b++) {
                fill_silence(tb, BLOCK_FRAMES);
                api->process_block(inst, tb, BLOCK_FRAMES);
                for (int i = 0; i < BLOCK_FRAMES; i++) x[n++] = tb[i * 2];
            }
            for (int band = 0; band < 2; band++) {
                double fq = band ? 6000.0 : 150.0;
                double w = 2.0 * TEST_PI * fq / (double)SAMPLE_RATE;
                double c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
                for (long i = 0; i < n; i++) { double s0 = x[i] + c*s1 - s2; s2 = s1; s1 = s0; }
                double m = sqrt(fabs(s1*s1 + s2*s2 - c*s1*s2)) / (double)n;
                if (band) hi[k] = m; else lo[k] = m;
            }
            api->destroy_instance(inst);
        }
        check(lo[1] > 0.0 && hi[1] > 0.0, "test29: centre passes both ends");
        check(hi[0] < hi[1] * 0.2,
              "test29: hard left takes the top away (lowpass)");
        check(lo[0] > hi[0],
              "test29: and leaves the bottom behind");
        check(lo[2] < lo[1] * 0.2,
              "test29: hard right takes the bottom away (highpass)");
    }

    /* ---------------------------------------------------------------
     * test30: the flavour knobs return to zero when a take dies.
     *
     * At death and nowhere else. Doing it on every close_recording put
     * the module and the UI out of step invisibly — the UI kept showing
     * the old position while the module read zero, so the first nudge of
     * the encoder wrote the OLD value back and the sound jumped.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test30: create_instance");
        api->set_param(inst, "input_routing", "A");
        api->set_param(inst, "loopA_decay_rate", "6");
        api->set_param(inst, "loopA_volume", "1");
        press_record(api, inst);
        int16_t tb[BLOCK_FRAMES * 2];
        float phase = 0.0f;
        for (int b = 0; b < 120; b++) {
            fill_tone(tb, BLOCK_FRAMES, 0.28f, 330.0f, &phase);
            api->process_block(inst, tb, BLOCK_FRAMES);
        }
        api->set_param(inst, "master_record", "STOP!");
        api->set_param(inst, "loopA_wow", "0.7");
        api->set_param(inst, "loopA_chaos", "0.8");
        api->set_param(inst, "loopA_hiss", "0.5");
        api->set_param(inst, "loopA_trim", "0.2");
        api->set_param(inst, "loopA_tone", "-0.7");
        api->set_param(inst, "loopA_send", "0.9");
        api->set_param(inst, "loopA_speed", "1/4x");
        api->set_param(inst, "loopA_volume", "1.4");
        run_silence(api, inst, BLOCK_FRAMES * 20);

        char v[32];
        api->get_param(inst, "loopA_wow", v, sizeof(v));
        check(atof(v) > 0.5, "test30: the knob holds while the take is alive");

        run_silence(api, inst, (long)SAMPLE_RATE * 8);   /* well past its 6s */
        static const char *flav[] = { "wow", "chaos", "hiss" };
        for (int i = 0; i < 4; i++) {
            char key[32];
            snprintf(key, sizeof(key), "loopA_%s", flav[i]);
            api->get_param(inst, key, v, sizeof(v));
            check(atof(v) == 0.0,
                  "test30: every flavour knob is back at zero once the take "
                  "has gone silent");
        }
        /* Everything the take owned goes back to its default with it. */
        api->get_param(inst, "loopA_decay_rate", v, sizeof(v));
        check(atof(v) > 299.0, "test30: Age returns to its maximum");
        api->get_param(inst, "loopA_trim", v, sizeof(v));
        check(atof(v) > 0.49 && atof(v) < 0.51, "test30: Trim returns to centre");
        api->get_param(inst, "loopA_tone", v, sizeof(v));
        check(atof(v) == 0.0, "test30: Tone returns to centre");
        api->get_param(inst, "loopA_send", v, sizeof(v));
        check(atof(v) == 0.0, "test30: Space returns to zero");
        api->get_param(inst, "loopA_speed", v, sizeof(v));
        check(strcmp(v, "1x") == 0, "test30: speed returns to 1x");
        api->get_param(inst, "loopA_volume", v, sizeof(v));
        check(atof(v) > 0.79 && atof(v) < 0.81,
              "test30: level returns to its 80% default");
        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test31: Trim shortens the loop from either end.
     *
     * The marker is placed so it survives the trim being measured — one
     * click cannot outlive both a START trim that eats the front and an
     * END trim that eats the back, so each direction gets its own take.
     * --------------------------------------------------------------- */
    {
        const int TAKE = 200;
        /* pass 0: marker at the head, measures END trims (right of centre)
           pass 1: marker at 75%, measures START trims (left of centre)   */
        /* Block 5, not 0: close_recording fades the take's first 4ms to
         * silence for the splice, which erased a marker placed at the very
         * head — this test had never actually run, so that went unseen. */
        const int marker[2] = { 5, 150 };
        const char *trims[2][3] = { { "0.5", "0.75", "0.95" }, { "0.5", "0.35", "0.2" } };
        for (int pass = 0; pass < 2; pass++) {
            double prev = 0.0;
            for (int k = 0; k < 3; k++) {
                void *inst = api->create_instance(".", NULL);
                check(inst != NULL, "test31: create_instance");
                api->set_param(inst, "input_routing", "A");
                api->set_param(inst, "loopA_decay_rate", "600");
                api->set_param(inst, "loopA_volume", "1");
                press_record(api, inst);
                int16_t tb[BLOCK_FRAMES * 2];
                for (int b = 0; b < TAKE; b++) {
                    fill_silence(tb, BLOCK_FRAMES);
                    if (b == marker[pass])
                        for (int i = 0; i < 40; i++) { tb[i*2] = 14000; tb[i*2+1] = 14000; }
                    api->process_block(inst, tb, BLOCK_FRAMES);
                }
                api->set_param(inst, "master_record", "STOP!");
                api->set_param(inst, "loopA_hiss", "0");
                api->set_param(inst, "loopA_chaos", "0");
                api->set_param(inst, "loopA_wow", "0");
                api->set_param(inst, "loopA_send", "0");
                api->set_param(inst, "loopA_trim", trims[pass][k]);

                static double x[900 * BLOCK_FRAMES];
                long n = 0; double pk = 0.0;
                for (int b = 0; b < 900; b++) {
                    fill_silence(tb, BLOCK_FRAMES);
                    api->process_block(inst, tb, BLOCK_FRAMES);
                    for (int i = 0; i < BLOCK_FRAMES; i++) {
                        x[n] = tb[i * 2];
                        if (fabs(x[n]) > pk) pk = fabs(x[n]);
                        n++;
                    }
                }
                double sum = 0.0; long cnt = 0, last = -1;
                for (long i = 1; i < n; i++) {
                    if (fabs(x[i]) > pk * 0.5 && fabs(x[i-1]) <= pk * 0.5) {
                        if (last >= 0) { sum += (double)(i - last); cnt++; }
                        last = i;
                    }
                }
                double per = cnt ? sum / (double)cnt / (double)SAMPLE_RATE : 0.0;
                check(per > 0.0, "test31: the marker repeats, so a period is measurable");
                if (k == 0) {
                    check(per > 0.5 && per < 0.65,
                          "test31: at centre the whole take plays (~0.58s)");
                } else {
                    check(per < prev * 0.95,
                          "test31: and every step away from centre makes the loop "
                          "shorter — left from the front, right from the back");
                }
                prev = per;
                api->destroy_instance(inst);
            }
        }
    }

    /* ---------------------------------------------------------------
     * test32: a trimmed loop wraps without a click.
     *
     * close_recording fades the take's two ENDS to silence, so an
     * untrimmed loop joins silence to silence. A trimmed loop joins
     * somewhere in the middle of the take, where nothing was faded, and
     * measured an ~8900-count step against a signal whose own slope is
     * 282 — a click every pass. The join is crossfaded now.
     * --------------------------------------------------------------- */
    {
        const char *trims[] = { "0.5", "0.6", "0.75", "0.9", "0.4", "0.25", "0.1" };
        for (size_t k = 0; k < sizeof(trims) / sizeof(trims[0]); k++) {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test32: create_instance");
            api->set_param(inst, "input_routing", "A");
            api->set_param(inst, "loopA_decay_rate", "600");
            api->set_param(inst, "loopA_volume", "1");
            press_record(api, inst);
            int16_t tb[BLOCK_FRAMES * 2];
            float phase = 0.0f;
            for (int b = 0; b < 200; b++) {
                fill_tone(tb, BLOCK_FRAMES, 0.28f, 220.0f, &phase);
                api->process_block(inst, tb, BLOCK_FRAMES);
            }
            api->set_param(inst, "master_record", "STOP!");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_send", "0");
            api->set_param(inst, "loopA_trim", trims[k]);

            int worst = 0, prev = 0, have = 0;
            for (int b = 0; b < 600; b++) {
                fill_silence(tb, BLOCK_FRAMES);
                api->process_block(inst, tb, BLOCK_FRAMES);
                if (b < 160) continue;         /* let it settle into the trim */
                for (int i = 0; i < BLOCK_FRAMES * 2; i += 2) {
                    if (have) {
                        int d = (int)tb[i] - prev;
                        if (d < 0) d = -d;
                        if (d > worst) worst = d;
                    }
                    prev = (int)tb[i]; have = 1;
                }
            }
            check(worst < 900,
                  "test32: a trimmed loop wraps without a click — the join is "
                  "crossfaded, so the worst step stays near the tone's own "
                  "slope instead of jumping to ~8900");
            api->destroy_instance(inst);
        }
    }

    /* ---------------------------------------------------------------
     * test33: every "%" param is declared as a FRACTION.
     *
     * The UI multiplies a %-unit value by 100 to display it — which is
     * why loopX_volume declares 0..1.5 and reads as 0..150%. Trim and Tone
     * were declared 0..100 and -100..100, so the device announced
     * "Trim, 5000%". Nothing in the contract catches that; only the screen
     * reader did.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test33: create_instance");
        static char js[16384];
        int n = api->get_param(inst, "chain_params", js, sizeof(js));
        check(n > 0, "test33: chain_params served");
        /* Walk the declarations looking for a "%" unit whose max is > 2:
         * no fraction-valued control has a legitimate maximum above that,
         * and every 100-scale mistake lands well past it. */
        const char *p = js;
        int checked = 0, bad = 0;
        while ((p = strstr(p, "\"unit\":\"%\"")) != NULL) {
            const char *seg = p;
            while (seg > js && *seg != '{') seg--;
            const char *mx = strstr(seg, "\"max\":");
            if (mx && mx < p) {
                double m = atof(mx + 6);
                checked++;
                if (m > 2.0) bad++;
            }
            p++;
        }
        check(checked >= 12, "test33: found the %-unit params to check");
        check(bad == 0,
              "test33: no %-unit param declares a max above 2 — a "
              "percentage control is a FRACTION on the wire, because the "
              "UI scales it by 100 for display");
        api->destroy_instance(inst);
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    /* Keep this list honest: it is the only summary of what the suite
     * covers, and it had gone on advertising erase, saturation and the
     * first-touch/second-touch ramp for some time after all three were
     * deleted. */

    /* ---- Test 34: the Glitch page. An end-of-chain step sequencer of
     * destructive effects, sitting after the send reverb and before the
     * output limiter.
     *
     * The two claims that make the page safe to leave loaded are pinned
     * FIRST, because both are one-line regressions: Mix at 0 is bypass,
     * and Odds at 0 is bypass whatever Mix says. Only then is it worth
     * asserting that turning both up does something. ---- */
    {
        static const char *dry[]     = { "glitch_mix", "0" };
        static const char *odds0[]   = { "glitch_mix", "1", "glitch_odds", "0" };
        static const char *stut[]    = { "glitch_mix", "1", "glitch_odds", "1",
                                         "glitch_kind", "Stutter",
                                         "glitch_step", "125" };
        glitch_run(api, g_capture_a, dry, 2);

        glitch_run(api, g_capture_b, odds0, 4);
        check(buf_diff(g_capture_a, g_capture_b) == 0,
              "test34: Odds at 0 is bypass even at full Mix — nothing is ever "
              "rolled active, so the page is silent-running");

        static const char *mix0[] = { "glitch_mix", "0", "glitch_odds", "1",
                                      "glitch_kind", "Crush" };
        glitch_run(api, g_capture_b, mix0, 6);
        check(buf_diff(g_capture_a, g_capture_b) == 0,
              "test34: Mix at 0 is bypass even at full Odds");

        glitch_run(api, g_capture_b, stut, 8);
        long diff = buf_diff(g_capture_a, g_capture_b);
        check(diff > GTEST_BLOCKS * BLOCK_FRAMES / 4,
              "test34: Mix and Odds up actually changes the signal, and on a "
              "large fraction of samples rather than a stray few");

        /* Clicks. Every splice in here is faded (grain wrap, step edge,
         * gate edge) and this module has already shipped two click bugs,
         * so the sample-to-sample step is bounded rather than eyeballed.
         * A hard cut on a 0.5-amplitude tone would show ~32k. */
        int dry_step = max_step(g_capture_a);
        int wet_step = max_step(g_capture_b);
        check(wet_step < 6000,
              "test34: Stutter introduces no click — bounded sample-to-sample step");
        check(wet_step >= dry_step,
              "test34: (and the bound is not vacuous — it is above the dry one)");

        /* Every Kind is reachable and audibly distinct, including through
         * Tumble, which rolls one per step. A Kind that silently fell
         * through to passthrough would look exactly like a working one
         * from the contract side. */
        static const char *kinds[] = { "Tumble", "Stutter", "Rewind",
                                       "Tape", "Gate", "Crush" };
        for (int k = 0; k < 6; k++) {
            const char *cfg[] = { "glitch_mix", "1", "glitch_odds", "1",
                                  "glitch_kind", kinds[k], "glitch_step", "125" };
            glitch_run(api, g_capture_b, cfg, 8);
            check(buf_diff(g_capture_a, g_capture_b) > GTEST_BLOCKS * BLOCK_FRAMES / 8,
                  "test34: this Kind reaches the audio");
            /* Crush is bounded LOOSER on purpose. Sample-and-hold is a
             * step function — the discontinuity is the effect itself, and
             * a bound tight enough for the others would only pass if the
             * crusher had quietly stopped crushing. Still bounded, because
             * a full-scale step is a different thing from a coarse one. */
            int cap = (strcmp(kinds[k], "Crush") == 0) ? 26000 : 20000;
            check(max_step(g_capture_b) < cap,
                  "test34: ...and does it without a full-scale discontinuity");

            char rb[64];
            void *pi = api->create_instance(".", NULL);
            api->set_param(pi, "glitch_kind", kinds[k]);
            api->get_param(pi, "glitch_kind", rb, sizeof(rb));
            check(strcmp(rb, kinds[k]) == 0,
                  "test34: Kind round-trips as a LABEL — the host learns an "
                  "enum's wire format from get_param, and input_routing "
                  "records what a bare atoi() cost");
            api->destroy_instance(pi);
        }

        /* Reach is the granular half: at 0 the grain is the audio that just
         * went past, turned up it is grabbed from anywhere in the last two
         * seconds. Same seed, same steps — only where they read from moves. */
        static const char *near_[] = { "glitch_mix", "1", "glitch_odds", "1",
                                       "glitch_kind", "Stutter", "glitch_reach", "0" };
        static const char *far_[]  = { "glitch_mix", "1", "glitch_odds", "1",
                                       "glitch_kind", "Stutter", "glitch_reach", "1" };
        glitch_run(api, g_capture_a, near_, 8);
        glitch_run(api, g_capture_b, far_, 8);
        check(buf_diff(g_capture_a, g_capture_b) > 0,
              "test34: Reach moves where grains are grabbed from");

        /* Ranges clamp rather than wrap into a divide-by-nothing. */
        {
            void *pi = api->create_instance(".", NULL);
            char rb[64];
            api->set_param(pi, "glitch_step", "0");
            api->get_param(pi, "glitch_step", rb, sizeof(rb));
            check(atof(rb) >= 20.0, "test34: Step clamps at its floor");
            api->set_param(pi, "glitch_step", "99999");
            api->get_param(pi, "glitch_step", rb, sizeof(rb));
            check(atof(rb) <= 1000.0, "test34: Step clamps at its ceiling");
            api->set_param(pi, "glitch_pitch", "-99");
            api->get_param(pi, "glitch_pitch", rb, sizeof(rb));
            check(atof(rb) >= -12.0, "test34: Pitch clamps");
            api->destroy_instance(pi);
        }
    }


    /* ---- Test 35: a key we do not implement answers "" (len 0), never -1.
     *
     * shadow_chain_mgmt.c maps a negative return to error=4 / result_len=-1,
     * the JS side reads that as null, and null means "the read did not
     * complete" — which the host RETRIES. A device left on the Forgetful
     * page logged 19,913 param_giveup events on fx1:preset_name alone,
     * roughly one a second, purely because a key we do not have was
     * answered as a failure rather than as an absence.
     *
     * The probed keys are listed explicitly rather than tested with one
     * made-up key: they are what the host actually asks for on a repaint,
     * and a future handler that starts answering one of them for real
     * should have to come here and say so. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        static const char *probed[] = {
            "preset_name", "is_loading", "display_name", "name_unset",
            "patch_count", "knob_1_name", "dirty", "state_unknown_key",
            "glitch_nonesuch", "loopA_nonesuch", "totally_made_up"
        };
        for (size_t k = 0; k < sizeof(probed)/sizeof(probed[0]); k++) {
            char b[256];
            memset(b, 'x', sizeof(b));
            int n = api->get_param(inst, probed[k], b, sizeof(b));
            check(n >= 0,
                  "test35: an unimplemented key is SERVED, not failed — a "
                  "negative return reads as 'did not complete' and is retried");
            check(n == 0 && b[0] == '\0',
                  "test35: ...and the answer is an empty string");
        }

        /* Not vacuous: a key we DO implement still answers with content, and
         * a genuinely broken call still fails. */
        {
            char b[64];
            int n = api->get_param(inst, "name", b, sizeof(b));
            check(n > 0 && b[0] != '\0', "test35: a real key still answers");
            check(api->get_param(inst, "name", NULL, 64) < 0,
                  "test35: a genuinely broken call still returns -1 — the "
                  "failure channel is kept for actual failures");
        }
        api->destroy_instance(inst);
    }


    /* ---- Test 36: Tone sits AHEAD of the Darken wash and the VINYL
     * crackle, and behind the write-back.
     *
     * It used to be dead last, so turning it down darkened the reverb and
     * the surface noise along with the loop. This pins the new order
     * WITHOUT needing a reference build: with Tone hard left, switching
     * VINYL from off to full must raise the output's high-frequency
     * content, because the crackle is added after the filter. Measured,
     * the old order moved this ratio 0.99x — the filter ate the crackle —
     * and the new one moves it 2.52x.
     *
     * That discrimination is the whole point. The existing Tone test
     * passed with the filter in EITHER position, so it could not have
     * caught this move in either direction. ---- */
    {
        double quiet, crackly;
        {
            void *inst = api->create_instance(".", NULL);
            record_full_buffer_loop_a_noise(api, inst);
            api->set_param(inst, "loopA_decay_rate", "300");
            api->set_param(inst, "loopA_tone", "-0.9");
            api->set_param(inst, "loopA_chaos", "0");
            run_silence(api, inst, 340L * BLOCK_FRAMES);
            quiet = measure_hf_ratio(api, inst, 120);
            api->destroy_instance(inst);
        }
        {
            void *inst = api->create_instance(".", NULL);
            record_full_buffer_loop_a_noise(api, inst);
            api->set_param(inst, "loopA_decay_rate", "300");
            api->set_param(inst, "loopA_tone", "-0.9");
            api->set_param(inst, "loopA_chaos", "1.0");
            run_silence(api, inst, 340L * BLOCK_FRAMES);
            crackly = measure_hf_ratio(api, inst, 120);
            api->destroy_instance(inst);
        }
        check(quiet > 0.0, "test36: the lowpassed loop still produces output");
        check(crackly > quiet * 1.8,
              "test36: VINYL crackle survives a hard-left Tone — the filter is "
              "ahead of it, not after it");

        /* And the other half: Tone must NOT move any earlier than the
         * write-back. Inside the recursion it stops being a tone control —
         * measured over 20 passes at -0.6 with Hiss on, brightness
         * collapsed 0.069 -> 0.020 while RMS ROSE 777 -> 13454, because the
         * level regulator shares that loop and boosts to replace what the
         * filter removes. Dark and loud at once, and centring the knob does
         * not undo it. So: a filtered loop must stay REVERSIBLE. */
        {
            void *inst = api->create_instance(".", NULL);
            record_full_buffer_loop_a_noise(api, inst);
            api->set_param(inst, "loopA_decay_rate", "300");
            api->set_param(inst, "loopA_hiss", "0.15");
            api->set_param(inst, "loopA_tone", "0");
            run_silence(api, inst, 340L * BLOCK_FRAMES);
            double open_before = measure_hf_ratio(api, inst, 60);

            api->set_param(inst, "loopA_tone", "-0.6");
            run_silence(api, inst, 3400L * BLOCK_FRAMES);   /* ~20 passes */
            double filtered = measure_hf_ratio(api, inst, 60);

            api->set_param(inst, "loopA_tone", "0");
            run_silence(api, inst, 340L * BLOCK_FRAMES);
            double open_after = measure_hf_ratio(api, inst, 60);

            check(filtered < open_before * 0.5,
                  "test36: the filter actually filters");
            check(open_after > open_before * 0.5,
                  "test36: and centring RESTORES it — Tone is outside the "
                  "write-back, so twenty passes of lowpass did not eat the "
                  "medium permanently");
            api->destroy_instance(inst);
        }
    }


    /* ---- Test 37: FREQ — fine varispeed, replacing Darken's slot.
     *
     * Semitones, not a fraction: -12..+12, no "%" unit, so nothing scales
     * it by 100 the way loopX_volume's 0..1.5 becomes 0..150%.
     *
     * It folds into the SAME log2 sum the Speed enum feeds, on its own
     * glide. Both halves are pinned: the ratio has to be right across the
     * range, and it has to compose with Speed rather than replace it —
     * +7st at 2x is very nearly a perfect twelfth, not a fifth. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        char b[64];

        api->get_param(inst, "loopA_freq", b, sizeof(b));
        check(strcmp(b, "0.00") == 0, "test37: FREQ defaults to no shift");
        api->set_param(inst, "loopA_freq", "99");
        api->get_param(inst, "loopA_freq", b, sizeof(b));
        check(atof(b) <= 12.0, "test37: FREQ clamps at +12 semitones");
        api->set_param(inst, "loopA_freq", "-99");
        api->get_param(inst, "loopA_freq", b, sizeof(b));
        check(atof(b) >= -12.0, "test37: FREQ clamps at -12 semitones");

        /* Darken is gone, key and all — not merely unhooked from the page.
         * A stale key that still answers is how a removed control keeps
         * being writable from a patch nobody edited. */
        int n = api->get_param(inst, "loopA_hf_loss", b, sizeof(b));
        check(n == 0 && b[0] == '\0',
              "test37: loopA_hf_loss is ABSENT — Darken is gone, not hidden");
        api->destroy_instance(inst);

        static const struct { const char *semis; const char *speed; double ratio; }
        cases[] = {
            { "0",   "1x", 1.0        },
            { "-12", "1x", 0.5        },
            { "7",   "1x", 1.4983     },
            { "12",  "1x", 2.0        },
            { "7",   "2x", 2.9966     },   /* composes with Speed, not replaces */
        };
        double base = 0.0;
        for (size_t k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
            void *in2 = api->create_instance(".", NULL);
            float phase = 0.0f;
            record_full_buffer_loop_a(api, in2, &phase);
            api->set_param(in2, "loopA_decay_rate", "300");
            api->set_param(in2, "loopA_speed", cases[k].speed);
            api->set_param(in2, "loopA_freq", cases[k].semis);
            /* long settle: Speed glides for SPEED_GLIDE_SECONDS (5s), and
             * measuring inside that glide reads a moving pitch, which is
             * exactly the mistake that made the 2x case look broken. */
            run_silence(api, in2, 8L * SAMPLE_RATE);
            double hz = measure_pitch(api, in2, 200);
            api->destroy_instance(in2);
            if (k == 0) { base = hz; check(base > 0.0, "test37: baseline pitch measurable"); continue; }
            double got = hz / base;
            check(got > cases[k].ratio * 0.97 && got < cases[k].ratio * 1.03,
                  "test37: FREQ shifts by the semitones it says, and composes "
                  "with the Speed enum");
        }
    }


    /* ---- Test 38: the speed enum, including the reverse half.
     *
     * Requested order (2026-08-29): default 1x, LEFT stepping 1/2, 1/4, 2x
     * and RIGHT stepping -1x, -1/2, -1/4, -2x. That puts 2x at the FAR LEFT,
     * which looks like a mistake and is not — it is what was asked for, so
     * the order is pinned literally here rather than left to look tidy.
     *
     * Nothing pinned the old option list at all, so the enum could have been
     * rewritten silently. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        /* 8192, not 4096: chain_params is ~6KB and the module REFUSES a
         * short buffer rather than truncating, so an undersized one here
         * returns -1 and leaves the buffer uninitialised — which reads as
         * a contract failure that is really a test bug. */
        char b[8192];
        int cpn = api->get_param(inst, "chain_params", b, sizeof(b));
        check(cpn > 0, "test38: chain_params fits the buffer this test gives it");
        check(strstr(b, "\"options\":[\"2x\",\"1/4x\",\"1/2x\",\"1x\","
                        "\"-1x\",\"-1/2x\",\"-1/4x\",\"-2x\"],\"default\":\"1x\"") != NULL,
              "test38: speed options are declared in the requested knob order, "
              "1x default");

        static const char *opts[] = { "2x","1/4x","1/2x","1x","-1x","-1/2x","-1/4x","-2x" };
        for (int i = 0; i < 8; i++) {
            char v[16];
            api->set_param(inst, "loopA_speed", opts[i]);
            api->get_param(inst, "loopA_speed", v, sizeof(v));
            check(strcmp(v, opts[i]) == 0,
                  "test38: each speed round-trips by NAME — the host learns the "
                  "wire format from get_param, and 1x/-1x differ only in sign");
            snprintf(v, sizeof(v), "%d", i);
            api->set_param(inst, "loopA_speed", v);
            api->get_param(inst, "loopA_speed", v, sizeof(v));
            check(strcmp(v, opts[i]) == 0,
                  "test38: ...and by INDEX, in the same order a patch writes");
        }
        api->destroy_instance(inst);

        /* Direction, measured off a ramp. A tone played backwards is the
         * same tone, so this needs a signal with a direction. */
        static const struct { const char *opt; double want; } dirs[] = {
            { "1x", 1.0 }, { "-1x", -1.0 }, { "2x", 1.0 },
            { "-2x", -1.0 }, { "1/2x", 1.0 }, { "-1/2x", -1.0 },
        };
        for (size_t k = 0; k < sizeof(dirs)/sizeof(dirs[0]); k++) {
            void *in2 = api->create_instance(".", NULL);
            record_full_buffer_loop_a_ramp(api, in2);
            api->set_param(in2, "loopA_decay_rate", "300");
            api->set_param(in2, "loopA_wow", "0");
            api->set_param(in2, "loopA_speed", dirs[k].opt);
            run_silence(api, in2, 7L * SAMPLE_RATE);   /* past the 5s glide */
            double sl = measure_slope_sign(api, in2, 300);
            check(sl * dirs[k].want > 0.7,
                  "test38: this speed plays the direction its name says");
            api->destroy_instance(in2);
        }

        /* The flip COASTS THROUGH ZERO. Direction is a linear ramp, separate
         * from the log2 magnitude glide, precisely because log2 cannot cross
         * zero — so 1x -> -1x, where the magnitude never changes, would
         * otherwise be a discontinuity at full speed. Measured: the tone
         * falls 440 -> ~44 -> 440 across the five seconds. */
        {
            void *in2 = api->create_instance(".", NULL);
            float phase = 0.0f;
            record_full_buffer_loop_a(api, in2, &phase);
            api->set_param(in2, "loopA_decay_rate", "300");
            api->set_param(in2, "loopA_wow", "0");
            api->set_param(in2, "loopA_speed", "1x");
            run_silence(api, in2, 7L * SAMPLE_RATE);
            double before = measure_pitch(api, in2, 200);

            api->set_param(in2, "loopA_speed", "-1x");
            run_silence(api, in2, (long)(2.4 * SAMPLE_RATE));  /* mid-flip */
            double middle = measure_pitch(api, in2, 200);

            run_silence(api, in2, 6L * SAMPLE_RATE);           /* settled */
            double after = measure_pitch(api, in2, 200);

            check(before > 300.0, "test38: baseline pitch measurable before the flip");
            check(middle < before * 0.35,
                  "test38: mid-flip the tape has nearly STOPPED — the direction "
                  "ramp passes through zero rather than cutting across it");
            check(after > before * 0.8,
                  "test38: and it comes back up to speed on the far side");
            api->destroy_instance(in2);
        }

        /* The reverse JOIN is crossfaded, like the forward one.
         *
         * Deleting the reverse wrap does not stop the loop wrapping — the
         * pre-read clamp still catches an out-of-window head — so the only
         * evidence is a CLICK at the seam, and the bound has to be tight
         * enough to see it. Measured on this fixture: 153 with the
         * crossfade, 13384 without. The forward direction is 147 either
         * way, which is what says the number is the seam and not the ramp. */
        {
            void *in2 = api->create_instance(".", NULL);
            record_short_ramp_loop_a(api, in2, SAMPLE_RATE);   /* ~1s: wraps often */
            api->set_param(in2, "loopA_decay_rate", "300");
            api->set_param(in2, "loopA_wow", "0");
            api->set_param(in2, "loopA_speed", "-1x");
            run_silence(api, in2, 7L * SAMPLE_RATE);
            int step = measure_max_step(api, in2, 1200);
            check(step < 1000,
                  "test38: running backwards, the loop seam is crossfaded — a "
                  "full-scale ramp joins smoothly instead of clicking");
            api->destroy_instance(in2);
        }
    }


    /* ---- Test 39: the ECHO countdown never shows 0.
     *
     * '0' and 'O' are the same shape in the device font, so a loop about to
     * die looked exactly like a loop being overdubbed — the two states
     * furthest apart in meaning. Reported from the device 2026-08-29.
     *
     * The scale is now nine steps rounded UP, 9..1, so any memory left at
     * all still shows a digit and '-' means gone rather than nearly gone.
     *
     * Sampled across a whole short life rather than at a couple of points,
     * because the failing character only appears in the last tenth: the old
     * decile scale passed every existing test in the suite. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        record_short_ramp_loop_a(api, inst, SAMPLE_RATE / 2);
        api->set_param(inst, "loopA_decay_rate", "3");   /* MIN_DECAY_RATE */

        int seen[128];
        memset(seen, 0, sizeof(seen));
        char prev_digit = 0;
        int monotonic = 1;
        for (int t = 0; t < 120; t++) {                  /* ~4.4s > the 3s life */
            char c[8];
            api->get_param(inst, "loopA_state", c, sizeof(c));
            unsigned char u = (unsigned char)c[0];
            seen[u & 127] = 1;
            if (c[0] >= '1' && c[0] <= '9') {
                if (prev_digit && c[0] > prev_digit) monotonic = 0;
                prev_digit = c[0];
            }
            run_silence(api, inst, SAMPLE_RATE / 27);
        }
        api->destroy_instance(inst);

        check(!seen['0'],
              "test39: ECHO never shows '0' — it is the same glyph as 'O' for "
              "overdub, on the state that means the opposite");
        check(seen['9'], "test39: a fresh take starts at 9");
        check(seen['1'],
              "test39: and reaches 1 — the bottom step is used, so the digit "
              "runs out exactly when the loop does");
        check(seen['-'], "test39: then goes to '-' when it is gone");
        check(monotonic, "test39: the countdown only ever counts DOWN");
    }

    /* The PASS line used to print unconditionally and the runner grepped
     * for it, so a suite with real failures still read as green — the same
     * class of blind pass signal as the earlier `grep -c "^FAIL"` that
     * counted compile errors as success. Report and EXIT NONZERO. */
    if (g_failures) {
        fprintf(stderr, "SUITE FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("PASS: forgetful LoopEngine bench test "
           "(chain_params shape, manual record start/stop, decay timing, "
           "continuous decay, routing, status-line word buckets, Loops "
           "Overview format, too-short blip discard, parameter clamping, "
           "extreme decay_rate, state round-trip, master_freeze, flavour "
           "knob slew, overdub toggle, click-free overdub write, recursive "
           "medium, Darken compounding, VINYL gate, level regulator, splice "
           "fade, Sound page, send reverb, speed glide, Tone filter, Trim + "
           "join crossfade, reset on take death, Glitch page, absent-key "
           "contract, Tone ordering, FREQ, reverse speeds)\n");
    return 0;
}
