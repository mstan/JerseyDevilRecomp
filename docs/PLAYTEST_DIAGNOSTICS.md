# Playtest diagnostics

Playtest Diagnostics is off by default. Enable it in the Mods menu before reproducing a problem you want to report (offline play).
Recording pauses during netplay. Input Recording
and Input Display are independent controls: hiding the display keeps recording
running. Both accepted controller inputs, analog positions and controller
presence are recorded. Normal player launches keep audio enabled.

When a problem occurs, press both stick buttons together (**L3 + R3**) to mark
an incident, then keep playing for a few seconds. Describe what you intended,
what happened, and the incident number when reporting the problem. With a
controller that has no stick buttons, note the displayed frame number instead.

Quit normally and zip the entire newest `saves/diagnostics/session-*` folder.
`diagnostics-location.txt` beside the executable identifies its absolute location.
A temporary writable directory is used if the normal location is blocked. The
folder contains inputs, starting memory cards, settings and mod choices,
rotating RAM fingerprints, and a `telemetry.csv` sidecar with host timing and
CD/render evidence. The disc and BIOS are never included. Python is optional;
playing and recording do not require Python or a debug server.

Host timing includes focus changes, pauses and pacing. A long interval alone
does not identify a CPU or GPU bottleneck. Preserve the complete session even
if a replay diverges: the input and incident evidence can still be useful.

Developers can replay a session using the shared
`psxrecomp/tools/replay_diagnostics.py` tool, which is also staged beside the
executable as `replay-diagnostics.py`. For example, from the project directory:

```powershell
python psxrecomp/tools/replay_diagnostics.py saves/diagnostics/session-123 --exe "build-release/JerseyDevil_Recompiled.exe" --disc "iso/Jersey Devil (USA).cue" --game game.toml --output replay-123
```

Use the actual session path and a new output directory. Automated replay mutes
speaker output while preserving audio emulation; normal play keeps audio on.
Replay checks the build identity and RAM fingerprints and uses isolated card
copies. Keep the matching game build, original session folder and your own
legal disc/BIOS available. A version string alone does not establish replay
compatibility across builds.
