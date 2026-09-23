# arcade

a single page game launcher on top of [emulatorjs](https://emulatorjs.org). games open in an about:blank tab, controls can be rebound per system (keyboard and mouse), save states can be kept, imported and exported, and the library is encrypted so the repo only ever holds ciphertext.

## adding games

roms go in `roms/`, which is gitignored and never leaves the machine:

```
roms/<system>/<Game Name>.<ext>
roms/<system>/<Game Name>.png      optional cover, same name as the game
roms/bios/<system>/<bios files>    e.g. roms/bios/psx/scph5501.bin
```

system folder names: nes snes n64 gb gba nds vb psx psp segaMD segaMS segaGG segaCD sega32x segaSaturn atari2600 atari7800 lynx jaguar pce pcecd pcfx ngp ws coleco 3do arcade neogeo mame dos

multi-file discs (cue + several bins) go in one `.zip`.

for ps1, bin/cue is recommended. the default ps1 core can't read `.chd`, so chd games run on the heavier beetle psx core instead, which can stutter on slow machines. the launcher marks them as heavy.

a `<Game>.name` file next to a rom sets the title shown in the launcher, for arcade romsets that have to keep names like `kof2002.zip`.

pc engine cd games go in `roms/pcecd/` with the system card in `roms/bios/pcecd/syscard3.pce`.

## covers

`./covers` finds box art for every game in `roms/` that doesn't have a cover yet. it pulls the free launchbox games database (no key or account needed, cached in `~/.cache/arcade` and only re-downloaded when launchbox publishes a new one), matches each rom by title, platform and region, and saves a 512 px jpeg next to the rom. `./covers -f` replaces existing ones. anything it can't match keeps the coloured initials card, or you can drop in a `<Game>.jpg` yourself.

## packing

build the tools once, then run the packer:

```
nix-shell -p openssl curl zlib pkg-config gnumake --run make
./pack
```

it asks for the password (twice the first time), encrypts everything into `lib/` in 16 MB chunks with random names, and only re-packs files that changed. commit and push `lib/`.

the password can't be recovered. if it's lost, delete `lib/` and pack again with a new one.

## neo geo

neo geo games run on fbneo in AES (home console) mode. games go in `roms/neogeo/` under their fbneo romset names (`mslug.zip`, not `Metal Slug.zip`), and the bios goes in `roms/bios/neogeo/neogeo.zip`, kept as a zip. `.neo` files (the neosd format) don't load in fbneo, and zipping one doesn't change that, so the packer skips them. fbneo needs the real romset (`garou.zip`, `kof2002.zip`), and a merged `.7z` set has everything.

the bios mode is picked in controls with neo geo selected. AES Europe/Asia needs `neo-epo.bin` inside neogeo.zip and AES Japan needs `neo-po.bin`. if the one picked is missing, the game falls back to UniBIOS 4.0 (`uni-bios_4_0.rom`), which can be switched to AES mode from its own menu (hold A+B+C while it boots).

## how it's served

- `index.html` is the whole launcher. open it from disk or paste it anywhere that runs html; nothing needs deploying
- `lib/meta.json` and the manifest come from raw.githubusercontent.com
- chunks come from jsdelivr, falling back to raw github for anything jsdelivr hasn't picked up yet
