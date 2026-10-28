# Changelog

## Unreleased

### Elden Ring 1.17.2

- Added support for Elden Ring App Ver. 1.17.2 (`eldenring.exe` 2.7.2.0).
  The native bridge detects the build at startup and uses its address table;
  App Ver. 1.17.1 (`eldenring.exe` 2.7.1.0) remains supported.

### ULTRAKILL guest extensions

- The native bridge implements the optional extensions of the
  [ULTRAKILL Crossover Bridge](https://github.com/Themetralla3000/ultrakill-crossover-bridge)
  protocol (`bridge_protocol_ext.h`, its docs/protocol.md section 8). They only
  turn on while an ULTRAKILL guest publishes its request block, so Minecraft
  behaves as before.
  - Host-owned health: the Tarnished's real HP is V1's health. Dash and hurt
    i-frames undo Elden Ring hits, a punch parries a nearby attacker, ULTRAKILL
    heals heal the Tarnished, and a lethal hit kills V1 first.
  - Weapon progression: bosses defeated from the global boss flags
    (9100-9299), with the character as the run seed.
  - Stat ratios: Dexterity drives attack speed, max FP alt-fire recharge, max
    stamina dash recharge.
  - Stat damage: ULTRAKILL damage scales with the best weapon's attack rating.
  - Menu hand-off: while a menu blocks Elden Ring's actions (Site of Grace,
    talking, item pop-ups), the guest hands input to Elden Ring.
  - Settings in `<bridge dir>\er-bridge.ini` (`[ULTRAKILL]`), written with
    defaults on first run.

## 0.3.0 — 2026-10-06

### Terrain collision

- Improved terrain collision and reduced generation overhead.
- Added predictive refinement of doorways, stairs and narrow passages while
  preserving real walls, floors, ceilings and previously verified clearance.

### World commands

- Synchronized Minecraft weather and time commands with Elden Ring.
- Preserved world settings and native scripted weather, with consistent rain
  in bridge worlds.

### Windows overlay

- Improved overlay focus, Alt+Tab and F8 switching.
- Restored the native cursor in Minecraft menus and hid the taskbar during play.
- Kept loading screens transparent and recovered frame transfer after interruptions.

## 0.2.0 — 2026-10-03

Major update to the experimental Windows bridge.

### Movement and terrain

- Added native moving-support tracking, client travel prediction and moving
  floor collision for boarding lifts and platforms before passenger contact.
- Added handling for ascent, descent, delayed poses, jumping and landing,
  gently sloped platforms and partial contact at an edge.
- Added detailed terrain collision for narrow arches and angled passages,
  including corner, one-sided wall, body-clearance and floor-safety checks.
- Added predictive terrain prefetch and refresh of nearby stale samples.
- Preserved refined shapes across unchanged samples and invalidated pending
  terrain work after door changes.
- Checked native body collision before filling columns under overhead surfaces.
- Corrected the hidden Tarnished's facing, including assisted door interactions.

### Windows rendering and diagnostics

- Replaced external fullscreen-mod dependencies with bridge-managed borderless
  window layout and separate Windows input/transparency modes.
- Kept Minecraft windowed at launch and during overlay operation.
- Suspended camera publishing during host handoff and retired pending GPU
  captures while compositing is paused.
- Prevented a held F8 key from causing a second switch during focus handoff.
- Added native fault recording with context, thread stacks and minidumps.
- Added seven regression scripts covering terrain, platforms, facing,
  frame switching and crash recording.

### Project information

- Reworked the README with a gameplay image, feature overview, current controls,
  requirements and known limitations.
- Added Windows setup, update, removal and developer documentation.
- Updated Fabric project metadata and removed obsolete dependency notices.

## 0.1.0 — Initial Windows port

- Adapted the upstream Elden Ring bridge for Windows with native build,
  installation, launch and restoration scripts.
- Included Minecraft/Fabric integration, host compositing, terrain, interactions
  and combat, with project licenses and third-party notices.
