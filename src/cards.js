/*
 * cards.js — the cards that float while a Forgetful knob is turned
 * (Schwung 1.2 `card_script`).
 *
 * A card answers a different question from a cell. The cell says the knob is
 * at 0.6; the card says what 0.6 MEANS. It is raised while the knob is held
 * or was just turned and is gone on release — no timer of its own, no input
 * of its own, not modal. The knob underneath keeps working exactly as it
 * would with no card declared.
 *
 * SEPARATE FROM canvas.js ON PURPOSE. The host evaluates this file on the
 * first touch of a declaring knob. Anything living in here is evaluated
 * during a gesture, so it holds three drawers and nothing else.
 *
 * A CARD SEES ONE PARAMETER. `o` is {w, h, name, value, raw} for the knob
 * under the finger and nothing else — there is no getParam and no access to
 * a neighbour. That is why Speed and FREQ are two cards rather than one
 * tuning picture: quite apart from the per-parameter rule, Speed lives on
 * the Main page and FREQ on the loop page, and no surface spans two pages.
 *
 * `o.raw` MAY BE NULL — the module did not answer that read. Every drawer
 * here prints "--" and stops when it does. A bar at zero is
 * indistinguishable from a genuine zero, so a read that did not answer must
 * never become a picture.
 *
 * The frame is the card's INSIDE: (0,0) is the content area's top-left, the
 * host has already drawn the border, and ctx.width/ctx.height are the
 * interior (the declared card_w/card_h less 3px of border and gap on each
 * side). Card size is declared PER PARAMETER, so an absolute coordinate is
 * wrong somewhere by construction — everything below is sized from
 * ctx.width/ctx.height.
 *
 * One strike: a drawer that throws is retired for the session and the card
 * is left as an empty frame.
 */

(function () {

    var FONT_H = 7;

    /* Internal padding. The host draws the border hard against the content
     * area, so text laid out at 0 touches it. PAD is inset from every side. */
    var PAD = 2;

    /* Name left, formatted reading right — the same reading the header
     * shows, so the card never disagrees with the strip above it. */
    function header(ctx, o) {
        var w = ctx.width - PAD * 2;
        ctx.print(PAD, PAD, String(o.name || ""), 1);
        var s = String(o.value || "");
        /* An EMPTY reading is not a zero-width print: frame_ctx counts any
         * print at x >= width as clipped, and an empty string right-aligns
         * to exactly that. Say nothing by drawing nothing. */
        var tw = ctx.textWidth(s);
        if (s && tw <= w) ctx.print(PAD + w - tw, PAD, s, 1);
    }

    function answered(o) {
        if (o.raw === null || o.raw === undefined || o.raw === "") return null;
        return o.raw;
    }

    function nothing(ctx) {
        var y = ctx.height - PAD - FONT_H;
        ctx.print(PAD, y < 0 ? 0 : y, "--", 1);
    }

    /* ------------------------------------------------------------- Age --
     *
     * 3 to 300 seconds. The number is a duration; what it MEANS is a slope.
     *
     * THE RAMP IS STRAIGHT BECAUSE THE DECAY IS. v2_process_block drains
     * memory by 1/(decay_rate * SAMPLE_RATE) per sample — linear, wall
     * clock, one rate the whole way down. An exponential curve here would
     * be a prettier picture of something the module does not do.
     *
     * The time axis is FIXED at the 300s maximum rather than scaled to the
     * current value. Scaling it would draw the identical line at every
     * setting, which is the one thing this card must not do.
     */
    globalThis.fg_age = function (ctx, o) {
        var w = ctx.width, h = ctx.height;
        if (w < 16 || h < 10) return;
        header(ctx, o);
        if (answered(o) === null) { nothing(ctx); return; }

        var n = Number(o.raw);
        if (!isFinite(n)) { nothing(ctx); return; }

        var MAX_AGE = 300;
        var x0 = PAD, cw = w - PAD * 2;
        var top = PAD + FONT_H + 2;
        var axis = h - PAD - 1;
        var bot = axis - 1;
        if (bot - top < 2) { top = axis - 3; bot = axis - 1; }
        if (top < PAD) top = PAD;

        ctx.fillRect(x0, axis, cw, 1, 1);           /* the time axis */

        var frac = n < 1 ? 1 / MAX_AGE : (n > MAX_AGE ? 1 : n / MAX_AGE);
        var xEnd = x0 + Math.round(frac * (cw - 1));
        ctx.line(x0, top, xEnd, bot, 1);            /* full memory -> gone */
        ctx.fillRect(xEnd, bot, 1, 2, 1);           /* where it is gone */
    };

    /* ------------------------------------------------------------ FREQ --
     *
     * Semitones, plus or minus an octave, on its own fast glide. What you
     * are listening for is the distance from centre while you tune this loop
     * against another, so the centre detent is what the card is built round.
     */
    globalThis.fg_freq = function (ctx, o) {
        var w = ctx.width, h = ctx.height;
        if (w < 24 || h < 14) return;
        header(ctx, o);
        if (answered(o) === null) { nothing(ctx); return; }

        var n = Number(o.raw);
        if (!isFinite(n)) { nothing(ctx); return; }

        var RANGE = 12;
        if (n < -RANGE) n = -RANGE;
        if (n > RANGE) n = RANGE;

        var mid = Math.floor((w - 1) / 2);
        var y = FONT_H + 2 + Math.floor((h - FONT_H - 2) / 2);
        var half = Math.floor((w - 1) / 2);

        ctx.fillRect(0, y, w, 1, 1);                       /* the scale */

        /* A tick per semitone, so the octave reads as twelve steps rather
         * than as a bare line. */
        for (var s = -RANGE; s <= RANGE; s++) {
            var tx = mid + Math.round((s / RANGE) * half);
            if (tx < 0 || tx >= w) continue;
            ctx.fillRect(tx, y - 1, 1, 3, 1);
        }
        ctx.fillRect(0, y - 3, 1, 7, 1);                   /* down an octave */
        ctx.fillRect(w - 1, y - 3, 1, 7, 1);               /* up an octave */
        ctx.fillRect(mid, y - 4, 1, 9, 1);                 /* the detent */

        var x = mid + Math.round((n / RANGE) * half);
        var top = y - 6 < 0 ? 0 : y - 6;
        var bh = (y + 6 >= h ? h - 1 : y + 6) - top + 1;
        ctx.fillRect(x - 1 < 0 ? 0 : x - 1, top, 3, bh, 1);
    };

})();
