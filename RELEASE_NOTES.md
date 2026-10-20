# v0.3.2

Frame interpolation is withdrawn because it garbled the picture and could freeze the game. It can no longer be enabled, including from older saved settings or leftover mod copies. Widescreen now keeps the HUD in the screen corners at its original size. Playtest Diagnostics is off by default; enable it in Mods before reproducing a problem you want to report.

Windows and Linux (AppImage) builds. These remain preview builds; full-game completion is not recertified.

---

# v0.3.1

Offline playtest recording and the controller display are enabled by default. Sessions capture accepted inputs, controller presence, RAM checks and host/CD/render timing evidence; the bundled replay helper checks reproduction against the same build. See [the diagnostics guide](docs/PLAYTEST_DIAGNOSTICS.md) for incident marking and private replay.

The framework and launcher are pinned to the qualified rollout revisions. Existing rendering improvements and game timing are retained. These remain preview builds; full-game completion is not recertified.

---

# v0.3.0

Game-code interpolation for higher presentation rates; camera/model motion stays separate from simulation. Widescreen subdivision and renderer-bank culling fixes retain geometry across the wider view.

Enable the frame-rate and widescreen enhancements in Mods. Native game timing remains unchanged. These are playable preview builds; full-game completion is not recertified.

---

# v0.2.1

The packaged runtime enables its native overlay cache. The shared release
pipeline now rejects configurations that would leave shipped modules unused.

Secondary executables and compressed renderer banks now ship with native AOT
coverage prepared directly from the original USA disc. This includes all five
secondary executables and five renderer variants across 70 bank occurrences.
Interpreter and runtime compilation fallback remain available.
One 100-byte helper in SAVE.EXE remains on fallback because it contains an
unsupported original instruction; its exclusion is byte-verified and documented.

Extraction uses shared psxrecomp methods, with checked loader instructions and
an inventory of all 93 BZZ archives. The decoder matches execution of the
original MIPS instructions on every relevant metadata and bank member.

Windows and Linux packagers always freshly extract, compile and audit the
configured shards. Releases include the native audit receipt, fallback tooling,
OpenBIOS, and the existing optional widescreen and frame interpolation mods.
Full static execution coverage and a complete playthrough remain unproven.
