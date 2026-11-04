# Ports from newer Azahar

Claudhar is built on Azahar 2126.2. Changes taken from later Azahar versions are listed
here, with what was left out and why, so that moving to a newer Azahar later knows what is
already in. Each port is a cherry-pick that keeps the upstream author, with the conflicts
and omissions explained in its commit message.

Ported files take upstream's license header for that file (GPLv3 or later where Azahar has
relicensed it) and a notice line saying the Claudhar Project ported the change.

| Upstream change | What it does | In Claudhar | Left out |
| --- | --- | --- | --- |
| [azahar-emu/azahar#1856](https://github.com/azahar-emu/azahar/pull/1856) (`49a1e8c`, 2127.0 Alpha 4) | Android settings refactor: values live in a `CitraSettings` instance with global values and per-game overrides kept apart | 0.0.3-alpha | The FastInterp CPU interpreter switch and the Anime4K-on-Vulkan dialog, whose features 2126.2 does not have |
| [azahar-emu/azahar#2698](https://github.com/azahar-emu/azahar/pull/2698) (`cd96507`, 2127.0 Alpha 4) | Per-application settings: every `config.ini` setting can be overridden per game, from the game's long-press menu or the in-game menu | 0.0.3-alpha | The Anime4K dialog and its strings; the input-latency changes around it (OpenGL present thread, Choreographer removal); battery and turbo-hold settings |
| Shortcut relaunch fixes ([#2656](https://github.com/azahar-emu/azahar/pull/2656), [#2658](https://github.com/azahar-emu/azahar/pull/2658)) | Relaunching a running game from a home-screen shortcut, and closing a game launched from one | 0.0.3-alpha, with #2698's version of `EmulationActivity` | Nothing; they came with that file. 2126.2 set `launchedFromShortcut` but checked `launched_from_shortcut` |

Per-application settings do not cover the custom GPU driver: Azahar installs one driver
for the whole app, outside `config.ini`. Claudhar adds that itself in 0.0.4-alpha, adapted
from the owner's Uberhar build: a game's driver is picked in its Per-Application Settings
(Graphics), kept in `config/drivers.ini` and loaded just before the game starts
(`GameDrivers.kt`).
