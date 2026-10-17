# Third-party notices

AutoHDR for SteamOS (HDR-Ket) is MIT licensed (see `LICENSE`). It includes or
is derived from the following third-party work. Full license texts are in
`LICENSES/`.

## Shipped in the Decky plugin

| Component | Author | License | Where | License text |
|---|---|---|---|---|
| [@decky/api](https://github.com/SteamDeckHomebrew/decky-frontend-lib) | Steam Deck Homebrew | LGPL-2.1 | The small loader-connection module bundled into `dist/index.js`. Its source is available at the link, unmodified. | `LICENSES/LGPL-2.1-decky-api.txt` |
| [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) | Kazuhiko Arase | MIT | Bundled into `dist/index.js` (QR code for the phone address). | `LICENSES/MIT-qrcode-generator.txt` |
| [react-icons](https://github.com/react-icons/react-icons) | react-icons contributors | MIT | The plugin's HDR icon, bundled into `dist/index.js`. | `LICENSES/MIT-react-icons.txt` |
| [Material Design Icons](https://github.com/google/material-design-icons) (`hdr_on`) | Google | Apache-2.0 | The plugin's icon, via react-icons. | `LICENSES/Apache-2.0-material-design-icons.txt` |

`@decky/ui` is provided by Decky Loader at runtime and is not bundled.

## Code and approaches this project is derived from

| Project | Author | License | What it contributed | License text |
|---|---|---|---|---|
| [vkBasalt](https://github.com/DadSchoorse/vkBasalt) | Georg Lehmann and contributors | Zlib | The structure of the Vulkan layer: instance/device chaining, dispatch tables and the present hook (`src/layer.cpp`, `src/present.cpp`). | `LICENSES/Zlib-vkBasalt.txt` |
| [PumboAutoHDR](https://github.com/Filoppi/PumboAutoHDR) | Filippo Tarpini | MIT | The luminance-based, additive highlight expansion above a shoulder (`src/shaders/itm.comp`). | `LICENSES/MIT-PumboAutoHDR.txt` |
| [decky-plugin-template](https://github.com/SteamDeckHomebrew/decky-plugin-template) | Steam Deck Homebrew | BSD-3-Clause | The plugin's build setup (`decky/rollup.config.js`, `decky/tsconfig.json`, `decky/package.json`). | `LICENSES/BSD-3-Clause-decky-plugin-template.txt` |

## Published standards implemented

- ITU-R BT.2390: the EETF used for tone mapping above the display peak.
- ITU-R BT.2020 / BT.2087: BT.709 ↔ BT.2020 color conversion.
- SMPTE ST 2084: the PQ transfer function for HDR10 output.
- CTA-861: EDID HDR static metadata (display peak brightness).
