# Buffer Inputs

A mod for Geometry Dash 2.2081 that simulates the next few frames ahead in the background and delays your click or release to keep you alive

## ⚠️ Cheating ⚠️

Please don't use this mod to cheat or give yourself an advantage

## How it works

When you press or release jump, it simulates a hidden player and runs it forward through the level for a short window and checks what delay will make you survive that click

This means a click that would have clipped a spike by a frame can get nudged by a tick or two automatically and make you survive

## Features

- **Trajectory-based press buffering** — looks ahead before committing to a click
- **Dual mode support** — handles both dual and two-player dual, where each player gets its own simulation
- **Rapid-click bypass for wave** — spam clicking in wave falls back to raw input so it doesn't feel weird or delayed
- **Adjustable buffer strength** — cycle through presets with a hotkey to change how far ahead it's allowed to delay

## Controls

Press **F5** while in a level to cycle through presets:

| Preset | Normal ticks | Wave ticks |
|---|---|---|
| 1 | 2 | 2 |
| 2 | 4 | 4 |
| 3 | 8 | 8 |
| 4 | 16 | 8 |
| 5 | Off | Off |

The current preset is printed to the Geode console when you switch it

## Compatibility

- **CBF** — is compatible
- **TPS Bypass** — is not compatible, keep TPS at 240

## Known limitations

- **teleport portals**: the simulation stops at teleport portals and just counts the click as "survived" from that point
- **Moving objects/triggers**: move, rotate, and toggle triggers aren't predicted, the simulation can only see the level as it is now
- **Performance**: a single click can trigger up to 25 short simulations so rapid clicking may cause fps drops

## Installation 

Go to the releases tab and download the latest .geode file. then, place that file into your GeometryDash/geode/mods folder (2.2081 WINDOWS ONLY!!!)
