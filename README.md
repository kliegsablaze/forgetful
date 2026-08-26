# Forgetful

One live input, sent by hand into whichever of four tape memories you
choose, each one already forgetting itself the moment you move on -
drifting out of tune, going dark, hissing, breaking up, until it's gone.
Mix the four together and you're performing with your own recent past, a
little more blurred each time you glance back at it.

A [Schwung](https://github.com/charlesvestal/schwung) `audio_fx` module for
Ableton Move.

**[Read the manual →](https://kliegsablaze.github.io/forgetful/)** — every
control on both pages, walked through on interactive reconstructions of the
Move's own display.

See [DESIGN.md](DESIGN.md) for the full interaction model, DSP architecture,
and design history.

## Installing

The easy way is [Schwung Manager](https://github.com/charlesvestal/schwung)
(the web UI at `http://move.local:7700`) — Forgetful is in the module
catalog, so it appears in the Module Store and updates itself from there.

To install it by hand instead, download
**[forgetful-module.tar.gz](https://github.com/kliegsablaze/forgetful/releases/latest/download/forgetful-module.tar.gz)**
([all releases](https://github.com/kliegsablaze/forgetful/releases)) and
unpack it into the `audio_fx` module directory on the Move:

```bash
scp forgetful-module.tar.gz ableton@move.local:/data/UserData/
ssh ableton@move.local \
  'mkdir -p /data/UserData/schwung/modules/audio_fx && \
   tar xzf /data/UserData/forgetful-module.tar.gz \
       -C /data/UserData/schwung/modules/audio_fx && \
   rm /data/UserData/forgetful-module.tar.gz'
```

Then rescan modules (or restart Schwung) and Forgetful shows up in any
Signal Chain fx slot.

## Building

Requires Docker (for ARM64 cross-compilation) or a native aarch64 toolchain.

```bash
./scripts/build.sh
```

Produces `dist/forgetful-module.tar.gz`.

To build natively on an aarch64 host instead of cross-compiling:

```bash
CROSS_PREFIX= ./scripts/build.sh
```

## Testing

```bash
bash tests/run.sh
```

Black-box bench test driving the module's public v2 plugin API
(`create_instance`/`set_param`/`get_param`/`process_block`) exactly as
Schwung's chain host would.

## Installing for local development

```bash
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Builds (if needed) and copies `forgetful.so` + `module.json` straight to a
connected Move over SSH. For normal use, install via
[schwung-manager](https://github.com/charlesvestal/schwung) instead.

## Releasing

1. Bump `version` in `src/module.json`.
2. `git commit`, then `git tag vX.Y.Z && git push origin main --tags`.
3. GitHub Actions cross-compiles, creates the release, and updates
   `release.json` on `main` automatically.
