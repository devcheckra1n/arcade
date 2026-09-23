# arcade

a single page game launcher on top of [emulatorjs](https://emulatorjs.org). games open in an about:blank tab, controls can be rebound per system (keyboard and mouse), save states can be kept, imported and exported, and the library is encrypted so the repo only ever holds ciphertext.

## adding games

roms go in `roms/`, which is gitignored and never leaves the machine:

```
roms/<system>/<Game Name>.<ext>
roms/<system>/<Game Name>.png      optional cover, same name as the game
roms/bios/<system>/<bios files>    e.g. roms/bios/psx/scph5501.bin
```

system folder names: nes snes n64 gb gba nds vb psx psp segaMD segaMS segaGG segaCD sega32x segaSaturn atari2600 atari7800 lynx jaguar pce pcfx ngp ws coleco 3do arcade neogeo mame dos

multi-file discs (cue + several bins) should be one `.chd` or a `.zip`.

build the packer once, then run it:

```
nix-shell -p openssl pkg-config gnumake --run make
./pack
```

it asks for the password (twice the first time), encrypts everything into `lib/` in 16 MB chunks with random names, and only re-packs files that changed. commit and push `lib/`.

the password can't be recovered. if it's lost, delete `lib/` and pack again with a new one.

## neo geo

neo geo games run on fbneo in AES (home console) mode. games go in `roms/neogeo/` under their fbneo romset names (`mslug.zip`, not `Metal Slug.zip`), and the bios goes in `roms/bios/neogeo/neogeo.zip`, kept as a zip.

the bios mode is picked in controls with neo geo selected. AES Europe/Asia needs `neo-epo.bin` inside neogeo.zip and AES Japan needs `neo-po.bin`. if the one picked is missing, the game falls back to UniBIOS 4.0 (`uni-bios_4_0.rom`), which can be switched to AES mode from its own menu (hold A+B+C while it boots).

## how it's served

- `index.html` is the whole launcher. open it from disk or paste it anywhere that runs html; nothing needs deploying
- `lib/meta.json` and the manifest come from raw.githubusercontent.com
- chunks come from jsdelivr, falling back to raw github for anything jsdelivr hasn't picked up yet
