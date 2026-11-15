# dsr-native — Dark Souls Remastered (Switch) native loader for Android

Runs the game's original ARM64 code (`exefs/main`) directly on an ARM64 Android device.


> [!IMPORTANT]
> **No game files are included for obvious reasons. You must obtain a legal copy of the Nintendo Switch version of Dark Souls: Remastered on your own, I cannot help you with this step.**

**NOTE: THE ASSISTANCE OF AI WAS HEAVILY USED DURING THIS PROJECT.**

## Status

> [!IMPORTANT]
> Mail GPU's not fully supported yet.

Early development. Expect visual bugs and minor performance issues. Base v1.0.0 and Update v1.0.3 are supported.

Tested with an AYN Thor, a OnePlus 6T, and MuMuPlayer using a 4:3 aspect ratio. Snapdragon chips should be fully supported along with Adreno 6xx, 7xx, and 8xx gpus.




## How does it run?

Tested areas have been the Undead Burg, Firelink Shrine, and the Undead Asylum. **>6GB of RAM Recommended** 

> [!IMPORTANT]
> Expect texture flickering and stuttering upon rendering in new areas. This will most likely happen the first time you load the game. Once loaded there should be no more glitching.

Runs at the Switch's default resolution of 720p. 

The game is very playable with a stable **60FPS** on a Snapdragon 8 Gen 2 or equivalent. Your mileage may vary with much older chips. Temps average around the **60c~** mark, and RAM usage is low.

I have yet to test later areas in the game so don't expect performance to be entirely perfect.

## Working features

- [x] Working cutscenes and in-game rendering
- [x] Full controller support
- [x] Working lighting
- [x] Fully working audio, including NPC voice lines and sound effects
- [x] Stable 60FPS (on my test device at least)
- [x] Mailbox frame presentation for lower input latency
- [x] On-screen touch controls
- [x] Experimental different aspect ratio support without stretching game image
- [x] User-selected game directory
- [x] Floating GUI (Hold - and + buttons for one full second) with various settings such as FPS cap, aspect ratio, frame presentation, etc..
- [x] Custom driver support
- [x] Render scale (30% - 100%) / Graphics options (shadows off/512/1024/2048, low-quality effects)
- [x] Several language options

## Roadmap

- [ ] Higher quality audio patch
- [ ] Further performance optimizations
- [ ] Support for more phone gpus and chipsets, specifically Mali GPUs




## Logs

Every launch writes a log file to the phone's shared storage:

```
Download/DSRNative/logs/dsr_YYYYMMDD_HHMMSS.txt
```

It contains device info, every runtime message, and (if the app crashes) a crash report with the
faulting address translated to `main+0x...` offsets (look these up in Ghidra with the ELF from
`tools/mkelf.py`), registers and a frame-pointer backtrace. Lines are written immediately, so the
file is complete even after a crash. The same output also goes to logcat (`adb logcat -s "DSR/*"`).






## Installing game files on the device
**Method 1**
> [!IMPORTANT]
> Requires your own prod.keys file.


Install the apk, launch it and give it permission to access files. Afterwards, select the game nsp/xci file along with its v1.0.3 update file (optional). The game will begin extracting the necessary files and place them inside "/Download/DSRNative/game/", and then boot the game from inside this folder.





**Method 2**

> [!IMPORTANT]
> You must extract the games **romfs** and **exefs** files to play the game. Method 1 already does this for you but if you prefer to do it yourself you can use a program like Ryubing to extract the files.

Install the APK, launch it and give it permission to access files to create the necessary folders, then choose a directory to search for the game files. 

Inside your game folder must be the extracted **romfs** files, along with the folders **movww**, **movjp**, and **exefs**. You must create the **exefs** folder and place your extracted **exefs** files inside.

<img width="270" height="480" alt="Screenshot_20261110_204928_Top" src="https://github.com/user-attachments/assets/1f08d681-820d-40d6-a912-261bbbfcf294" />

Folder structure should look like this.



## Known issues


- [ ] Cutscenes can sometimes still appear glitched
- [ ] Cutscenes may appear stretched or odd while playing on a different aspect ratio.


## Issues

If you encounter any issues, please submit an issue on the repo and attach log files.



## Building

**GitHub Actions (no local tools needed)** — push this repo to GitHub. Every push runs
`.github/workflows/build.yml`, which builds the debug APK (download it from the run page under
*Artifacts* → `dsr-native-debug-apk`) and runs the runtime tests on x86-64 and arm64. You can also
start a build by hand from the *Actions* tab (*Build* → *Run workflow*).
Do not commit game files: the repo only contains source code, and `.gitignore` blocks game data.

**Android Studio** — open `android/` in Android Studio (with NDK + CMake installed) and run. Or from a shell
with the Android SDK configured: `cd android && gradle assembleDebug`.
Requirements: arm64-v8a device, Android 10 (API 29) or newer.

**Host tool** (Linux): builds the same loader and HLE layer for a dry run against the real game files:

```
cmake -S . -B build-host && cmake --build build-host -j
./build-host/dsr_dryrun /path/to/DarkSoulsR --list --verbose
```

On x86 it maps/relocates/binds and prints the import coverage report. On an arm64 Linux machine `--run` executes the game the same way the APK does (useful for debugging HLE without a phone).
