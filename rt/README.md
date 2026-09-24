copy of the EmulatorJS 4.2.3 data folder (`@emulatorjs/emulatorjs` on npm) plus the cores this library uses (`@emulatorjs/core-*`), served from this repo through jsdelivr. EmulatorJS is GPL-3.0, see LICENSE; each core's own license is inside its `.data` archive.

to refresh or add a core, fetch `https://cdn.jsdelivr.net/npm/@emulatorjs/core-<name>@<version>/<name>-wasm.data` (and `-legacy-wasm.data`, `reports/<name>.json`) into `cores/`, and add the name to `LOCAL_CORES` in `index.html`.
