# Ferns FPS Unlocker

Removes the 60 FPS cap from the Ferns 2021 Roblox revival client.

## Requirements

- Python 3.x
- Ferns running

## Usage

1. Open Ferns and load into a game
2. Run `ferns_fps_unlocker.py` as Administrator
3. Done — the cap is lifted instantly

You can also double-click `Launch.bat` if you have the script folder, it handles the admin elevation automatically.

## How it works

Scans the game's writable memory for the frame time values used by the engine's task scheduler (1/60 and 1/30 as both float and double) and replaces them with a near-zero value, effectively removing the frame cap.

## Notes

- Run **after** the game has fully loaded into a place, not on the menu
- Re-run after each game join
- Must be run as Administrator
