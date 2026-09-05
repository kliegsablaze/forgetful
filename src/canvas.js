/*
 * canvas.js — Forgetful's in-grid custom widgets (Schwung 1.2 `drawCell`).
 *
 * Two cells on two different pages, drawn by ONE registered kind. The host
 * calls registerWidget exactly once, with `ov.widgetKind`
 * (shadow_ui.js ensureComponentWidgets), so a module gets ONE custom kind
 * and branches inside it. `custom:fg` is that kind; the branch is the viz
 * GROUP id, which resolveViz passes through as `group.group` — null for a
 * single-cell declaration.
 *
 * There is no `draw` export and no `type: "canvas"` parameter. The widget
 * registry loads this file purely because chain_params declares a `custom:`
 * kind, so none of Forgetful's seven pages spends a knob slot to get here.
 *
 * THE RULES THIS FILE IS WRITTEN AGAINST
 *
 *   The frame is not the screen. (0,0) is the knob box's own top-left and
 *   ctx.width/ctx.height are the box's. The same widget is handed at least
 *   sixteen frame sizes across the two renderers, so every coordinate below
 *   is computed from ctx.width/ctx.height and never from a constant.
 *
 *   You are given values; you cannot read them. There is no getParam here —
 *   a read is ~2.8ms against a 1.68ms whole-page render.
 *
 *   The label is not ours. Schwung draws "ECHO", "START" and "END" itself.
 *
 *   One strike. If drawCell throws, `custom:fg` is disabled for the session
 *   and the built-in widget draws instead. That degraded page looks EXACTLY
 *   like a typo in the kind name, a failed load, and an older host — the
 *   only thing that tells them apart is debug.log. See HANDOFF.md.
 *
 * ES5-ish on purpose: this is evaluated as a plain script by the host's
 * QuickJS, not bundled, and nothing here is worth a syntax risk.
 */

(function () {

    /* A wire value that did not answer reads as null, and null must never
     * become a picture. Returns null rather than 0 so the callers have to
     * decide, instead of silently drawing a genuine-looking zero. */
    function num(v) {
        if (v === null || v === undefined || v === "") return null;
        var n = Number(v);
        return isFinite(n) ? n : null;
    }

    function clamp01(n) { return n < 0 ? 0 : (n > 1 ? 1 : n); }

    /*
     * A 3x5 bitmap font, because the glyph has to be drawn PIXEL BY PIXEL.
     *
     * ctx.print would be easier and is wrong here: it paints a glyph in one
     * colour, and this glyph is two colours at once — lit above the fill line
     * and dark below it, changing as the bar rises through it. Nothing in the
     * frame context can read back what it has drawn, so the only way to invert
     * a glyph against a moving background is to own every pixel of it.
     *
     * Only the characters loop_status_char can actually emit: '-', R, O, F and
     * 1-9. There is deliberately no '0' — the module never sends one (see the
     * 9-down-to-1 note in forgetful.c), and adding a glyph for a character
     * that cannot arrive would only make the table look more complete than the
     * contract is.
     */
    var GLYPH = {
        "-": ["...", "...", "###", "...", "..."],
        "1": [".#.", "##.", ".#.", ".#.", "###"],
        "2": ["###", "..#", "###", "#..", "###"],
        "3": ["###", "..#", "###", "..#", "###"],
        "4": ["#.#", "#.#", "###", "..#", "..#"],
        "5": ["###", "#..", "###", "..#", "###"],
        "6": ["###", "#..", "###", "#.#", "###"],
        "7": ["###", "..#", "..#", "..#", "..#"],
        "8": ["###", "#.#", "###", "#.#", "###"],
        "9": ["###", "#.#", "###", "..#", "###"],
        "R": ["##.", "#.#", "##.", "#.#", "#.#"],
        "O": ["###", "#.#", "#.#", "#.#", "###"],
        "F": ["###", "#..", "##.", "#..", "#.."]
    };

    /*
     * One loop's slot: a bordered tank that fills from the bottom, with the
     * loop's own status character floating inside it.
     *
     * TWO READINGS, TWO CHANNELS. The FILL is how much memory is left; the
     * GLYPH is what the loop is doing. They do not compete for the same
     * pixels, which is what lets the cell say "recording" and "nearly gone"
     * at the same time — the thing four bare bars could not do and four bare
     * characters could not do either.
     *
     * The glyph is drawn as a NEGATIVE: lit where the tank is dark, dark
     * where the tank is lit. So it stays readable at every level instead of
     * disappearing into the fill at the top of the travel, and the boundary
     * moving through the character is itself the reading.
     *
     * R, O and F leave the tank EMPTY rather than full. The code carries no
     * level for those three — a recording has not got one yet, and a frozen
     * loop's is held at a value this string does not report — so filling the
     * tank would be inventing a number. The glyph says what is happening; the
     * empty tank says the level is not being claimed.
     */
    function drawSlot(ctx, x, y, w, h, c) {
        /* NO BORDER. The fill and the glyph are the whole of it: the column
         * is defined by what is in it, not by a frame drawn around it. */
        var ix = x, iy = y, iw = w, ih = h;
        ctx.fillRect(ix, iy, iw, ih, 0);                  /* dark to start */

        var lvl = (c >= "1" && c <= "9") ? Number(c) / 9 : 0;
        var fh = lvl > 0 ? Math.round(lvl * ih) : 0;
        if (lvl > 0 && fh < 1) fh = 1;                    /* 1 must still show */
        var fillTop = iy + ih - fh;
        if (fh > 0) ctx.fillRect(ix, fillTop, iw, fh, 1);

        var g = GLYPH[c];
        if (!g || iw < 3 || ih < 5) return;               /* no room to letter */
        var gx = ix + Math.floor((iw - 3) / 2);
        var gy = iy + Math.floor((ih - 5) / 2);
        for (var r = 0; r < 5; r++) {
            for (var k = 0; k < 3; k++) {
                if (g[r].charAt(k) !== "#") continue;
                var py = gy + r;
                ctx.setPixel(gx + k, py, (fh > 0 && py >= fillTop) ? 0 : 1);
            }
        }
    }

    /* `<` and `>` for the loop window, in the same 3x5 the tanks use. */
    var ARROW = {
        "<": ["..#", ".#.", "#..", ".#.", "..#"],
        ">": ["#..", ".#.", "..#", ".#.", "#.."]
    };

    function stamp(ctx, g, x, y, color) {
        for (var r = 0; r < 5; r++) {
            for (var k = 0; k < 3; k++) {
                if (g[r].charAt(k) === "#") ctx.setPixel(x + k, y + r, color);
            }
        }
    }

    /* Centred text, no box. The built-in enum widget draws a bordered square;
     * these cells carry a word and nothing else, so the border was ink spent
     * on saying "this is a cell", which the grid already says. */
    function drawWord(ctx, text) {
        var w = ctx.width, h = ctx.height;
        var s = String(text === null || text === undefined ? "" : text);
        if (!s) return;
        /* SHORTEN IT OURSELVES. print truncates a string that will not fit,
         * which is the right outcome but arrives as a clipped draw — the
         * frame counts it, and a widget that trips that counter every frame
         * is indistinguishable from one that is genuinely painting outside
         * its box. Decide the truncation here so the count stays meaningful. */
        var tw = ctx.textWidth(s);
        while (s.length > 1 && tw > w) { s = s.slice(0, -1); tw = ctx.textWidth(s); }
        if (tw > w) return;
        var x = Math.floor((w - tw) / 2);
        if (x < 0) x = 0;
        var y = Math.floor((h - 5) / 2);
        if (y < 0) y = 0;
        ctx.print(x, y, s, 1);
    }

    /*
     * Speed: the rate centred, and the DIRECTION as a triangle beside it —
     * left-pointing on the left edge for reverse, right-pointing on the right
     * edge for forward.
     *
     * The sign is stripped from the text because a minus is a poor way to say
     * "the tape is running backwards"; the triangle says it as a direction,
     * which is what it is.
     */
    function drawSpeed(ctx, group, values) {
        var w = ctx.width, h = ctx.height;
        if (w < 8 || h < 5) return;
        var key = group.keys && group.keys[0];
        var raw = key ? values[key] : null;
        var lab = (raw === null || raw === undefined) ? "" : String(raw);
        if (!lab) return;

        var rev = lab.charAt(0) === "-";
        drawWord(ctx, rev ? lab.slice(1) : lab);

        var cy = Math.floor(h / 2);
        var t = 3;                                  /* half-height of the head */
        for (var i = 0; i < t; i++) {
            /* A solid triangle, one row at a time: widest at the base. */
            var run = t - i;
            var yA = cy - i, yB = cy + i;
            if (rev) {
                ctx.fillRect(0, yA, run, 1, 1);
                if (i) ctx.fillRect(0, yB, run, 1, 1);
            } else {
                ctx.fillRect(w - run, yA, run, 1, 1);
                if (i) ctx.fillRect(w - run, yB, run, 1, 1);
            }
        }
    }

    /* ------------------------------------------------------- the Main page --
     *
     * master_loops_overview. The four-character code ("7R-1") that
     * master_loops_overview_text already builds arrives here verbatim, one
     * character per loop in A/B/C/D order: '-' idle or forgotten, 'R'
     * recording, 'O' overdubbing, 'F' frozen, '1'-'9' a memory decile.
     *
     * This is the picture the module's own header comment says could not be
     * drawn — "nowhere near a 23-character string" for a cell that holds six
     * to eight characters. Four tanks need one character each.
     *
     * Nothing about the code changes: it is still access "read", still that
     * exact string, still what tests 7 and 8 pin. Only the drawing is new.
     *
     * The 9-down-to-1-never-0 scale still matters here and for the same
     * reason it always did: this widget draws its own glyphs, and '0' and 'O'
     * would be as confusable in a 3x5 font as they are in the device's.
     */
    function drawMemories(ctx, group, values) {
        var w = ctx.width, h = ctx.height;
        if (w < 7 || h < 5) return;             /* too small to say anything */

        var key = group.keys && group.keys[0];
        var raw = key ? values[key] : null;
        var code = (raw === null || raw === undefined) ? "" : String(raw);
        /* Short or absent is not four empty loops, it is no answer — so the
         * four tanks still draw, and simply carry no character. */
        var known = code.length >= 4;

        var gap = 1;
        var colw = Math.floor((w - gap * 3) / 4);
        if (colw < 1) colw = 1;

        for (var i = 0; i < 4; i++) {
            var x = i * (colw + gap);
            if (x + colw > w) break;
            drawSlot(ctx, x, 0, colw, h, known ? code.charAt(i) : "");
        }
    }

    /* ------------------------------------------------- the four loop pages --
     *
     * START and END, as the two marks they are and nothing else: a `<` where
     * the loop begins and a `>` where it ends, on black.
     *
     * The rails and the filled block are gone. They drew the window as a
     * solid object, which made two knobs look like one heavy control and cost
     * most of the cell's ink to say something the two marks say on their own.
     *
     * BOTH MARKS ARE DRAWN AT THEIR OWN KNOB'S VALUE, always, including when
     * `>` sits left of `<`. That is a real position the knobs can reach and
     * the picture reports it rather than tidying it away — see the note in
     * forgetful.c's trim block for what the DSP then plays.
     */
    function drawWindow(ctx, group, values) {
        var w = ctx.width, h = ctx.height;
        if (w < 7 || h < 5) return;

        var roles = group.roles || {};
        var a = num(values[roles.a]);
        var b = num(values[roles.b]);

        var y = Math.floor((h - 5) / 2);
        if (y < 0) y = 0;

        if (a === null && b === null) {
            ctx.print(0, y, "--", 1);          /* neither read answered */
            return;
        }

        /* Clamped so a mark at either extreme stays whole rather than losing
         * a column off the edge — the frame would clip it silently. */
        var span = w - 3;
        if (a !== null) stamp(ctx, ARROW["<"], Math.round(clamp01(a) * span), y, 1);
        if (b !== null) stamp(ctx, ARROW[">"], Math.round(clamp01(b) * span), y, 1);
    }

    var LETTER = {
        "A": ["###", "#.#", "###", "#.#", "#.#"],
        "B": ["##.", "#.#", "##.", "#.#", "##."],
        "C": ["###", "#..", "#..", "#..", "###"],
        "D": ["##.", "#.#", "#.#", "#.#", "##."]
    };

    var SPEED_MAG = { "1/4x": 0.25, "1/2x": 0.5, "1x": 1, "2x": 2 };

    /* One lap every two seconds at 1x. Not the loop's own period — the widget
     * is never told it — so this is a readable pace rather than a measurement,
     * and it is honest about that: it tracks the RATIO between the four loops,
     * which is the thing the four speed knobs are for. */
    var ORBIT_HZ_AT_1X = 0.5;

    /*
     * Per-loop orbit state, kept here because there is nowhere else to keep
     * it: the draw path is handed values, not history, and a phase that must
     * survive a speed change and stop dead on a freeze cannot be recomputed
     * from nowMs alone.
     *
     * All four advance, not just the one on screen: they are all playing, so
     * switching Send to another loop should show where THAT loop is, not
     * where the last one was.
     */
    var orbit = { t: 0, ph: [0, 0, 0, 0], lap: [0, 0, 0, 0] };

    function orbitAdvance(values, keys, nowMs, code) {
        if (typeof nowMs !== "number") return;          /* no clock, no motion */
        var dt = orbit.t ? (nowMs - orbit.t) / 1000 : 0;
        orbit.t = nowMs;
        /* A tab away and back arrives as one enormous dt; a lap is not worth
         * simulating through it. */
        if (dt <= 0 || dt > 0.5) return;

        for (var i = 0; i < 4; i++) {
            var c = code.length >= 4 ? code.charAt(i) : "-";
            if (c === "-" || c === "F") continue;       /* empty, or held */
            var lab = String(values[keys[i]] || "1x");
            var rev = lab.charAt(0) === "-";
            var mag = SPEED_MAG[rev ? lab.slice(1) : lab];
            if (!mag) mag = 1;
            var d = dt * ORBIT_HZ_AT_1X * mag * (rev ? -1 : 1);
            var ph = orbit.ph[i] + d;
            while (ph >= 1) { ph -= 1; orbit.lap[i] = (orbit.lap[i] + 1) % 2; }
            while (ph < 0)  { ph += 1; orbit.lap[i] = (orbit.lap[i] + 1) % 2; }
            orbit.ph[i] = ph;
        }
    }

    /*
     * Send: which loop is listening, drawn as that loop's own face.
     *
     * The letter sits inside a ring, and a dot runs round the inside of the
     * ring at the loop's own speed and in its own direction — so the four
     * speed knobs underneath are visible in the one cell above them.
     *
     * THE RING IS THE LAP COUNTER. On one lap the dot wipes the ring away
     * behind it; on the next it lays it back down. So the ring is not
     * decoration, it says how far through the current pass the loop is, and
     * an empty loop's unbroken ring is the one state that never moves.
     *
     * Frozen holds the dot where it is: freeze stops everything else here, so
     * it stops this.
     */
    function drawSend(ctx, group, values) {
        var w = ctx.width, h = ctx.height;
        if (w < 9 || h < 9) return drawWord(ctx, values[group.keys[0]]);

        var sel = String(values[group.keys[0]] || "A").charAt(0);
        var idx = "ABCD".indexOf(sel);
        if (idx < 0) idx = 0;

        /* Everything else this widget needs is on the same page, so it is in
         * `values` — no reads, and nothing to keep in sync. */
        var keys = ["loopA_speed", "loopB_speed", "loopC_speed", "loopD_speed"];
        var full = group.keys[0];
        var pre = full.length > "input_routing".length
                ? full.slice(0, full.length - "input_routing".length) : "";
        for (var i = 0; i < 4; i++) keys[i] = pre + keys[i];

        var ovKey = pre + "master_loops_overview";
        var ovRaw = values[ovKey];
        var code = (ovRaw === null || ovRaw === undefined) ? "" : String(ovRaw);

        var cx = Math.floor(w / 2), cy = Math.floor(h / 2);
        /* All FOUR edges, not just the near two: at an even height the centre
         * sits past the middle, so cy alone let the bottom of the ring fall a
         * row outside a 32x14 frame. */
        var r = Math.min(cx, cy, w - 1 - cx, h - 1 - cy);
        if (r > 7) r = 7;

        var c = code.length >= 4 ? code.charAt(idx) : "";
        var empty = (c === "" || c === "-");

        if (empty) {
            ctx.drawCircle(cx, cy, r, 1);               /* whole, and still */
        } else {
            orbitAdvance(values, keys, arguments.length > 3 ? arguments[3] : undefined, code);
            var deg = orbit.ph[idx] * 360;
            /* Lap 0 wipes it away ahead of the dot; lap 1 lays it back down
             * behind. Either way the gap is where the dot has been. */
            if (orbit.lap[idx] === 0) ctx.drawArc(cx, cy, r, deg, 360 - deg, 1);
            else ctx.drawArc(cx, cy, r, 0, deg, 1);

            var rad = deg * Math.PI / 180;
            var dr = r - 1;
            ctx.setPixel(cx + Math.round(Math.sin(rad) * dr),
                         cy - Math.round(Math.cos(rad) * dr), 1);
        }

        var g = LETTER[sel];
        if (g && r >= 4) stamp(ctx, g, cx - 1, cy - 2, 1);
        else drawWord(ctx, sel);
    }

    function endsWith(str, suffix) {
        return str.length >= suffix.length &&
               str.indexOf(suffix, str.length - suffix.length) !== -1;
    }

    /*
     * One loop's ECHO on its own page. Deliberately the SAME tank at the SAME
     * size as one of Main's four, rather than a big one filling the cell: the
     * two readings are the same reading, and drawing them differently would
     * make you learn it twice.
     */
    function drawOneTank(ctx, group, values) {
        var w = ctx.width, h = ctx.height;
        if (w < 5 || h < 5) return;
        var key = group.keys && group.keys[0];
        var raw = key ? values[key] : null;
        var c = (raw === null || raw === undefined) ? "" : String(raw).charAt(0);

        var colw = Math.floor((w - 3) / 4);          /* Main's column width */
        if (colw < 3) colw = 3;
        if (colw > w) colw = w;
        drawSlot(ctx, Math.floor((w - colw) / 2), 0, colw, h, c);
    }

    globalThis.canvas_overlay = {
        widgetKind: "custom:fg",

        /* No widgetNominal: nothing here is sprite art, everything is drawn
         * proportionally, so every frame size is fine. */

        drawCell: function (ctx, o) {
            if (!o || !o.group || !o.values) return;
            var g = o.group;
            if (g.group === "w") return drawWindow(ctx, g, o.values);

            /* Single-cell kinds, told apart by their key. Matched on the
             * SUFFIX, never the whole key: the host may hand these prefixed
             * by the component (fx1:...), and a literal would quietly stop
             * matching without anything else changing. */
            var k = String((g.keys && g.keys[0]) || "");
            if (endsWith(k, "master_loops_overview")) return drawMemories(ctx, g, o.values);
            if (endsWith(k, "input_routing")) return drawSend(ctx, g, o.values, o.nowMs);
            if (endsWith(k, "_speed")) return drawSpeed(ctx, g, o.values);
            if (endsWith(k, "_state")) return drawOneTank(ctx, g, o.values);
            /* glitch_kind, and anything else that is just a word. */
            return drawWord(ctx, o.values[g.keys && g.keys[0]]);
        }
    };

})();
