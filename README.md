# openmw-distantlandtool

A companion tool for the OpenMGE XE build of OpenMW
([moneymitch7/openmw](https://github.com/moneymitch7/openmw), branch `openmge-xe-actorclamp`). It makes simpler
distant versions of the models placed outside, so the distant chunks of object paging draw less.

## What it does

It reads your `openmw.cfg` and load order the way the game does, the same way `openmw-navmeshtool` does. Then, for
every static, activator and door model placed outside, it:

1. Loads the NIF and keeps its static shapes, with only the base texture.
2. Merges the shapes that look alike: same texture, material, alpha and sides.
3. Simplifies them with [meshoptimizer](https://github.com/zeux/meshoptimizer), staying within `--max-error` game
   units of the original (4 by default, less than a pixel from where distant chunks begin).
4. Writes the result as a Morrowind NIF, `<model>_dist_0.nif`, to the local data folder (`data-local`, usually
   `Documents/My Games/OpenMW/data`).

OpenMW (0.47 and later) uses a model's `_dist_0.nif` in the distant chunks of object paging, so nothing else is
needed in the game. Nearby cells (the active grid) still use the full models. The files open in NifSkope too.

Some models are left as they are:

- models with animation or other view-dependent parts
- models with night and day versions (Glow in the Dahrk)
- harvestable plants
- models it can't make any lighter
- models a mod already gives a distant version of its own (`_dist_0.nif`)

A list of what was written is kept in `distantland-files.txt` in the output folder. The next run removes those files
before writing new ones. Delete the files on that list to undo the tool completely.

`--self-test` writes a model with every property the tool writes, reads it back the way the game does, and compares
the two.

## Use

1. Copy `openmw-distantlandtool.exe` into your OpenMW folder, next to `openmw.exe`. It uses that folder's DLLs; it is
   built with the same libraries as the OpenMGE XE OpenMW build (the OpenMW 0.52 development libraries).
2. With Mod Organizer 2, add it as an executable and run it from MO2, as you run OpenMW.
3. Run it again after adding or removing mods that change the world outside.

The log is written to `distantlandtool.log`, next to `openmw.log`.

Options (`--help` lists them all):

| Option | Default | |
|---|---|---|
| `--max-error` | 4 | How far a simplified shape may stray from the model, in game units |
| `--min-part-size` | 8 | Parts smaller than this radius are left out |
| `--min-object-size` | 100 | Models smaller than this radius are skipped (distant chunks leave them out anyway) |
| `--output` | data-local | Where to write the distant models |
| `--threads` | cores - 1 | |
| `--dry-run` | | Report only, write nothing |
| `--model` | | Only these models, each read back after it is written (testing) |
| `--self-test` | | Check the NIF writer against the game's NIF loader |

## Building

The tool builds inside an OpenMW source tree and links its `components` library:

```
./add-to-openmw.sh path/to/openmw
cmake -S path/to/openmw -B build -D BUILD_OPENMW=OFF -D BUILD_OPENCS=OFF -D BUILD_LAUNCHER=OFF -D BUILD_WIZARD=OFF
cmake --build build --target openmw-distantlandtool
```

GitHub Actions builds the Windows exe against the fork with the same prebuilt dependencies as its own Windows build
(see `.github/workflows/windows.yml`). The `openmw-ref` input picks the branch, tag or commit to build against.

## Licence

GPL-3.0, as OpenMW (see `LICENSE`). meshoptimizer is MIT licensed (see `extern/meshoptimizer/LICENSE.md`).
