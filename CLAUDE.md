# Forgetful — working notes for Claude

Four independent tape memories on one live input, each decaying on its own
clock. A Schwung `audio_fx` module for Ableton Move, written in C.

**Read `HANDOFF.md` before changing anything.** It carries the traps that cost
real time to find, and most of them look like something else. `DESIGN.md` is
the interaction model and the DSP rationale. This file is only the short list
of things you need in the first five minutes.

## The two rules that matter most

**Never `scp` onto the live `.so` path on the device.** `scp` opens the
destination `O_TRUNC`; the shim has that file `dlopen`'d whenever the module
sits in a chain slot, so it truncates a live mapping and takes Move's whole
audio process down. `scripts/install.sh` stages beside the target and renames.
Use it; do not hand-roll a copy.

**Mutate the code and confirm the test fails.** Every behavioural test here has
been verified that way, and the repeated failure mode in this project is a test
that passes against visibly broken code. When you mutate, check the EXIT CODE,
not just for a `FAIL` line: `-Werror` turns some mutations into compile errors
and a grep for `^FAIL` then finds nothing, which reads as "not caught".

## Layout

| Path | What |
|---|---|
| `src/dsp/forgetful.c` | The entire module — DSP, state machine, `chain_params`, `ui_hierarchy` |
| `src/canvas.js` | In-grid custom widgets (Schwung 1.2 `drawCell`), one registered kind, `custom:fg` |
| `src/cards.js` | Cards raised while a knob is turned (`card_script`) |
| `src/help.json` | On-device help. **Hard 20-character line limit, and it is invisible** — validate with a script |
| `src/module.json` | id, version, `component_type: audio_fx` |
| `tests/test_forgetful_loopengine.c` | Black-box bench driving the real v2 plugin API |
| `docs/index.html` | The manual, with hand-rolled reconstructions of the Move screen |

## Commands

```bash
bash tests/run.sh        # the suite; exits non-zero on failure
./scripts/install.sh     # build (via Docker) + deploy to move.local + VERIFY the running inode
./scripts/release.sh     # the ONLY thing that reaches Schwung Manager. Never a side effect.
```

Bump the patch version in `src/module.json` as you go — the Manager reads the
version off the device, so an unbumped deploy makes it report a version that
is not what is running.

## The UI surfaces (Schwung 1.2)

Declared in `chain_params` **in the DSP**, never only in `module.json` — the
chain host serves the contract from `get_param`.

- A module gets **one** widget kind. Everything shares `custom:fg` and
  `drawCell` branches on the key's suffix (never the whole key: it may arrive
  component-prefixed).
- **No `getParam` on any draw path.** Values arrive as an argument. Everything
  a widget needs must be on the same page, because `values` is that page.
- **`raw` may be null** — the read did not answer. Draw a word and stop; a bar
  at zero and a genuine zero are the same picture.
- **The frame is not the screen.** Size everything from `ctx.width`/`ctx.height`.
- **Failure is silent.** A typo in the kind, a `canvas.js` that failed to load,
  an older host, and a widget retired after throwing all produce the same
  screen: a correct page drawn with the built-in widgets. `debug.log` is the
  only thing that tells them apart:
  ```bash
  ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'
  ssh ableton@move.local 'grep -i "widgets:" /data/UserData/schwung/debug.log | tail -5'
  ```
  Want: `widgets: forgetful declares a custom viz kind; loading canvas.js`

## Verifying UI work without the device

The device is slow to iterate on and its failures are invisible. Do this first:

1. **Drawers** — run them against Schwung's own `frame_ctx.mjs` at every frame
   size, asserting no throw, nothing clipped, nothing outside the frame. A
   widget is handed at least sixteen different frame sizes.
2. **The declaration** — drive Schwung's `resolveViz` against the live
   contract, with the widget registered AND unregistered. It answers the one
   question the pictures cannot: whether the host claims those cells at all.
3. **The page** — `tools/param-pages/preview.mjs` in the schwung checkout, with
   the contract injected into `tests/fixtures/module-contracts.json`. It does
   **not** load a module's `canvas.js` on its own; pre-register the widget (see
   HANDOFF's Testing section for the snippet), or it renders the built-ins and
   tells you nothing.

## Buffers that must grow together

`chain_params` is built into `char json[12288]` in `forgetful.c` and read into
three separate fixture buffers in the test file. `get_param` returns -1 rather
than truncating, so an undersized fixture fails as "chain_params readable" —
a message that says nothing about the real problem. Grow all four together.

## Working preferences

- **Do not update `docs/index.html` during UI iteration.** The manual redraws
  every widget with its own canvas renderer, so each visual change means
  writing the drawing code twice. Let it go stale, say so, and sync it as one
  explicit step before the commit that gets pushed. Same for `help.json` and
  the prose in `DESIGN.md` / `README.md`.
- Deploy to the device for judgement by eye; the sound and the look have only
  ever been judged there.
