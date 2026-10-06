# civnexus6-linux

Native Linux CivNexus6 and Civ VI asset cooker, packaged as an AppImage.

Download: https://github.com/Addanc7/civnexus6-linux/releases/download/v1.0.1/CivNexus6-linux-x86_64.AppImage

```bash
chmod +x CivNexus6-linux-x86_64.AppImage
./CivNexus6-linux-x86_64.AppImage
./CivNexus6-linux-x86_64.AppImage cook --mode XLP --platform Windows \
  --pantry /path/to/pantry --config Civ6.cfg file.xlp
```

Point `--pantry` at your real Civ VI pantry. The pantry is not inside the image.

## v1.0.1

`create-cn6` writes the skeleton from the CN6 (names, parents, bind pose, inverse matrices, and per-vertex bone indices) instead of leaving the template `BLANK_SKELETON`.

```bash
./CivNexus6-linux-x86_64.AppImage export-cn6 unit.fgx unit.cn6
./CivNexus6-linux-x86_64.AppImage create-cn6 unit.cn6 unit_out.fgx
./CivNexus6-linux-x86_64.AppImage info unit_out.fgx
```

A skinned round-trip keeps the full bone count. `BLANK_MESH`, `BLANK_SKELETON`, and template material names are not written as public package keys.

## Build

`civ6cook` is standalone. `civnexus6` links [opengr2](https://github.com/arves100/opengr2) (`libopengrn.a`).

```bash
make OPENGR2=/path/to/opengr2
```
