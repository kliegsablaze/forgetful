# Forgetful

One live input, sent by hand into whichever of four tape memories you
choose, each one already forgetting itself the moment you move on -
drifting out of tune, hissing, breaking up, until it's gone.
Mix the four together and you're performing with your own recent past, a
little more blurred each time you glance back at it.

A [Schwung](https://github.com/charlesvestal/schwung) `audio_fx` module for
Ableton Move.

**[Read the manual →](https://kliegsablaze.github.io/forgetful/)** — every
control on all seven pages, walked through on interactive reconstructions
of the Move's own display.

See [DESIGN.md](DESIGN.md) for the full interaction model, DSP architecture,
and design history.

## Installing

Forgetful is in the Schwung module catalog, so **Schwung Manager installs
and updates it for you**. Open `http://move.local:7700`, find Forgetful in
the Module Store and install it; new releases show up there within a few
minutes of being tagged.

Needs **Schwung 0.12.1 or newer**.

### Installing by hand

For development, or on a Move with no network. Download
**[forgetful-module.tar.gz](https://github.com/kliegsablaze/forgetful/releases/latest/download/forgetful-module.tar.gz)**
([all releases](https://github.com/kliegsablaze/forgetful/releases)) and
unpack it into the `audio_fx` module directory:

```bash
scp forgetful-module.tar.gz ableton@move.local:/data/UserData/
ssh ableton@move.local 'set -e
  D=/data/UserData/schwung/modules/audio_fx/forgetful
  mkdir -p "$D" /data/UserData/.fg-stage
  tar xzf /data/UserData/forgetful-module.tar.gz -C /data/UserData/.fg-stage
  mv -f /data/UserData/.fg-stage/forgetful/forgetful.so "$D/forgetful.so"
  mv -f /data/UserData/.fg-stage/forgetful/module.json "$D/module.json"
  rm -rf /data/UserData/.fg-stage /data/UserData/forgetful-module.tar.gz'
```

It unpacks to a staging directory and *moves* the files into place rather
than extracting straight over them. That matters if you are replacing an
install that is currently loaded in a slot: writing over a `.so` that a
running process has mapped truncates the file under the live mapping, and
the next page fault takes Move's audio process down with it. A `mv` within
`/data` is an atomic rename, so the running instance keeps its old mapping
and simply carries on with the old code until you reload the module.

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
connected Move over SSH, staging and atomically renaming so it is safe to
run against a loaded slot. For normal use, install from the Module Store —
see [Installing](#installing) above.

## Releasing

1. Bump `version` in `src/module.json`.
2. `git commit`, then `git tag vX.Y.Z && git push origin main --tags`.
3. GitHub Actions cross-compiles, creates the release, and updates
   `release.json` on `main` automatically.
