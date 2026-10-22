<p align="center">
  <img src="assets/brand/beautype-logo-dark.png" alt="Beautype" width="120">
</p>

# Beautype

> **A desktop pet that types with you + an input method that looks good.** · Open Source · Custom skins via uploaded images · With typing pet

Typing is the most frequent little thing in our daily lives — why not make it cute? Beautype combines an **input method** and a **desktop pet** into one: while you type, a little pet on your desktop moves along with your rhythm; and the candidate window no longer has to be the boring system style — you can **upload any image and design it yourself**.

100% local: no data upload, no cloud dependency, no account, no telemetry. Your input stays on your computer.

[中文](README.md) · **English**

## ✨ Highlights

- **Fully customizable IME skin** — more than just a color: **upload a character image** or **an entire input-frame image**, or visually adjust font, size, corner radius, colors and layout — changes apply instantly.
- **A pet that types with you** — built-in Rive animated pet that reacts to your typing; hover, "getting angry", and keystroke hints all come with cute responses.
- **Pet skin library** — upload local `.riv` files, switch, delete, and restore defaults anytime — a new pet, a new mood.
- **Multiple input schemes** — Pinyin, Shuangpin (double pinyin) and Wubi, switchable with one key (`F4` scheme menu).
- **Privacy friendly** — 100% local, no telemetry, no account.

## ⌨ Input Method Features

### Multiple Input Schemes
Pinyin, Shuangpin and Wubi, switch on demand. Press **`F4`** (or `Ctrl+Shift+1`) to open the scheme menu and jump to the next scheme.

### Custom Skins (Visual Editor)
Beautype's skin system is not a simple recolor — it's a complete **visual skin editor** built into the settings panel:

| Mode | Effect | Image Spec |
|---|---|---|
| **Character image** | A character stands beside the candidate window, fixed without distortion | 256×256 PNG with transparency |
| **Input-frame image** | The whole candidate window takes the shape of your uploaded image (non-rectangular), while candidate selection, commit and paging keep working | 600×200 PNG (left/right 150px fixed, middle 300px stretchable) |
| **Default mode** | Solid color / gradient background + visual adjustment of colors, font, size, corner radius, layout | No image needed |

The two image modes can be combined or swapped: the character image overlays on the right side, the input-frame image replaces the whole rectangular UI. **The functional pipeline (candidates, commit, paging, selection) is independent of the skin mode — only the UI style changes**, so you can make it as pretty as you like without affecting typing efficiency.

Upload skins from the settings panel; changes take effect **instantly** and can be switched anytime. Format and suggested sizes: `docs/skin-format.md`.

![Beautype skin settings panel](assets/screenshots/skin-settings-panel.png)

### Shortcut Keys

| Shortcut | Function |
|---|---|
| `F4` / `Ctrl+Shift+1` | Open scheme menu / switch to the next input scheme (Pinyin · Shuangpin · Wubi) |
| `Ctrl+Shift+2` | Toggle Chinese / English input mode |
| `Ctrl+Shift+3` / `Ctrl+.` | **Toggle Chinese / English punctuation** (`。，？` ⇄ `. , ?`) |
| `Ctrl+Shift+4` | Toggle Simplified / Traditional Chinese |
| `Ctrl+Shift+5` | Toggle full-width / half-width |
| `-` / `=` (`+` also pages down) | Flip candidates (aligned with rime-ice defaults, 6 candidates per page) |
| `Space` / number keys | Commit first candidate / select a candidate |

> Smart punctuation: pressing the period right after a digit doesn't commit immediately — it waits to see whether you type another digit or something else, then outputs decimal point `.` or full stop `。` accordingly.

### Language Bar Context Menu
Right-click the Beautype icon in the language bar to open the IME menu:

| Menu Item | Description |
|---|---|
| **Switch to Chinese / English input** | Toggle input mode (same as `Ctrl+Shift+2`) |
| **Customize Skin** | Open the skin settings panel |
| **Dictionary Manager** | Open the user dictionary directory |
| **Sync User Data** | Sync user config and dictionary data |
| **Open Pet** | Launch / spawn another pet |
| **Pin Pet** | Keep the pet always on top (also brings it back if covered or off-screen) |
| **Open User Directory / Open Log Directory** | Quickly locate config and log files |
| **Redeploy** | Redeploy input scheme data |
| **Check for Updates** | Auto-update entry: fetches the latest version from GitHub Releases (with a domestic mirror fallback) |
| **Exit** | Exit the input method |

![Beautype language bar context menu](assets/screenshots/langbar-menu.png)

### More
- Horizontal candidate list, 6 candidates per page, `-`/`=` paging, number keys to select
- Chinese/English punctuation, Simplified/Traditional and full/half-width states are **remembered** across restarts

## 🐰 Desktop Pet

Built-in Rive animated pet (default skin: beautype_pet.riv) that moves along with your typing, reacts to hover, and shows real-time keystroke hints (HUD).

### Pet Context Menu

| Menu Item | Description |
|---|---|
| **Switch Scene** | Submenu: switch the current skin's animation state (idle, angry, etc., depending on the skin); current scene is checked |
| **Switch Skin** | Submenu: default skin + your skin library; current skin is checked |
| **Upload Local .riv…** | Pick a `.riv` file from disk to add as a new skin |
| **Delete Skin** | Submenu: remove skins you no longer need |
| **Restore Defaults** | Restore default skin / position / scale and all settings |
| **Show Keystrokes** | Toggle: show/hide the keystroke HUD (checked = on) |
| **Mute** | Toggle: mute pet sounds |
| **Exit Pet** | Exit the current pet |

![Beautype pet context menu](assets/screenshots/pet-context-menu.png)

Other pet abilities:
- **Multiple instances**: run several pets at the same time
- **Drag to move (left button)**: drag the pet anywhere; position is remembered and restored on restart
- **Scroll to resize**: mouse wheel resizes the pet (200–800 px)
- **Middle-click to pin**: when covered by other windows, a middle-click brings the pet back on top
- **Keystroke HUD**: shows keystroke hints while typing — modifiers (Shift/Ctrl/Alt), punctuation, digits, all covered

## 🚀 Quick Start

1. Download the latest installer (`BeautypeSetup.exe`) from the **Releases** page
2. Double-click to install, restart, and start typing
3. Try the default skin right away; **right-click the language bar** or **right-click the pet** to customize your input method and pet

> Auto-update is supported: Language Bar context menu → **Check for Updates**.

## 📚 Documentation

- Architecture & design: `docs/architecture.md`
- Config reference: `docs/config-schema.md`
- Skin format: `docs/skin-format.md`
- Key mapping / protocol: `docs/ipc-protocol.md`
- Developer guide: `docs/dev-guide.md`

## 🔨 Building

(For developers) IME: CMake build (see `docs/dev-guide.md`); pet: `tools/smoke/compile_pet.bat`; installer: NSIS (`packaging/`).

Tech stack: C++ / TSF / Rime (librime) · rive-runtime (D3D11) · NSIS.

## 📄 License

GPL-3.0 (see `LICENSE`). Third-party licenses: `THIRD-PARTY-NOTICES.md`.

## References

- [weasel](https://github.com/rime/weasel) · [Rime](https://github.com/rime) · [Metasequoia IME](https://github.com/metasequoiaime/msime-windows) · [PIME](https://github.com/EasyIME/PIME)
