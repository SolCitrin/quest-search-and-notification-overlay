# Quest Search and Notification Overlay

An SKSE plugin with a Prisma UI page for Skyrim Anniversary / Special Edition. It replaces the J journal with a
searchable quest overlay, and can show the game's top-left notifications as readable boxes that stay on screen.

## Features
- **Quest overlay (J):** every open quest with its objectives, place and journal text, grouped by type.
  - Search as you type.
  - Track (Enter), pin (P) and hide (H) quests, and show a quest on the world map (M).
  - Tabs: Pinned, Nearby, Main, Side, Misc, Completed, Hidden and Notifications.
  - Pins and hidden quests are saved in the SKSE co-save.
- **Quest boxes:** a box when a quest updates, and when you arrive at or leave a town with open objectives, or come
  near one.
- **Game notifications as boxes (optional):** the game's own top-left lines ("Gold added", "Autosaving...") become
  boxes in the same stack. The boxes wait while a menu or loading screen is open. Lines that the B612 notification
  manager handles are passed through to it.
- **Notifications tab:** a history of everything that popped up in this game session, newest first, with times.
- **Settings page** inside the overlay: position, style, scale, box time, max boxes, and more. Saved to
  `Documents/My Games/Skyrim Special Edition/SKSE/QuestSearchOverlay_Settings.ini`.

## Requirements
- Skyrim **1.6.1170** or **1.7.104** (one DLL for both). Other versions, and Skyrim VR, are not tested.
- [SKSE64](https://skse.silverlock.org/) for your game version.
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (All in One).
- [Prisma UI](https://www.nexusmods.com/skyrimspecialedition/mods/148718) (1.5.0 or newer on Skyrim 1.7).

Keyboard and mouse. Controllers are not tested.

## Building
Windows, Visual Studio 2022 (C++ desktop workload). Put the dependencies in `deps\`:

| Folder | What |
|---|---|
| `deps\CommonLibSSE-NG-v16.12.0\` | [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), tag v16.12.0 |
| `deps\directxtk-install\` | [DirectXTK](https://github.com/microsoft/DirectXTK), built and installed (CommonLibSSE-NG needs it) |
| `deps\spdlog-install\` | [spdlog](https://github.com/gabime/spdlog) 1.14.1, built and installed |
| `deps\headers\rapidcsv-8.84\` | [rapidcsv](https://github.com/d99kris/rapidcsv) 8.84 |
| `deps\prisma\PrismaUI_API.h` | the API header from the Prisma UI SDK |

Then run `powershell -File tools\Build.ps1`. The DLL and an MO2-ready folder land in `dist\`.

## Licence
Copyright (C) 2026 SolCitrin.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed WITHOUT ANY WARRANTY; see [LICENSE](LICENSE).

It is built with CommonLibSSE-NG, which is GPL-3.0-or-later with a Modding Exception and a Linking Exception (see
that project's `EXCEPTIONS.md`). Other parts it uses keep their own licences: spdlog (MIT), rapidcsv (BSD-3-Clause),
DirectXTK (MIT) and Prisma UI (its own licence).
