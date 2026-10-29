# 0.5.6 Play: probe notes

Checked on Linux by reading the installed game's strings (`bin/linux64/engine.so`, `launcher.so`) and
`p2ce.sh` / `p2ce.bat`. The game was not launched: this host's display belongs to the owner.

## Launch arguments

- `p2ce.sh` changes into the install directory, sets `LD_LIBRARY_PATH` and `LD_PRELOAD`, appends
  `-game p2ce` to its own arguments and runs `./bin/linux64/p2ce "$@"`. Our `-game <gamedir>` comes after
  it, so the later value wins. `p2ce.bat` is `start "" ".\bin\win64\p2ce.exe" %*` (Windows, unverified here).
- The engine contains the literal `+map` (the devshots help text) and no `-map`. `+map <name>` is the console
  command taken from the command line, so the default arguments are
  `-dev -console -allowdebug -game $gamedir +map $file`, as the built-in profiles already had. D9 wrote
  `-map`; that stays unproven, and a profile can change the arguments.
- `-campaign` exists in the engine; its value format belongs to 0.5.7.

## Console log

- `-condebug` is a known engine flag and the engine names `console.log` and the error
  `Could not create console.log in folder '%s'!`. The log goes to `<gamedir>/console.log`.
  Apex always adds `-condebug` to the game step.
- `-conclearlog` clears it at start; without it the file is appended to. The Game log view therefore starts
  at the file's size at launch and rereads from the start if the file shrinks.

## Game already running

- `launcher.so` / `engine.so` contain `Only one instance of the game can be running at one time.` and a
  `-multirun` flag. A second launch is refused by the game, so *Reload map* does not change the map of a
  running game. Apex does not track the process or inject console commands (D9); the owner closes the game
  or adds `-multirun` to the profile's game arguments.

## Decisions

- The game is started with `ProcessRunner::startDetached` (a new session, no pipes, no job object): the exit
  code is ignored, and cancelling or closing Apex does not stop it.
- *Compile and Run* forces the game step on; plain *Compile* follows the profile's *run game after* option
  (new checkbox in the profile editor; built-in profiles start with it off).
- *Run Game* needs an existing `<gamedir>/maps/<name>.bsp` and says so otherwise.
- Keys: Hammer preset F5 (Compile and Run) and Ctrl+F5 (Run Game); Blender preset F12 and Ctrl+F12.

## Still to do

- The owner confirmed real playtests on current commit `1f89631` on 2026-10-28. Platforms were not specified; this clears the manual-playtest item without asserting an unreported platform.
- Native Windows lookup now follows the shipped launcher: `bin/win64/p2ce.exe`.
