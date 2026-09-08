# MultiverseGo - Project Notes for Agents

This file collects what has been learned about this game project so an agent
that reads it can pick the codebase up fast. It is game-specific; the engine's
own AGENTS.md (in the GDTK repository root) defines the coding standards that
apply to all code in both repositories.

## Repositories

- MultiverseGo (this repository): the game plugin and its content.
  - `Codes/`: the game. `Game.h/cpp` is the GamePlugin (turn flow, play/stop
    lifecycle). `Unit.h/cpp` holds the grid actors (player and patrols) and the
    player's walk state machine. `Main.cpp` is the standalone launcher.
  - `Resources/`: `Scenes/test.scene` (main scene), `Prefabs/` (player and
    patrol prefab scenes), `Meshes/Character/` (skeleton, skinMesh, and the
    `.anim` clips under `Movement/` and `Execution/`), materials, audio...
  - `Plugins/grider`: a git submodule providing editor grid tooling
    (`GridEditor.cpp`) and the shared grid data model `GridGraph.h/cpp`, whose
    source is compiled verbatim into the game plugin.
- GDTK (engine, separate repository, currently on branch `develop`, checked out
  around `~/Documents/GitHub/GDTK`): contains `ToolKit/` (engine sources) and
  its own `AGENTS.md` with coding standards. Engine changes live and get
  committed there, never in this repo.

## Building

- Game plugin: `ninja -C build` from this repo root (Debug config, ninja
  generator). Output: `Codes/Bin/MultiverseGod.so` (Debug postfix adds the
  `d`). The engine path is resolved from
  `~/.config/ToolKit/Config/Path.txt` at configure time.
- Engine: `cmake --build <GDTK>/build --target ToolKit -j 4` (Debug config).
  Debug engine library: `<GDTK>/BinDebug/libToolKitd.so`.
- The editor and plugins load shared libraries from their bin directories at
  startup, so swapping the `.so` files is enough after a rebuild (no editor
  rebuild required for engine-only changes).
- Pressing Play in the editor runs the game plugin (`Game::OnPlay`); stopping
  runs `Game::OnStop`.

## Gameplay overview (as of the animation milestone)

- Turn-based stealth-like prototype on a tile grid. The player moves exactly
  one tile per turn by clicking a connected neighbor; patrol enemies act on a
  fixed order after the player phase, then the turn returns to the player.
- Grid: a master entity named `GridNode` parents every tile. `GridGraph`
  (`Plugins/grider/Codes/GridGraph.h/cpp`) builds the data model at play start:
  `node.center` = tile top-surface center, `node.size` = tile AABB extent, so
  the neighbor spacing equals the tile size. The grid is PARAMETRIC, not fixed
  (any N x M, any tile size; the current scene uses 5-unit spacing). Traverse
  with `NodeAtPoint`, `Neighbor`, `Connected`.
- Entities are found by tag at `Game::OnPlay`: `scene->GetByTag("player")`,
  `"stationary-patrol"`, `"linear-patrol"`, `"seeker-patrol"`; optional target
  marker tagged `"target"`; the grid master by name via `GetFirstByName`.
- `Unit` (Codes/Unit.h/cpp) wraps the prefab top root entity. Subclasses:
  `Player`, `StationaryPatrol`, `LinearPatrol`, `SeekerPatrol`. Turn behavior
  lives in `OnTurn`; `Player` also implements per-frame driving of its walk.
- Model forward convention: local -Z. `Unit::FaceTowards` rotates the root so
  -Z faces the given direction (`RotationTo(-Z, dir)`). Grid directions:
  `GridDir::Xm/Xp/Zm/Zp`.

## Prefab and animation setup

- Character prefabs (e.g. `player.scene`): the tagged top node (named `root`,
  an empty anchor `EntityNode`) is the unit/game root. The actual character is
  its CHILD entity, which carries `MeshComponent` (SkinMesh), `SkeletonComponent`,
  `AnimControllerComponent`, `MaterialComponent` and an `AABBOverrideComponent`.
- The player's `AnimControllerComponent` records ("signals"): `rest`, `idle`,
  `walk_f_start`, `walk_f`, `walk_f_end`, `run_f_start`, `run_f`, `run_f_stop`,
  `fight_walk_f`, `turn_l_90`, `turn_l_180`, `turn_r_90`, `turn_r_180`. Scene
  files persist `ApplyRootMotion="0"`; the walk code enables root motion on the
  three walk clips at runtime.
- Animation clips: `Resources/Meshes/Character/Player/Movement/*.anim`. The root
  key (skeleton root bone) of the player character is `Character_Male_Jacket`.
- The player's actor child carries an authored 180-degree Y yaw. Together with
  the engine's local-space root motion this maps the baked +Z root curve to
  forward walking. Do not "fix" apparent direction problems by reverting the
  engine to world-space root motion application; adjust prefab yaw / the
  facing convention instead.

## Root motion (engine)

- `ToolKit::AnimationPlayer::ApplyRootMotion`
  (`<GDTK>/ToolKit/Resources/Animation.cpp`) samples the animation's root key
  between `record->m_prevRootMotionTime` and `record->m_currentTime` and applies
  the delta to the record's entity node in LOCAL space
  (`TransformationSpace::TS_LOCAL`, translate + rotate). It was deliberately
  changed from world space so the actor/prefab orientation decides the walk
  direction: rotate the prefab top root to aim the actor.
- `AnimRecord`: `m_applyRootMotion`, `m_loop` (`Play` always sets it true),
  `m_timeMultiplier`, `m_currentTime` (seconds). `AnimControllerComponent::Play`
  (`signalName`, `stopPrevAnim = true`) resets `m_currentTime` and
  `m_prevRootMotionTime` and registers the record with the global
  `AnimationPlayer`, which updates every engine frame.
- Timing gotcha: engine frame deltas arrive in MILLISECONDS; the
  `AnimationPlayer` converts to seconds internally. Game/Player code must
  convert ms to seconds before driving the walk state machine.

## Player walk state machine (in Codes/Unit.cpp)

- Purpose: a root-motion-driven walk between two grid nodes that covers ANY
  (parametric) gap and stops exactly on the destination node center.
- Flow (async): a click on a connected neighbor reaches `Player::TryMove` (it
  validates the direct connected neighbor and that the player has not moved
  this turn), which calls `Player::StartWalk`:
  1. `FaceTowards(step)` on the prefab top root - an instant snap rotation for
     now; the in-place turn clips are not wired to gameplay yet.
  2. Enable root motion on `walk_f_start`, `walk_f`, `walk_f_end`.
  3. Build a fresh engine `StateMachine` (`ToolKit::State`/`StateMachine`, one
     per walk, deleted on arrival) with the states:
     - `WalkStart`: plays `walk_f_start` once (the record keeps looping; the
       state switches on its own elapsed time reaching the clip duration).
     - `WalkLoop`: plays `walk_f` looping while the remaining distance is larger
       than the end clip's root travel.
     - `WalkEnd`: plays `walk_f_end`; the walk ends when the remaining distance
       drops below `kWalkArriveEps` (~0.03 units) or the clip plays through.
- Distance math: every frame, `remaining` = horizontal distance from the actor
  node (`m_actor->m_node`) to the destination `node.center`. The loop-to-end
  switch fires when `remaining <= endReach`, where `endReach` is computed at
  runtime by `ClipRootTravel`: the net horizontal displacement between the
  first and the last root key of the end clip. Reference numbers for the
  current assets: `walk_f_start` ~1.15 units over 0.8 s, `walk_f` ~1.92 units
  per 1.3 s cycle, `walk_f_end` ~0.39 units over 0.8 s.
- Clip transitions crossfade: every phase switch (idle -> walk_f_start ->
  walk_f -> walk_f_end -> idle) goes through the helper `BlendTo`, which calls
  `AnimControllerComponent::SmoothTransition(signal, kWalkBlendDuration)` so
  the engine fills the record blending data and fades the skeleton pose over
  `kWalkBlendDuration` (0.2 s) instead of popping. IMPORTANT: before blending,
  `BlendTo` sets `m_applyRootMotion = false` on the outgoing record; while it
  still sits in the animation player during the fade it would otherwise keep
  driving the actor together with the incoming clip (double movement). Only the
  incoming clip moves the actor.
- `Player::FinishWalk` anchors the prefab top root on the exact destination
  center and restores the actor's authored local translation
  (`m_actorLocalBase`) so the root-motion offset accumulated on the actor node
  is folded back into the top root; it then returns the character to the idle
  loop. (Do not read the walk context after it is deleted - that was a
  use-after-free bug.)
- `Game.cpp`: while `Player::IsWalking()` is true, `Game::Frame` calls
  `Player::Frame(dt)` and defers `CompletePlayerMove()` (patrol contact, win
  check, enemy turn) until the walk actually arrives. Input is locked while
  walking.
- Stall watchdog: if the gap does not shrink for ~1 second the walk aborts and
  snaps the player to the tile (log: `walk stalled ... snapping`). This guards
  against a rig/orientation mismatch ever locking the turn.
- Legacy actors without an `AnimControllerComponent`: `TryMove`/`StartWalk`
  fall back to the old instant snap, so scenes without character prefabs keep
  working.

## Logs and failure signatures

- `Player: walk started (...) -> (...), gap ..., end clip reach ...`: during a
  normal walk the gap must shrink every frame.
- `Player: walk stalled (gap ... not shrinking); snapping to the tile.`: root
  motion direction/orientation mismatch; check the prefab yaw and the facing
  convention.
- All game logs go through `TK_LOG`.

## Scene files may be dirty from the live editor

- The ToolKit editor writes `Resources/Scenes/test.scene` and prefab files
  while it is open, so they often show as modified without a deliberate code
  change. Inspect and commit them deliberately, never blindly.

## Commit policy (important)

- NEVER commit, stage or push anything in any repository (this repo, the GDTK
  engine repo, or the grider plugin) without asking the human first and getting
  an explicit go-ahead. Committing is done by the human or only after their
  approval.
- If work is finished and commits are pending, summarize what is ready to be
  committed and ask whether to commit (and push) -- do not do it unilaterally.
