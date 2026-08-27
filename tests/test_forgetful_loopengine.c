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
 *   0. chain_params / ui_hierarchy shape — exactly 40 entries (8 master +
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

#define TEST_PI_F 3.14159265358979323846f

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

/* loopX_status is a full state line, not a bare state name — Idle reads
 * "Ready" (changed from "Listening..." with the manual-record redesign,
 * since the loop is no longer passively monitoring input level) and Looping
 * reads "Looping - NN% (word)" with a live percentage, so the LOOPING checks
 * below match the fixed prefix rather than the whole string. */
static int status_is_looping(const char *status) {
    return strncmp(status, "Looping - ", 10) == 0;
}
#define STATUS_READY "Ready"

static const char *erase_readout(audio_fx_api_v2_t *api, void *inst, char letter) {
    static char key[32];
    static char buf[64];
    snprintf(key, sizeof(key), "loop%c_erase", letter);
    int n = api->get_param(inst, key, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

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
        check(key_count == 40, "test0: chain_params has exactly 40 entries (8 master + 4x8 loop)");

        n = api->get_param(inst, "ui_hierarchy", hier, sizeof(hier));
        check(n > 0, "test0: ui_hierarchy readable");
        check(n > 0 && strstr(hier, "\"knobs\":[") != NULL, "test0: ui_hierarchy has a knobs array");

        /* Five named levels — root (Master) plus loopA..loopD — not one flat
         * array (see forgetful.c's ui_hierarchy comment). */
        check(strstr(hier, "\"root\":{") != NULL, "test0: ui_hierarchy has a root level");
        static const char *level_keys[] = { "loopA", "loopB", "loopC", "loopD" };
        static const char *level_labels[] = { "A", "B", "C", "D" };
        for (size_t i = 0; i < 4; i++) {
            /* Labels are just the loop letter now (2026-08-25, trimmed from
             * "Loop A" etc.) — probe the level's OWN declaration
             * ("loopA":{"label":"A"...) rather than a bare "label":"A",
             * which loopA_volume's identically-renamed label would also
             * satisfy and make this check meaningless. */
            snprintf(probe, sizeof(probe), "\"%s\":{\"label\":\"%s\"", level_keys[i], level_labels[i]);
            check(strstr(hier, probe) != NULL, "test0: ui_hierarchy loop level has its own label");
        }
        /* Master's top row is now fully occupied — Route, Status, Rec,
         * Freeze — no blank slots left (2026-08-25 added Freeze into the
         * last one). */
        check(strstr(hier, "\"master_loops_overview\",\"master_record\",\"master_freeze\",\"loopA_volume\"") != NULL,
              "test0: Master page's top row is Route/Status/Rec/Freeze, no blanks");

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
            "decay_rate", "wow", "hf_loss", "hiss", "saturation", "chaos", "state", "erase"
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

    /* ---- Test 4: single-click erase starts a fade, not an instant clear
     * (Loop A). Removed double-click-confirm 2026-08-25: touch+jog-click is
     * already a deliberate two-part gesture, a second click on top was
     * redundant. Erasing a LOOPING loop now fades it out over
     * ERASE_FADE_SECONDS instead of cutting it, and memory decay is
     * suspended for the duration (see the LOOPING case's erasing guard). ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test4: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test4: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(status_of(api, inst, 'A'), "Erasing...") == 0,
              "test4: single click starts the fade immediately (Erasing...), not an instant clear");

        /* Well before the fade completes: still fading, buffer not cleared. */
        run_silence(api, inst, TEST_DECAY_FRAMES(2));
        check(strcmp(status_of(api, inst, 'A'), "Erasing...") == 0,
              "test4: still Erasing partway through the fade");

        /* A repeat press mid-fade must not restart or double-speed it —
         * confirm by running the ORIGINAL remaining budget and expecting
         * completion right on schedule, not late. */
        api->set_param(inst, "loopA_erase", "Erase!");
        run_silence(api, inst, TEST_DECAY_FRAMES(8) + BLOCK_FRAMES * 4);
        check(strcmp(status_of(api, inst, 'A'), STATUS_READY) == 0,
              "test4: fade completes on schedule (10s total) and drops to Ready, buffer cleared");

        api->destroy_instance(inst);
    }

    /* ---- Test 5: a fresh master_record press claims a loop mid-erase-fade
     * immediately, rather than waiting out the fade (Loop A). ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test5: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test5: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(status_of(api, inst, 'A'), "Erasing...") == 0, "test5: fade started");
        run_silence(api, inst, TEST_DECAY_FRAMES(1)); /* partway through the fade, nowhere near done */

        press_record(api, inst);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test5: pressing record mid-fade claims the loop immediately, not after the fade finishes");

        api->destroy_instance(inst);
    }

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
        check(n == 5, "test8: overview is exactly 5 characters (A+B, '_', C+D)");
        check(strcmp(buf, "--_--") == 0, "test8: fresh instance reads all-idle");

        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        press_record(api, inst);
        float phase = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES * 20, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test8: A is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 5 && strcmp(buf, "R-_--") == 0, "test8: overview shows A recording, rest idle");

        /* Switch routing to B: closes A into Looping at memory=1.0 - the top
         * decile, digit '9' (the decile scheme has no distinct digit for
         * "100%" versus "90-99%", by design - see the header comment). */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 5 && strcmp(buf, "9-_--") == 0, "test8: overview shows A's fresh-close decile, rest idle");

        /* B now records too: overview reflects both loops at once. */
        press_record(api, inst);
        float phase_b = 0.0f;
        run_tone(api, inst, BLOCK_FRAMES * 6, 0.5f, 880.0f, &phase_b);
        check(strcmp(status_of(api, inst, 'B'), "Recording") == 0, "test8: B is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 5 && strcmp(buf, "9R_--") == 0, "test8: overview reflects both A and B simultaneously");

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

        static const char *unit_keys[] = { "wow", "hf_loss", "hiss", "saturation", "chaos" };
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
        check(strcmp(buf, "1.000") == 0, "test10: loop volume clamps to max 1");
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

    /* ---- Test 13: erase in the two states 4/5 never exercise (both only
     * fire while Looping, where erase now fades rather than cuts). The
     * design doc says erase "works identically regardless of Input Routing
     * state"; it should also work identically regardless of the loop's OWN
     * state — Idle and Recording both still clear INSTANTLY, single click,
     * no fade (nothing to fade for Idle; Recording is an unfinished take
     * being discarded, not a loop being let go of — see the erase handler
     * comment in forgetful.c). ---- */
    {
        /* 13a: firing erase on a fresh, empty (Ready) loop must not crash or
         * misbehave — instant no-op clear, still Ready, harmlessly. */
        void *inst_idle = api->create_instance(".", NULL);
        check(inst_idle != NULL, "test12a: create_instance");
        check(strcmp(status_of(api, inst_idle, 'A'), STATUS_READY) == 0, "test12a: fresh loop is Ready");

        api->set_param(inst_idle, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst_idle, 'A'), "ERASE") == 0,
              "test12a: erase readout is always 'ERASE' (not state-aware — no arm/confirm state any more, 2026-08-25 renamed from the idle '-' spelling)");
        check(strcmp(status_of(api, inst_idle, 'A'), STATUS_READY) == 0, "test12a: still Ready, no crash/misbehavior");

        api->destroy_instance(inst_idle);

        /* 13b: firing erase while actively Recording hard-closes it
         * immediately (no fade — see the erase handler), discarding
         * whatever was captured so far. */
        void *inst_rec = api->create_instance(".", NULL);
        check(inst_rec != NULL, "test12b: create_instance");
        api->set_param(inst_rec, "input_routing", TEST_ROUTE_A);
        press_record(api, inst_rec);

        float phase = 0.0f;
        run_tone(api, inst_rec, BLOCK_FRAMES * 20, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst_rec, 'A'), "Recording") == 0, "test12b: Recording before erase");

        api->set_param(inst_rec, "loopA_erase", "Erase!");
        check(strcmp(status_of(api, inst_rec, 'A'), STATUS_READY) == 0,
              "test12b: single click drops a Recording loop straight to Ready, discarding the take, no fade");

        /* A fresh recording can start immediately afterward. */
        press_record(api, inst_rec);
        check(strcmp(status_of(api, inst_rec, 'A'), "Recording") == 0,
              "test12b: engine accepts a new recording right after an erase-during-Recording");

        api->destroy_instance(inst_rec);
    }

    /* ---- Test 14: `state` get/set round-trip — get_param("state") is used
     * for ordinary slot autosave/patch reload. Confirms: (a) live knobs
     * (decay_rate + the five flavor params, Master volumes, input_routing)
     * survive a save/restore into a FRESH instance bit-for-bit, and (b)
     * recorded content/state is deliberately NOT part of the blob — a
     * Looping source loop must restore into a Ready destination loop, not
     * some half-restored Looping-with-no-buffer state, and must not crash
     * doing so. ---- */
    {
        void *src = api->create_instance(".", NULL);
        check(src != NULL, "test13: create_instance src");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, src, &phase); /* gets A into Looping */
        check(status_is_looping(status_of(api, src, 'A')), "test13: src loop A is Looping before save");

        /* Flavor knobs are plain live values (no randomization since
         * 2026-08-25) — set to known, distinctive values so the saved blob
         * is deterministic. decay_rate=120
         * (seconds) is deliberately outside the OLD 3..60 range, to prove
         * the wider range round-trips too. */
        api->set_param(src, "loopA_decay_rate", "120");
        api->set_param(src, "loopA_wow", "0.7");
        api->set_param(src, "loopA_hf_loss", "0.6");
        api->set_param(src, "loopA_hiss", "0.3");
        api->set_param(src, "loopA_saturation", "0.9");
        api->set_param(src, "loopA_chaos", "0.1");
        api->set_param(src, "loopB_volume", "0.25");
        api->set_param(src, "input_routing", TEST_ROUTE_B);

        char state_json[1024];
        int n = api->get_param(src, "state", state_json, sizeof(state_json));
        check(n > 0, "test13: state readable");

        void *dst = api->create_instance(".", NULL);
        check(dst != NULL, "test13: create_instance dst");
        api->set_param(dst, "state", state_json);

        char buf[64];
        api->get_param(dst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "120.0") == 0, "test13: decay_rate restored");
        api->get_param(dst, "loopA_wow", buf, sizeof(buf));
        check(strcmp(buf, "0.700") == 0, "test13: wow restored");
        api->get_param(dst, "loopA_hf_loss", buf, sizeof(buf));
        check(strcmp(buf, "0.600") == 0, "test13: hf_loss restored");
        api->get_param(dst, "loopA_hiss", buf, sizeof(buf));
        check(strcmp(buf, "0.300") == 0, "test13: hiss restored");
        api->get_param(dst, "loopA_saturation", buf, sizeof(buf));
        check(strcmp(buf, "0.900") == 0, "test13: saturation restored");
        api->get_param(dst, "loopA_chaos", buf, sizeof(buf));
        check(strcmp(buf, "0.100") == 0, "test13: chaos restored");
        api->get_param(dst, "loopB_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.250") == 0, "test13: Master loop volume restored");
        api->get_param(dst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "B") == 0, "test13: input_routing restored");

        /* The part that must NOT be restored: dst's loop A comes back
         * Ready, not Looping, even though src's loop A was Looping when the
         * state was saved — recorded buffer content and playback position
         * are deliberately outside the `state` blob. */
        check(strcmp(status_of(api, dst, 'A'), STATUS_READY) == 0,
              "test13: dst loop A is Ready after restore, not carried over as Looping");

        api->destroy_instance(src);
        api->destroy_instance(dst);
    }

    /* ---- Test 15: saturation-stage passthrough. sat_amount==0 must be
     * exact identity — true both at the Warmth/Drive knob's literal minimum
     * (applied_saturation chases toward 0, so it stays 0) AND for a
     * freshly-closed loop (applied_saturation starts at 0.0 regardless of
     * the knob's setting, until the chase has had time to move it). Uses a
     * constant-value recording (not a sine tone) so the recorded content is
     * known exactly, and drives every other chase-scaled stage
     * (wow/hf_loss/hiss/chaos) to zero — the saturation stage's output is
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
            api->set_param(inst, "loopA_hf_loss", "0");
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

            /* Expected: filt_l == SAMPLE_F exactly (hf_loss=0 means
             * applied_hf_loss's target and chase both stay 0, so the Darken
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
            api->set_param(inst, "loopA_hf_loss", "0");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_saturation", "1");
            api->set_param(inst, "loopA_volume", "1");

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

    /* ---- Test 16: turning a flavor knob live GLIDES to the new target,
     * it does not snap. Same constant-value / dry-recovery technique as
     * Test 14: saturation is 0 (and every other chase-scaled stage is 0)
     * for the first second of a decay_rate=3s loop, so applied_saturation
     * has settled at exactly 0. The knob is then turned to max — if the
     * knob write itself moved applied_saturation, the very next sample
     * would already show full-drive output; instead it must still read as
     * identity, because only the CHASE TARGET moved. A further second of
     * decay must then show the output has audibly moved away from
     * identity, proving the chase is actually advancing toward the new
     * target rather than being stuck. ---- */
    {
        const int16_t CONST_VALUE = 16000;
        const float SAMPLE_F = (float)CONST_VALUE / 32768.0f;

        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test16: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3");
        record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
        check(status_is_looping(status_of(api, inst, 'A')), "test16: Looping after buffer-full close");

        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_hf_loss", "0");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_volume", "1");

        run_silence(api, inst, TEST_DECAY_FRAMES(1));

        /* Turn Warmth to max NOW — applied_saturation is settled at 0 and
         * must stay there for the very next sample. */
        api->set_param(inst, "loopA_saturation", "1");

        int16_t buf[BLOCK_FRAMES * 2];
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);

        float expected_memory = 1.0f - 1.0f / 3.0f;
        float expected_wet = SAMPLE_F * expected_memory;
        int32_t expected_out = lroundf(expected_wet * 32767.0f);
        check(abs((int)buf[0] - (int)expected_out) <= 20 &&
              abs((int)buf[1] - (int)expected_out) <= 20,
              "test16: turning saturation to max does not snap — sample right "
              "after the knob write is still identity (within float32-accumulation "
              "tolerance, sample 0)");

        /* Let the chase run for another second at the new target (2s total
         * decay now, decay_rate=3s: memory == 1/3, applied_saturation ==
         * 1/3). Compare against a freshly-computed identity reference AT
         * THIS memory level (not the stale 1s one above) so the check is
         * about saturation actually engaging, not just memory's own further
         * decay — the two pull output in opposite directions (saturation
         * drives a mid-level constant hard toward full scale; memory keeps
         * shrinking it), and saturation's pull dominates by roughly 1800
         * LSB here, far past any decay-alone or accumulation-error
         * explanation. */
        run_silence(api, inst, TEST_DECAY_FRAMES(1));
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);

        float expected_memory_2s = 1.0f - 2.0f / 3.0f;
        int32_t identity_out_2s = lroundf(SAMPLE_F * expected_memory_2s * 32767.0f);
        check((int)buf[0] - identity_out_2s > 200,
              "test16: a second later, saturation has audibly engaged — output is "
              "well above what pure identity-at-this-memory-level would give, "
              "proving the chase is actually advancing toward the new target");

        api->destroy_instance(inst);
    }

    /* ---- Test 17: v2 flavor-knob ramp mechanic (2026-08-25) — the FIRST
     * set_param write to Warp/Darken/Hiss/VINYL since the take started jumps
     * `applied_X` straight to the new value with NO ramp at all; every write
     * after that starts a fresh ramp instead, timed to the loop's remaining
     * time-to-silence (memory*decay_rate), and does NOT jump either. Uses
     * Darken specifically: its reverb wash is a deterministic function of
     * applied_hf_loss and the loop's own (constant, known) recorded content,
     * so "is the wash clearly present" is a reliable proxy for "did
     * applied_hf_loss actually reach a large value" without needing to
     * predict exact reverb output samples. ---- */
    {
        const int16_t CONST_VALUE = 16000;

        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test17: create_instance");

        api->set_param(inst, "loopA_decay_rate", "20");
        record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
        check(status_is_looping(status_of(api, inst, 'A')), "test17: Looping after buffer-full close");

        /* Lock every other chase-scaled stage off via their own first touch
         * (each jumps to 0 instantly, same mechanic under test). */
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_hiss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_volume", "1");

        int16_t buf[BLOCK_FRAMES * 2];

        /* Age the take to roughly half gone before probing Darken. Since
         * 2026-08-27 every flavour is scaled by `age` (1 - memory), so on a
         * FRESH take the wash is zero by design and could not report on the
         * ramp mechanic at all — this test measured the wash as a proxy for
         * applied_hf_loss, and the proxy needs the loop to have decayed
         * enough for the stage to be doing something. decay_rate is 20s
         * here, so 10s leaves memory ~= 0.5. */
        run_silence(api, inst, (long)SAMPLE_RATE * 10);

        /* The un-darkened level, measured rather than derived: the content
         * is a known constant, but it is now scaled by whatever memory has
         * decayed to, so the old SAMPLE_F * 32767 identity no longer holds. */
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        int32_t identity = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int32_t v = abs((int)buf[i * 2]);
            if (v > identity) identity = v;
        }
        check(identity > 1000,
              "test17: aged take still has a clearly measurable level to "
              "compare the wash against");

        /* FIRST touch on Darken: jumps applied_hf_loss to 0.9 instantly. */
        api->set_param(inst, "loopA_hf_loss", "0.9");
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        int32_t max_dev_1 = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int32_t dev = abs(abs((int)buf[i * 2]) - identity);
            if (dev > max_dev_1) max_dev_1 = dev;
        }
        check(max_dev_1 > 500,
              "test17: first touch on Darken snaps instantly — the reverb wash "
              "is already clearly audible in the very first block, not still "
              "ramping up from 0 the way an automatic chase would leave it");

        /* SECOND touch on Darken: target changes to 0.0 (fully off). This
         * must NOT jump — remaining time is ~20s here, so one more block
         * (128 samples) can only move applied_hf_loss a tiny fraction of the
         * way toward 0; the wash must still be clearly present. */
        api->set_param(inst, "loopA_hf_loss", "0.0");
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        int32_t max_dev_2 = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int32_t dev = abs(abs((int)buf[i * 2]) - identity);
            if (dev > max_dev_2) max_dev_2 = dev;
        }
        check(max_dev_2 > 500,
              "test17: second touch on Darken (to a wildly different target) "
              "does not snap either — the wash is still clearly present one "
              "block later, proving it ramps over remaining time rather than "
              "jumping on every write");

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
        api->set_param(inst, "loopA_hf_loss", "0");
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

    /* ---------------------------------------------------------------
     * test19: an erase counts DOWN, on ECHO and on the trigger itself.
     * Before 2026-08-27 both sat on whatever they read when erase fired.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test19: create_instance");
        record_full_buffer_loop_a_constant(api, inst, 8000);
        api->set_param(inst, "loopA_erase", "Erase!");

        char ov[16], tr[32];
        int seen_digit = 0, seen_erasing = 0, descends = 1, last = 10;
        int distinct = 0, prev_digit = -1;
        for (int step = 0; step < 12; step++) {
            run_silence(api, inst, (long)SAMPLE_RATE);   /* 1 second */
            api->get_param(inst, "master_loops_overview", ov, sizeof(ov));
            api->get_param(inst, "loopA_erase", tr, sizeof(tr));
            if (ov[0] >= '0' && ov[0] <= '9') {
                int d = ov[0] - '0';
                seen_digit++;
                if (d > last) descends = 0;
                last = d;
                if (d != prev_digit) { distinct++; prev_digit = d; }
            }
            if (strncmp(tr, "ERASING", 7) == 0) seen_erasing++;
        }
        /* `distinct`, not just `seen_digit`: before the fix ECHO fell
         * through to the memory decile, which is FROZEN during an erase, so
         * it read a constant "9" the whole way down. A constant trivially
         * satisfies both "is a digit" and "never increases" — only the
         * number of DISTINCT values separates a countdown from a stuck
         * character. */
        check(distinct >= 5, "test19: ECHO's digit actually moves while erasing "
                             "(not stuck on one value for the whole fade)");
        check(seen_digit >= 5, "test19: ECHO shows a digit while erasing");
        check(descends, "test19: and that digit only ever counts down");
        check(seen_erasing >= 5, "test19: the erase trigger reads ERASING n meanwhile");
        api->get_param(inst, "master_loops_overview", ov, sizeof(ov));
        api->get_param(inst, "loopA_erase", tr, sizeof(tr));
        check(ov[0] == '-', "test19: ECHO reaches '-' once the loop is gone");
        check(strcmp(tr, "ERASE") == 0, "test19: trigger returns to ERASE once done");

        api->destroy_instance(inst);
    }

    /* ---------------------------------------------------------------
     * test20: the flavours arrive as the take ages, they are not on from
     * the first pass. Hiss is the clearest case to measure because it is
     * the only one that is not part of the music: it is deliberately kept
     * OUT of the memory multiply, so a loop that has decayed to near
     * nothing should still be making noise, where before it faded out
     * along with everything else.
     * --------------------------------------------------------------- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test20: create_instance");
        api->set_param(inst, "loopA_decay_rate", "20");
        record_full_buffer_loop_a_constant(api, inst, 16000);
        api->set_param(inst, "loopA_hf_loss", "0");
        api->set_param(inst, "loopA_chaos", "0");
        api->set_param(inst, "loopA_wow", "0");
        api->set_param(inst, "loopA_saturation", "0");
        api->set_param(inst, "loopA_volume", "1");
        api->set_param(inst, "loopA_hiss", "1");      /* first touch: snaps */

        int16_t buf[BLOCK_FRAMES * 2];
        /* Deviation from the take's own constant level IS the hiss, since
         * every other stage is off and the content is DC. */
        int32_t fresh_noise = 0, level_fresh = 0;
        fill_silence(buf, BLOCK_FRAMES);
        api->process_block(inst, buf, BLOCK_FRAMES);
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int32_t v = abs((int)buf[i * 2]);
            if (v > level_fresh) level_fresh = v;
        }
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            int32_t d = abs(abs((int)buf[i * 2]) - level_fresh);
            if (d > fresh_noise) fresh_noise = d;
        }

        /* now let it decay almost all the way */
        run_silence(api, inst, (long)SAMPLE_RATE * 19);
        int32_t old_peak = 0;
        for (int b = 0; b < 8; b++) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                int32_t v = abs((int)buf[i * 2]);
                if (v > old_peak) old_peak = v;
            }
        }

        check(fresh_noise < 60,
              "test20: Hiss at maximum is essentially silent on a FRESH take "
              "(the knob sets where the loop ends up, not how it sounds now)");
        check(old_peak > 200,
              "test20: and a nearly-gone take is still audibly hissing, "
              "because hiss is outside the memory multiply — before this it "
              "faded away with the music it was supposed to outlive");

        api->destroy_instance(inst);
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: forgetful LoopEngine bench test "
           "(chain_params shape, manual record start/stop, decay timing, "
           "continuous decay, single-click erase fade-out, routing, "
           "status-line word buckets, Loops Overview format, too-short blip "
           "discard, parameter clamping, extreme "
           "decay_rate, erase during Recording/Idle, state round-trip, "
           "saturation passthrough, master_freeze, flavor-knob chase glide, "
           "overdub toggle, v2 flavor-ramp first-touch/second-touch)\n");
    return 0;
}
