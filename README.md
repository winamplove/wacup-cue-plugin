# ⚡WACUP CUE Sheet Support 1.0 plugin (Nov 2026)

It's been [a long time coming](https://getwacup.com/community/index.php?topic=869.0), but .CUE file support is finally available with this unofficial third-party WACUP plugin.

## Installation:
1. **Download the correct .dll for your WACUP version**
  - [x86 version](https://github.com/winamplove/wacup-cue-plugin/releases/download/1.0/in_cuesheet_x86.dll) (most common)
  - [x64 version](https://github.com/winamplove/wacup-cue-plugin/releases/download/1.0/in_cuesheet_x64.dll) (untested)
2. Put the .dll in your **/plugins** folder next to WACUP.exe
3. If you are using portable mode, go to **Preferences** ➜ **Advanced** ➜ **Portable Mode** to turn on _**allow 3rd party plug-ins**_
4. Enable the plugin at **Preferences** ➜ **Plug-ins** ➜ **Input** ➜ ☑️ check the box next to **CUE Sheet Support 1.0**

Now you can play .CUE files like it's 2013 again. Please create GitHub issues for any... uh... issues. Otherwise enjoy!

----

**AI Disclaimer:** this plugin was made with Claude Code (Opus 5.5 Medium) using 5 stepwise prompts. Here is the consolidated prompt:

> Create a plugin for WACUP music player that offers .CUE file support. When CAUP opens a .CUE file, the plugin will parse the file and treat it as a playlist for the matching .mp3 in the same folder. Each song in the .cue sheet should appear in the WACUP playlist as an individual song title. When a user selects to play one of the songs from the playlist, WACUP will seek to the correct position in the .mp3 and play. While the .mp3 is playing, the playlist will highlight the currently playing song, as it currently does with separated song files. The visualization and waveform seeker in WACUP should both function in respect to the currently playing song in the playlist, and not the .mp3 as a whole.
> 
> Visual Studio Community is installed at ___ and WACUP is installed at ___.
