# Gaia

Gaia watches the AI agents in your game, flags behaviour that looks broken, and records what they did. It runs in development builds only: nothing is compiled into shipping.

An alert looks like this, in the Output Log:

```
LogGaia: Warning: Character_0 [AirborneTooLong] In the air for 5.1 s. Did it fall through the floor, or is there no ground below it? (t=5.1 s, frame 10124)
```

Every alert says when it happened. The same moment is in a log file Gaia writes for each play session, on the agent's Visual Logger timeline, and through `UGaiaSubsystem` for tests.

## What you get

- **Alerts** for agents that look broken: stuck on a path, heading for the world origin, a StateTree that isn't running or is flipping between states, no navmesh in a world with AI. Each says what it found, with names.
- **A log** of every session, one JSON object a line: what each agent was doing, how it changed, the events your code records, and each alert. Written on a background thread.
- **A Visual Logger timeline** for each agent, so an alert can be read back to its cause.
- **Your own rules**, a few lines each, which see what you tell Gaia about your game.

## Requirements

- Unreal Engine 5.5 or 5.8. It's been built and run on 5.8.4 and 5.5 on Windows, and not on anything else.
- A C++ project: Gaia is a code plugin, so the project needs a module of its own and a compiler.
- AI that uses `AAIController`. StateTree is optional; Gaia reads it if it's there.

## Install

1. Put this repository in your project's `Plugins` folder, so that `YourProject/Plugins/Gaia/Gaia.uplugin` exists:
   ```
   cd YourProject/Plugins
   git clone https://github.com/VChuckShunA/Gaia.git
   ```
   (or download the ZIP and unzip it there; if your project is in git, a submodule keeps the plugin separate). The repository holds source only: the binaries are built on your machine in step 3.
2. Enable it: in `YourProject.uproject`, add to `"Plugins"`:
   ```json
   { "Name": "Gaia", "Enabled": true }
   ```
   (or Edit > Plugins). Gaia also needs the engine's `StateTree` and `GameplayStateTree` plugins, which it enables itself.
3. Regenerate project files, and build your editor target in the Development configuration.

That's all it needs. Play in the editor, or run a development game build. To check it works, open a map with AI, press Play, and look for the `LogGaia: Gaia watching ...` line (below), then type `gaia.Report` in the console.

## What happens with no setup

Gaia watches every AI-controlled pawn and reads what the engine knows: location, velocity, its own time dilation, movement mode, the StateTree's status and active states, path following, focus, and which senses are configured. The log's path is in the Output Log when a session starts:

```
LogGaia: Gaia watching MyMap. Log: .../Saved/Logs/AIDiagnostics/AIDiag_20261102-140000_MyMap_PIE0.jsonl
```

Rules that need to know things about *your* game (is it in combat, does it have a target, is it dead) stay quiet until you tell Gaia, below. A world with AI and no navmesh raises an alert at once. A world with no AI writes no file.

The newest 20 session logs are kept. Gaia only records on the server (or in a standalone game), and not in a client's world.

## Teaching Gaia your game

A **probe** is a class that reads your game's agents into plain data. `Samples/` has one that compiles in any project: it adds whether a character is in the air, and a rule for one that's up there too long.

1. Copy `Samples/GaiaSampleProbe.h` and `.cpp` into your game module's source folder, and rename the class if you like.
2. Add `"Gaia"` to your module's `PublicDependencyModuleNames` in its `.Build.cs`.
3. Name the class in `Config/DefaultGame.ini` (or Project Settings > Plugins > Gaia):
   ```ini
   [/Script/Gaia.GaiaSettings]
   ProbeClass=/Script/YourModule.GaiaSampleProbe
   ```
4. Build and play.

The sample shows each of the places a probe plugs in:

| Override | For |
|---|---|
| `IsAgent` | Which pawns to watch. By default, every AI-controlled one |
| `CaptureAgent` | What you know about an agent. Call `Super` first, then fill in the **common facts** the generic rules ask for (`bAlive`, `Activity`, `bInCombat`, `Target`, `bTargetVisible`, `bCanAct`, `LastActionTime`) and your own data |
| `CreateRules` | Gaia's rules plus yours |
| `CaptureAssets` | What an agent runs (its trees, say), as named assets, written once to the log. Lets a test check that agents are wired as intended |

Your own data is a struct derived from `Gaia::FProjectSample`. It's plain data, because it's copied to the worker thread; it writes itself into the log's snapshots and changes. A rule is a class derived from `Gaia::FRule`: override `OnSample`, for something that holds for too long, or `OnEvent`, for something that happens too often, and use `Context.HeldFor`, `Context.CountRecent` and `Context.Raise`. A rule runs on the worker thread and sees plain data only. The sample has the first form; the second is in the same few lines, for example a rule that raises when an event is recorded three times in ten seconds.

### Events

Sampling ten times a second can't see a reaction starting or a noise being heard. Your code records those where they happen:

```cpp
#include "GaiaSubsystem.h"

GAIA_EVENT(*Controller, MyTags::AI_Diag_Investigating, TEXT("Going to %s"), *Spot.ToString());
```

The first argument is the pawn or its controller, the second a native gameplay tag (the log shows its last part), then a `Printf` format. The message is only built while Gaia records, and the whole line compiles away in shipping, arguments included. Call it on the game thread.

### In tests

`UGaiaLibrary` (Blueprint or Python) has `SetSessionLabel`, so a log says which test played it, and `ExpectLogicStopped`, which tells Gaia a test stopped an agent's logic on purpose, so rules about that logic stay quiet until it runs again. Each class of agent also gets a **coverage** record at the end of a session (events and states it reached), so a test can check something happened, which alerts can't.

## Reading the output

- **The Output Log**, category `LogGaia`, and on screen for ten seconds. `gaia.OnScreen 0` turns the on-screen text off.
- **The log files**, `Saved/Logs/AIDiagnostics/`. Every record has `t` (world seconds) and `f` (frame). The format is in `Docs/LogSchema.json`; the first line says which version. A quick look: `findstr anomaly Saved\Logs\AIDiagnostics\*.jsonl`.
- **The Visual Logger.** While it records (Window > Developer Tools > Visual Logger, or the `vislog record` and `vislog stop` commands), each agent's timeline gets Gaia's alerts, your events, and every change of its StateTree states. Start it before the session: Gaia can't back-fill.
- **Console:**

| Command | Does |
|---|---|
| `gaia.Enabled 0` or `1` | Off or on (on). Turning it off finishes the log |
| `gaia.SampleRate <Hz>` | How often agents are captured, 1 to 60 (10) |
| `gaia.OnScreen 0` or `1` | Alerts on screen (on) |
| `gaia.Report` | Prints what Gaia knows about every agent, and what Gaia costs |

## Cost

On the game thread, a capture copies each agent into plain data: measured at 0.084 ms for seven agents (0.012 ms each) in a development build. Everything else is on a background thread. Entries for the Visual Logger are only worked out while it records. Lower `gaia.SampleRate` for a level with many agents.

## Limits

- Tested in three games (a shooter on StateTree, a boss fight on behaviour and State Trees, a shop simulation on behaviour trees) and a blank project, so the common facts and rules may not fit yours without a probe.
- Names of the StateTree's active states need the gameplay debugger, which shipping and some other builds leave out; then `states` is empty and the rules that don't use them still work.
- Recording is on the server only. Multiplayer hasn't been exercised.
- A StateTree flipping faster than the sample rate looks calmer than it is; events and counters catch what sampling can't.
- Rules measure in world time. An agent whose own time is stopped (`CustomTimeDilation` 0, as a game does to pause its AI) isn't counted as stuck or idle while it is; one sped up or slowed is measured in world seconds all the same.
- Gaia keeps the newest 20 logs. Copy any you want to keep.

## Versions

- **The plugin** has `VersionName` in `Gaia.uplugin`, as `major.minor.patch`. Before 1.0, a minor version may change the C++ API; a patch only fixes things. From 1.0, a breaking change to the API or the log bumps the major version.
- **The log** has its own number, `schema` in a session's first line (now 3). It goes up whenever a record's meaning changes: a field removed, renamed or changed. A new field doesn't. Anything that reads the logs should check it.
- **The engine:** a new engine version is supported when the plugin builds on it and a project's own tests pass. Until then it isn't.

See [CHANGELOG.md](CHANGELOG.md).

## Uninstall

Take it out of `.uproject`, delete the `Plugins/Gaia` folder, and remove `ProbeClass` and any probe code of yours. Nothing else in your project depends on Gaia.

## Licence

No licence has been chosen yet, so all rights are reserved. Add a `LICENSE` file before reusing or redistributing the code.
