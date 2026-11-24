# Gaia changelog

The plugin version is `VersionName` in `Gaia.uplugin`; the log's own version is `schema` in each session's first line. See "Versions" in the [README](README.md).

## 0.2.0

Log schema 3 (a new field only).

- **Paused agents:** each agent's own time dilation (`CustomTimeDilation`) is captured as `FAgentSample::TimeDilation` and logged as `timeDilation`. While it's 0, the Stuck, AttackWithoutTarget and NotEngaging rules don't count the time, so a game that pauses its AI (by setting its agents' own time to 0) doesn't raise them. `FAgentSample::IsTimeFrozen` is there for a project's own rules.
- **Engine:** builds and runs on UE 5.5 as well as 5.8, unchanged.

## 0.1.0

The first version. Log schema 3.

- **Watching:** every AI-controlled pawn a probe recognises, captured ten times a second on the game thread and checked on a worker thread. Nothing in shipping builds.
- **Rules:** stuck, heading for the world origin, StateTree not running or thrashing, dead but still active, in combat with no target, fighting without acting, a missing sense, and no navmesh. Projects add their own.
- **Log:** one JSON Lines file a session in `Saved/Logs/AIDiagnostics`, with snapshots, changes, events, alerts, a summary for each agent, and coverage for each class. The newest 20 are kept.
- **Reporting:** alerts in the Output Log (with the time and frame), on screen, on the agent's Visual Logger timeline, and through `GetRecentAnomalies`. Events and StateTree changes go on the same timeline while the Visual Logger records.
- **Probes:** a base probe that works with no setup, and a project class that adds its own facts, data, rules and the assets an agent runs.
- **Tests:** `UGaiaLibrary` labels a session with the test that played it and declares a stop on purpose.
