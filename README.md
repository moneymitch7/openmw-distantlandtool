# openmw-distantlandtool

A companion tool for OpenMW builds from [moneymitch7/openmw](https://github.com/moneymitch7/openmw): the opt-in build
(branch `openmge-shadows-only`, upstream OpenMW with a few opt-in features) and the OpenMGE XE fork (branch
`openmge-xe-actorclamp`). It makes simpler distant versions of the models placed outside, so the distant chunks of
object paging draw less. The models it writes work in any OpenMW since 0.47.

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

For the OpenMGE XE fork it also bakes the **distant texture set**, `distantland/textures.bin` in the same folder:
every base texture those distant models use, shrunk to one size (`--texture-size`, 128 by default in the fork's exe,
0 (none) in the opt-in build's, which doesn't read it), with full mipmaps and compressed as DXT5.
The smaller levels keep the coverage of alpha tested textures, so foliage doesn't thin out from afar. The OpenMGE XE
build (from the commit "Distant land stage 2") loads the set into texture arrays and draws each distant chunk's
objects that use only those textures as a few merged shapes, instead of a draw call per texture. Other OpenMW
builds ignore the file. `[Terrain] object paging distant texture set = false` in `settings.cfg` turns it off.

A list of what was written is kept in `distantland-files.txt` in the output folder. The next run removes those files
before writing new ones. `--clean` removes them (and the list) without writing new ones, undoing the tool completely.

`--self-test` writes a model with every property the tool writes, reads it back the way the game does, and compares
the two.

## Use

1. Copy `openmw-distantlandtool.exe` into your OpenMW folder, next to `openmw.exe`. It uses that folder's DLLs, so take
   the exe built for that build: `openmw-distantlandtool-opt-in-<commit>` for the opt-in build,
   `openmw-distantlandtool-fork-<commit>` for the OpenMGE XE fork (each is built with that branch's libraries).
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
| `--texture-size` | 128 (fork exe), 0 (opt-in exe) | Size of the distant texture set's textures (16 to 1024); 0 bakes no set |
| `--threads` | cores - 1 | |
| `--clean` | | Remove the last run's distant models and stop (to compare with and without) |
| `--dry-run` | | Report only, write nothing |
| `--model` | | Only these models, each read back after it is written (testing) |
| `--self-test` | | Check the NIF writer and the texture compression against the game's loaders |

## Building

The tool builds inside an OpenMW source tree and links its `components` library:

```
./add-to-openmw.sh path/to/openmw
cmake -S path/to/openmw -B build -D BUILD_OPENMW=OFF -D BUILD_OPENCS=OFF -D BUILD_LAUNCHER=OFF -D BUILD_WIZARD=OFF
cmake --build build --target openmw-distantlandtool
```

GitHub Actions builds a Windows exe against each branch with the same prebuilt dependencies as that branch's own
Windows build (see `.github/workflows/windows.yml`). Run by hand, the `openmw-ref` input builds against one branch,
tag or commit instead. `-D DISTANTLANDTOOL_TEXTURE_SET=OFF` makes `--texture-size` default to 0.

## Licence

GPL-3.0, as OpenMW (see `LICENSE`). meshoptimizer is MIT licensed (see `extern/meshoptimizer/LICENSE.md`).
