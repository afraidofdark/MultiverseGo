# MultiverseGo - Project Notes for Agents

This file collects what has been learned about this game project so an agent
that reads it can pick the codebase up fast. It is game-specific; the engine's
own AGENTS.md (in the GDTK repository root) defines the coding standards that
apply to all code in both repositories.

## Repositories

- MultiverseGo (this repository): the game plugin and its content.
  - `Codes/`: the game. `Game.h/cpp` is the GamePlugin (turn flow, play/stop
    lifecycle). `Unit.h/cpp` holds the grid actors (player and patrols), the
    shared animated walk, and the turn decisions. `Main.cpp` is the standalone
    launcher.
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
  one tile per turn by clicking a connected neighbor. Each turn resolves as ONE
  concurrent act: the moment the player commits to a tile, every enemy decides
  its reaction to that move and they all animate at the same time (see
  "Parallel turn orchestration" below). Only eating waits until the player
  physically arrives on its destination tile.
- Grid: a master entity named `GridNode` parents every tile. `GridGraph`
  (`Plugins/grider/Codes/GridGraph.h/cpp`) builds the data model at play start:
  `node.center` = tile top-surface center, `node.size` = tile AABB extent, so
  the neighbor spacing equals the tile size. The grid is PARAMETRIC, not fixed
  (any N x M, any tile size; the current scene uses 5-unit spacing). Traverse
  with `NodeAtPoint`, `Neighbor`, `Connected`.
- Entities are found by tag at `Game::OnPlay`: `scene->GetByTag("player")`,
  `"stationary-patrol"`, `"linear-patrol"`, `"seeker-patrol"`; optional target
  marker tagged `"target"`; the grid master by name via `GetFirstByName`.
- `Unit` (Codes/Unit.h/cpp) wraps the prefab top root entity. Units that move
  between tiles derive from `AnimatedUnit` (the shared root-motion walk), with
  `Player`, `StationaryPatrol`, `LinearPatrol`, `SeekerPatrol` on top; each
  concrete type only decides its next tile in `OnTurn`, and `AnimatedUnit`
  drives the move every frame.
- Model forward convention: local -Z. `Unit::FaceTowards` rotates the root so
  -Z faces the given direction (`RotationTo(-Z, dir)`). Grid directions:
  `GridDir::Xm/Xp/Zm/Zp`.

## Prefab and animation setup

- Character prefabs (e.g. `player.scene`, the three patrol prefabs): the tagged
  top node (named `root`, an empty anchor `EntityNode`) is the unit/game root.
  The actual character is its CHILD entity, which carries `MeshComponent`
  (SkinMesh), `SkeletonComponent`, `AnimControllerComponent`,
  `MaterialComponent` and an `AABBOverrideComponent`. Every character's actor
  carries the same anim controller records ("signals"): `rest`, `idle`,
  `walk_f_start`, `walk_f`, `walk_f_end`, `run_f_start`, `run_f`, `run_f_stop`,
  `fight_walk_f`, `turn_l_90`, `turn_l_180`, `turn_r_90`, `turn_r_180`. Scene
  files persist `ApplyRootMotion="0"`; the walk code enables root motion on the
  three walk clips at runtime.
- Animation clips: `Resources/Meshes/Character/Player/Movement/*.anim` are
  shared by every animated actor. The root
  key (skeleton root bone) of the player character is `Character_Male_Jacket`.
- The character actors carry an authored 180-degree Y yaw. Together with
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
- `AnimRecord`: `m_applyRootMotion`, `m_loop` (respected by Play; defaults to
  false = one-shot, see "Clip looping is explicit" below), `m_timeMultiplier`,
  `m_currentTime` (seconds). `AnimControllerComponent::Play`
  (`signalName`, `stopPrevAnim = true`) resets `m_currentTime` and
  `m_prevRootMotionTime` and registers the record with the global
  `AnimationPlayer`, which updates every engine frame.
- Timing gotcha: engine frame deltas arrive in MILLISECONDS; the
  `AnimationPlayer` converts to seconds internally. Game/Player code must
  convert ms to seconds before driving the walk state machine.

## Shared walk state machine (AnimatedUnit, in Codes/Unit.cpp)

- Lives on `AnimatedUnit` so the player and every patrol move with the SAME
  code: `Unit` (grid/glide base) -> `AnimatedUnit` (walk FSM + timing + scale)
  -> `Player`/patrols (only decide the next tile). Movement is driven by
  `AnimatedUnit::StartMove(node, targetDuration)`, which plays the walk FSM
  when the unit has an `AnimControllerComponent` and otherwise falls back to a
  glide of the same target length.
- Purpose: a root-motion-driven walk between two grid nodes that covers ANY
  (parametric) gap and stops exactly on the destination node center.
- Flow (async): a click on a connected neighbor reaches `Player::TryMove` (it
  validates the direct connected neighbor and that the player has not moved
  this turn), which starts the shared walk:
  1. Turn decision: if the current facing (`root` local -Z) is not already
     aligned with the step axis (`|dYaw| > 0.02`), a turn-in-place phase runs
     first (`WalkTurn`); otherwise `FaceTowards(step)` snaps the top root and
     the machine starts directly in `WalkStart`.
  2. Enable root motion on `walk_f_start`, `walk_f`, `walk_f_end` (and on the
     turn clip when one is used).
  3. Build a fresh engine `StateMachine` (`ToolKit::State`/`StateMachine`, one
     per walk, deleted on arrival) with the states:
     - `WalkTurn`: plays the in-place turn (see "Turn-in-place" below), then
       hands over to `WalkStart`.
     - `WalkStart`: plays the one-shot `walk_f_start` (`m_loop = false`); it
       holds its final frame at its end and the state switches on its own
       elapsed time reaching the clip duration.
     - `WalkLoop`: plays `walk_f` (a looping clip) while the remaining distance
       is larger than the end clip's root travel.
     - `WalkEnd`: plays the one-shot `walk_f_end`; the walk ends when the
       remaining distance drops below `kWalkArriveEps` (~0.03 units) or the
       clip has fully played.
- Turn-in-place (in `Codes/Unit.cpp`): the turn clips were re-authored so the
  rotation is ROOT MOTION (bone space only steps in place), so `WalkTurn`
  plays the selected clip with `m_applyRootMotion = true`, `m_loop = false`
  and the engine yaws the ACTOR node itself; translation stays zero. The clip
  is picked by `TurnClipFor` from the signed yaw delta in degrees
  (`turn_l_90/180`, `turn_r_90/180`). When the clip ends, `WalkTurn` FOLDS the
  rotation into the persistent facing: the prefab top root is set to the
  target yaw and the actor's local orientation is restored to its pre-turn
  base, so the front-authored walk clip starts clean. If no usable clip
  exists, a node-only fallback yaws the top root over `kWalkTurnDuration`
  (0.4 s). The stall watchdog is exempt while `turning` is true (a turn
  legitimately makes no distance progress).
- Yaw units gotcha (in `Codes/Unit.cpp`): `YawOf`, `YawDeltaTo` and
  `YawRotation` all work in RADIANS; `glm::rotate` expects radians too. A
  stray `glm::degrees` inside `YawRotation` once scaled every fold yaw by
  180/pi and left the character facing a near-random direction (log symptom:
  `TurnFold: root yaw ... (wanted ...)` disagreeing). Do not "fix" that by
  converting call sites to degrees.
- Turn root motion and facing agree on Y only: the actor's authored base yaw
  (180 degrees) and the turn clips' rotation are both pure +Y rotations, so
  local-axis deltas equal world-axis deltas; the actor world orientation after
  the fold is exactly what `FaceTowards(stepDir)` would have produced, which
  keeps the walk direction identical to a straight move.
- Clip loop flags (app): the walk machine marks the clips explicitly before
  playing them -- `walk_f_start` and `walk_f_end` are ONE-SHOT
  (`m_loop = false`), `walk_f` and `idle` loop (`m_loop = true`).
- Distance math: every frame, `remaining` = horizontal distance from the actor
  node (`m_actor->m_node`) to the destination `node.center`. The loop-to-end
  switch fires when `remaining <= endReach`, where `endReach` is computed at
  runtime by `ClipRootTravel`: the net horizontal displacement between the
  first and the last root key of the end clip. Reference numbers for the
  current assets: `walk_f_start` ~1.15 units over 0.8 s, `walk_f` ~1.92 units
  per 1.3 s cycle, `walk_f_end` ~0.39 units over 0.8 s.
- Clip transitions crossfade: every phase switch (idle -> walk_f_start ->
  walk_f -> walk_f_end -> idle) goes through the helper `BlendTo`, which calls
  `AnimControllerComponent::SmoothTransition(signal, gWalkBlendDuration)` so
  the engine fills the record blending data and fades the skeleton pose over
  `gWalkBlendDuration` instead of popping. IMPORTANT: before blending,
  `BlendTo` sets `m_applyRootMotion = false` on the outgoing record; while it
  still sits in the animation player during the fade it would otherwise keep
  driving the actor together with the incoming clip (double movement). Only the
  incoming clip moves the actor.
- Clip looping is explicit (engine): `AnimControllerComponent::Play` no longer
  forces `m_loop = true`; it respects the record's own flag. One-shot records
  (`m_loop = false`) reaching their duration HOLD their final frame instead of
  wrapping to the first frame or being dropped -- the Unity-like behavior that
  keeps a walk-stop clip on its stopping pose while it crossfades to idle.
  Looping clips keep looping. `gWalkBlendDuration` is a GLOBAL float (declared
  in Unit.h, defined in Unit.cpp, default 0.2 s) so the crossfade length can be
  tuned at runtime instead of an inline constant.
- Fade timing: clip switches happen at the clip boundary (WalkStart leaves
  when its one-shot has fully played). This is seamless because the one-shot
  holds its final frame at the boundary -- no wrap, no early-leave hacks
  needed.
- Debug logs: the walk machine logs every phase switch and `BlendTo` logs the
  outgoing/incoming clips and the fade length. (The engine used to log every
  frame of a clip's fade-out -- `AnimBlend: fading out ...` / `faded out
  after ...` -- while transitions were being verified; those debug logs were
  removed from `ToolKit/Resources/Animation.cpp`, so the per-frame spam is
  gone.)
- `AnimatedUnit::FinishWalk` anchors the prefab top root on the exact
  destination center, restores the actor's authored local translation
  (`m_actorLocalBase`) so the root-motion offset accumulated on the actor node
  is folded back into the top root, plays any queued arrival turn
  (`TurnOnArrival`) and returns the character to the idle loop. (Do not read
  the walk context after it is deleted - that was a use-after-free bug.)
- `Game.cpp`: once a move is committed, `Game::Frame` switches to the acting
  phase and drives the player's walk plus every enemy move together; input is
  locked until the whole turn settled (see "Parallel turn orchestration").
- Stall watchdog: if the gap does not shrink for ~1 second the walk aborts and
  snaps the unit to the tile (log: `Move: walk stalled ... snapping`). This
  guards against a rig/orientation mismatch ever locking the turn.
- Units without an `AnimControllerComponent`: the player lands instantly on
  `TryMove`; enemies fall back to a glide of the same target length
  (`Unit::StartMove` / `AnimatedUnit::StartMove`), so scenes without animated
  character prefabs keep working.

## Parallel turn orchestration (in Game.cpp)

- One concurrent act per turn: a click on a connected neighbor commits the
  player's move (`Player::TryMove` -> the shared walk) and then
  `Game::BeginPlayerMove(dest)` freezes the turn:
  1. Every enemy decides its action AT ONCE, against the tile the player WILL
     stand on (`dest`) and the heading it will face there
     (`Game::FacingToward`), i.e. exactly the inputs the old sequential enemy
     phase used.
     - A patrol standing ON `dest` is "captured": it never acts this turn and
       is removed when the player arrives (`m_capturedEnemies`).
     - `OnTurn` only DECIDES: a moving patrol records the tile it will step to
       (`Unit::GetIntendedMove`) instead of teleporting.
     - A step onto the player's destination is a bite (`m_stepBites`): held
       back until the player actually arrives.
     - Any other step starts moving immediately (`StartMove`), so it runs at
       the same time as the player's walk.
  2. `m_phase = Acting`; `Game::UpdateActing` drives the player's walk and
     every enemy move together each frame (input stays locked).
  3. When the player physically arrives (`Game::ResolvePlayerArrival`):
     captured patrols leave the grid; guards whose threat tile is the player's
     tile LUNGE (added to `m_activeBites`); the win is checked only when no
     guard strike is inbound; then the held-back step bites start moving. A
     bite move that lands eats the player (`Game::EatPlayer`).
  4. When the player arrived and no bite is pending/in flight and no enemy is
     moving, `StartPlayerTurn` hands the input back.
- Guards never move on their own (their `OnTurn` is empty): the lunge is
  exclusively the arrival-time reaction when `ThreatTile() == player tile`,
  preserving the old resolution order -- capture first, then guard bites, then
  win, then moving-patrol bites.
- Every enemy move runs through the polymorphic `Unit::StartMove(node,
  duration)` / `AnimatedUnit::StartMove(node, duration)`: the shared animated
  walk when the patrol's prefab carries an `AnimControllerComponent`, otherwise
  a plain glide of the same length. `StartMove` snaps onto the exact node
  center when the move lands (`m_node` updates only on arrival), so patrols
  keep working with or without an animated actor.
- In-place turns are animated too, through the SAME turn phase:
  `AnimatedUnit::StartTurn(dir)` / `StartInPlaceTurn(yaw)` play the turn clip +
  fold mini state machine (no walking states) and `TurnOnArrival(orient)`
  queues the turn to play the moment the current move lands. Used for the
  LinearPatrol line-end about-face and the SeekerPatrol arrival turn / idle
  stare; units without turn clips snap instantly (Unit::StartTurn fallback).
- `SeekerPatrol` decides at click time against the destination. Its arrival
  look is taken from the tile it WILL land on along the held heading
  (`SeesAlong(origin, dir, player)`); the actual turn to that heading runs
  ANIMATED the moment the move lands (`TurnOnArrival`), so the patrol arrives
  and then turns like the player would.

## Uniform turn duration (time scaling)

- Goal: every entity's action of a turn lasts exactly `gTurnDuration` seconds
  (global float, default 3.0, tunable like gWalkBlendDuration) -- the whole
  tableau starts and stops together.
- The move's NATURAL duration is measured from the animation data, not
  hardcoded. `WalkClipTiming` (built by `BuildClipTiming` from each clip's root
  key: key time = frame / fps; progress = running max of the signed projection
  of the root position onto the net travel axis, clamped to the playable clip
  duration) is cached per unit in `AnimatedUnit::EnsureWalkTimings` at init and
  lazily when the clip resources (re)load. `EstimateWalkDuration` then sums
  the machine phases exactly the way the FSM gates them (optional turn duration
  + wind-up + as many stride cycles as needed + landing clip) into the natural
  length (a 5-unit move with a turn measures ~4.7 s natural on the current
  assets; without a turn ~3.7 s).
- `AnimatedUnit::StartWalk` computes `scale = natural / target`, stores it in
  `AnimatedUnit::m_timeScale` and writes it into the `m_timeMultiplier` of
  every clip that can play during the walk (idle + the three walk clips + the
  four turn clips), so the FSM timers AND the clip playback -- including blend
  countdowns -- advance at the same rate. `AnimatedUnit::Frame` feeds
  `dt * m_timeScale` to the machine; `FinishWalk` restores 1x before the idle
  settle blend. The engine scales each record by its OWN `m_timeMultiplier`, so
  every animated unit of a turn scales independently to the same target.
- Moves run per unit: the player's `TryMove` and every enemy's `StartMove`
  (tile step, glide fallback and bite lunge alike) target `gTurnDuration`,
  so every moving action of a turn lasts the same length -- there is no second
  "temporary" move duration global.

## Logs and failure signatures

- `Move: natural X.XX s -> Y.YY s (xZ.ZZ), turn yes/no`: per-move timing (one
  per animated unit per turn). X is the natural FSM duration measured from the
  walk clips; the move is scaled so it finishes in the requested target length
  (Y). A missing/wrong X means the clip timing profiles failed to build.
- `Move: walk started (...) -> (...), gap ..., end clip reach ...`: during a
  normal walk the gap must shrink every frame.
- `Move: walk stalled (gap ... not shrinking); snapping to the tile.`: root
  motion direction/orientation mismatch; check the prefab yaw and the facing
  convention.
- `Game: a guard strikes the player on (...).` / `Game: a patrol closes in on
  the player on (...).`: an eat is inbound (a bite move started after the
  player arrived); the loss lands when that move completes.
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
