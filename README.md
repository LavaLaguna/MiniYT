# Mini YT

A tiny, fast YouTube search and player for old Windows machines. One native `MiniYT.exe` (C++/Win32, no installer, no .NET),
with an embedded [mpv](https://mpv.io) player. On first start it downloads mpv (and, as a backup, yt-dlp) by itself.

## Features
- Search videos, playlists or both; browse channels and playlists; Home page; related videos
- Playlists of any size (continuation paging, yt-dlp backup), watched ticks that persist between sessions
- Your own playlists: hover a video and press **+ ADD TO PLAYLIST**
- Quality (360p to 1080p, audio only), speed control, full screen (double-click the picture or F11), Space = play/pause
- Drag rows up or down in the playlist panel to change their order
- **Backup**: export / import all playlists, ticks, history and settings (merge or replace)
- Portable mode: put an empty `portable.txt` next to the exe and everything lives in `MiniYT-data` beside it

## Where data lives
`%LOCALAPPDATA%\MiniYT` (`state.json` = your playlists and ticks, `mpv\` = the player). In portable mode: `MiniYT-data\` next to the exe.

## Build
Needs Visual Studio 2019/2022 with the "Desktop development with C++" workload.

* **Visual Studio:** open `MiniYT.sln`, set **Release / x64**, *Build > Build Solution*. Output: `build\MiniYT.exe`.
* **Command line:** open "x64 Native Tools Command Prompt for VS", run `build-msvc.bat`. Output: `MiniYT.exe` in this folder.
* **GitHub Actions:** push a tag such as `v1.12.0`; the workflow builds the exe and attaches it to the release.
* **MinGW-w64 (Linux/Windows):**
  `windres src/miniyt_msvc.rc -O coff -o res.o` then
  `g++ -std=c++17 -O2 -mwindows -municode -static -s src/miniyt.cpp res.o -o MiniYT.exe -lwinhttp -lgdiplus -lgdi32 -luser32 -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lshell32 -ladvapi32 -lmsimg32 -luuid`

## Notes
Uses YouTube's public web endpoints; they change from time to time, so a YouTube update can break search until the app is updated.
Not affiliated with YouTube or Google. For personal use.
