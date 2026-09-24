# MultiverseGo - Project Notes for Agents

This file collects what has been learned about this game project so an agent
that reads it can pick the codebase up fast. It is game-specific; the engine's
own AGENTS.md (in the GDTK repository root) defines the coding standards that
apply to all code in both repositories.

## Repositories

- MultiverseGo (this repository): the game plugin and its content.
  - `Codes/`: the game. `Game.h/cpp` is the GamePlugin (turn flow, play/stop
    lifecycle). `Unit.h/cpp` holds the grid actors (player and patrols), the
    shared animated walk, and the turn decisions. `ClipMotion.h/cpp` measures
    what a clip's root key does to its actor (shared by the walk and the
    execution). `Execution.h/cpp` is the execution/strike catalogue.
    `Main.cpp` is the standalone launcher.
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

- Game plugin: `cmake --build Intermediate/Plugin -j 4` from this repo root
  (Debug config; the CMake binary dir is `Intermediate/Plugin`, generated with
  Unix Makefiles -- the older `build/` ninja directory no longer exists).
  Output: `Codes/Bin/MultiverseGod.so` (Debug postfix adds the `d`). The engine
  path is resolved from `~/.config/ToolKit/Config/Path.txt` at configure time.
- On a Windows checkout the same binary dir is generated for Visual Studio, so
  the build line becomes `cmake --build Intermediate/Plugin --config Debug -j 4`
  and the plugin lands in `Codes/Bin/MultiverseGod.dll` (the whole `**/Bin/`
  tree is gitignored, so the built plugin is never part of a commit -- a fresh
  checkout has to be built once). The engine path the configure step used is
  `TOOLKIT_DIR` in `Intermediate/Plugin/CMakeCache.txt` -- do not assume the
  `~/.config/ToolKit/Config/Path.txt` of the Linux notes.
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
  `fight_walk_f`, `turn_l_90`, `turn_l_180`, `turn_r_90`, `turn_r_180`, plus the
  execution pairs: `ambush_1/2/3` (striker) with `ambushed_1/2/3` (victim), and
  `execution_1..13` (striker) with `executed_1..13` (victim). Scene
  files persist `ApplyRootMotion="0"`; the walk code enables root motion on the
  three walk clips at runtime, and the execution code does the same for the
  strike / reaction clips it plays.
- Animation clips: `Resources/Meshes/Character/Player/Movement/*.anim` and
  `.../Execution/*.anim` are shared by every animated actor. The root
  key (skeleton root bone) of the player character is `Character_Male_Jacket`,
  named by the `rootKey="..."` attribute of every `.anim` file -- that track is
  the one the engine applies as root motion and the one the game measures (see
  "Clip motion measurement").
- The execution clips carry root motion too, and it is what places the strike:
  measured forward reach (the running max of the root track's travel along its
  net axis) is `ambush_1` ~1.64 u over 1.70 s, `ambush_2` ~0.46 u over 1.90 s,
  `ambush_3` ~1.59 u over 1.70 s (reactions `ambushed_*` 3.73 s each), and for
  the head on series `execution_1..13` 0.91-2.65 u over 1.90-3.37 s (reactions
  `executed_1..13` 3.30-4.13 s). Never hardcode those numbers -- they are
  measured at runtime.
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
     - `WalkLoop`: plays the move's STRIDE clip (a looping clip) while the
       remaining distance is larger than the landing phase's root travel. The
       stride is `walk_f` for a plain step and `fight_walk_f` when the move is an
       EXECUTION (see "Execution system": an attacker walking into its victim
       walks the fight cycle). The context carries it as `ctx->loopSignal`, so
       the phase itself is stride-agnostic and the clip's own measured motion is
       what the move is timed on (`StartAction` passes that profile to
       `EstimateWalkDuration`).
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
  (`m_loop = false`), `walk_f`, `fight_walk_f` and `idle` loop (`m_loop = true`).
- Distance math: every frame, `remaining` = horizontal distance from the actor
  node (`m_actor->m_node`) to the destination `node.center`. The loop-to-end
  switch fires when `remaining <= endReach`, where `endReach` is computed at
  runtime by `MeasureClipRootTravel` (ClipMotion.h): the net horizontal
  displacement between the first and the last root key of the end clip.
  Reference numbers for the current assets: `walk_f_start` ~1.15 units over
  0.8 s, `walk_f` ~1.92 units per 1.3 s cycle, `walk_f_end` ~0.39 units over
  0.8 s, `fight_walk_f` ~1.91 units per 1.1 s cycle (the same step at a faster
  combat cadence, measured the same way into `m_timingFightLoop`).
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

## Play, pause and stop (GamePlugin lifecycle)

- Two layers, deliberately separated:
  1. The EDITOR owns the simulation state. The play / pause / stop buttons
     (`SimulationWindow::ShowActionButtons`) only call `App::SetGameMod(...)`,
     and the engine wide consequences of a state live there, in
     `App::ApplySimulationServices`: it pauses / resumes the animation player,
     and any future engine service that runs on its own during a play session
     belongs in the same place. This behaviour is identical for every project,
     so it must not be repeated in each game plugin.
  2. The PLUGIN is only told about the change (`OnPlay / OnPause / OnResume /
     OnStop`) and does its own, game specific work. This game currently needs
     none of them, so they are empty.
- `Game::Frame` is called only while the simulation is RUNNING
  (`PluginManager::Update`), so pausing freezes the turn flow by itself: the
  walk FSM simply gets no delta time.
- The engine's animation player does not work that way: `Main::Frame` updates it
  every frame whatever the simulation state is, which is why pausing has to hold
  it too (see above). The API is `AnimationPlayer::Pause / Resume / IsPaused`
  (engine, GDTK `ToolKit/Resources/Animation.{h,cpp}`) and it holds every
  record: record times, blend countdowns and root motion all stop, the animation
  data of the last update is kept, and resuming carries on from the same time
  with no jump.
- Do not pause per record instead: `AnimControllerComponent::Pause` pauses only
  the ACTIVE record (and dereferences it without a null check), so a fade-out
  that is still registered or a unit whose clip just ended would keep moving.
- Engine and editor changes are made in the GDTK checkout and rebuilt there
  (`cmake --build <GDTK>/build --target ToolKit -j 4`, and `--target Editor` for
  editor code such as `App.cpp`). The editor loads
  `<GDTK>/BinDebug/libToolKitd.so` from its own directory via `$ORIGIN`, so a
  running editor must be restarted to pick up a new engine library.

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
       back until the player actually arrives, and any landing turn the enemy
       queued is dropped (`CancelArrivalTurn`) -- a bite ends the turn, so the
       enemy bites and stops instead of about-facing afterwards.
     - Any other step starts moving immediately (`StartMove`), so it runs at
       the same time as the player's walk.
  2. `m_phase = Acting`; `Game::UpdateActing` drives the player's walk and
     every enemy move together each frame (input stays locked).
  3. When the player physically arrives (`Game::ResolvePlayerArrival`):
     captured patrols leave the grid; guards whose threat tile is the player's
     tile LUNGE (added to `m_activeBites`); the win is checked only when no
     guard strike is inbound; then the held-back step bites start moving. A
     bite resolves the moment the biting enemy STANDS on the player's tile
     (`bite->GetNode() == player tile`), not when its animation finishes: the
     eat does not wait for a queued turn or a fade, it bites and stops there
     (`Game::EatPlayer`). A bite with an authored EXECUTION for its relation is
     the exception: it runs its whole strike scene first (see "Execution
     system"), and the loss lands on the scene's last beat.
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
  LinearPatrol line-end about-face, the SeekerPatrol arrival turn / idle stare
  and the victim's pre-strike turn of a side strike
  (`TurnToFaceAttacker`, see "Execution system"); units without turn clips snap
  instantly (Unit::StartTurn fallback). Any in-place turn also raises
  `IsTurning()` until its action ends, which is what the side strike's hold
  gates on.
  Reaching the END of its line IS special while stepping: when the step lands there, the
  about-face is QUEUED for that arrival (`Linear: step reaches the line end; about-face
  queued for the arrival.`), so the patrol reaches the end of its line and turns around
  INSIDE the same turn -- the turn's natural length is budgeted into the arrival's window
  (`, arrival turn included`) and it plays faster than the action's tempo
  (`TurnOnArrival(orient, gChainTurnSpeedUp)`, log: `Move: about-face queued for the
  landing 1.50x (0.71 s of the window).`). THAT IS WHAT MAKES THE PATROL'S LOOK LEGIBLE:
  standing at the line end it faces back down the line -- the tile it threatens -- and the
  rule "step where I am looking and I eat you" only reads if the turn has happened when
  the player plans their next move. A TRANSIT turn that arrives while the patrol is still
  walking into the end does not use it: it syncs the patrol onto that tile and the else
  branch below CHAINS the 180 into the walk in flight (`turning on the way`), so a player
  who keeps moving never makes the patrol spend a turn turning on the spot. AND ONCE IT IS
  AT THE LINE END IT DOES NOT STAND
  THERE: its `OnTurn` else branch decides the step BACK (`Linear: line end at +X;
  U-turn step back to (x, z).`) and lets the walk take the 180 WITH it -- a turn on
  the way when the patrol is still walking into the end (the chain turn curves it
  around just before the last tile), a turn in place before the step when it is
  already standing there. Only a true dead end (nothing behind it either) falls back
  to a bare `StartTurn`.
- `SeekerPatrol` decides at click time against the destination. Its arrival
  look is taken from the tile it WILL land on along the held heading
  (`SeesAlong(origin, dir, player)`); the actual turn to that heading runs
  ANIMATED the moment the move lands (`TurnOnArrival`), so the patrol arrives
  and then turns like the player would.

## Clip motion measurement (Codes/ClipMotion.h/cpp)

- One shared model of "what does a clip do to its actor", used by BOTH the walk
  and the execution: `ClipMotion { duration, totalTravel, keyTimes, keyTravel }`
  built by `MeasureClipMotion(anim)` from the clip's `rootKey` track (key time =
  `m_frame / fps`; travel = running max of the signed projection of the root
  position onto the clip's NET travel axis, clamped to the playable duration),
  plus `ClipMotion::TimeToTravel(distance)` and `MeasureClipRootTravel(record)`
  (net first-to-last key displacement).
- Nothing about a clip's length or reach is ever hardcoded: authoring a new
  `.anim` (or re-exporting one) changes the walk's natural duration and an
  execution's start distance by itself. Measurements happen at init / on
  resource reload, never per frame.
- `unit scale`: `MeasureClipMotion` returns `totalTravel == 0` for clips with no
  net horizontal travel (turn-in-place, idle, `rest`); callers must treat that
  as "cannot close a gap" (see `ClipMotion::HasTravel`).

## Execution system (Codes/Execution.h/cpp)

- An EXECUTION is a kill performed with authored animation instead of a plain
  step: the attacker closes in, the LAST stretch is covered by its strike clip's
  own root motion, and the victim plays the paired reaction clip at the same
  time. The player and every patrol can be either side; nothing in the system is
  player-specific or patrol-specific.
- THE CLIP SAYS WHERE THE STRIKE STARTS. `ExecutionClip::StartDistance()` is the
  strike's measured forward reach (`ClipMotion::totalTravel`), so the attacker
  walks until it is exactly that far from the victim and then plays the clip,
  whose root motion lands it on the victim's tile. Measured on the current
  assets: `ambush_1` 1.64 u (1.70 s), `ambush_2` 0.46 u (1.90 s), `ambush_3`
  1.59 u (1.70 s). Never write those numbers into code.
- THE RELATION PICKS THE CLIP, AND A SIDE STRIKE IS SET UP, NOT SKIPPED.
  `ExecutionLibrary::RelationOf(approach, victimFacing)` classifies a strike from
  the four grid sides of the victim: `Behind` = the attacker walks the way the
  victim faces (it comes up its back), `Front` = they walk into each other,
  `Left` / `Right` = the victim's shoulders (its right is its facing turned
  toward `+X x +Y`). `ExecutionLibrary::Resolve` returns an `ExecutionPlan`
  (the clip pair + how the scene has to be set up), and the catalogue in
  `Execution.cpp` maps each relation to its clip variants, the victim's reaction
  paired by INDEX:
  - `Behind -> ambush_1/2/3 + ambushed_1/2/3` (a strike from the back);
  - `Front -> execution_1..13 + executed_1..13` (a head on kill);
  - `Left` / `Right` have no scenes of their own, so a strike over a shoulder is
    turned INTO a head on kill instead of keeping the plain step: the plan's
    `victimTurnsToAttacker` is set, the victim turns its FRONT toward the
    attacker first, and the scene then plays as `Front` (`execution_N`).
  Adding a direction -- or new variants -- is a table row, not code: a relation
  whose variants list is not empty always plays them, so authoring shoulder clips
  later switches the shoulders back to their own scenes.
- THE VICTIM'S PRE-STRIKE TURN (the first step of a side strike): the attacker's
  `StartAction` calls `victim->TurnToFaceAttacker(dir, scale)` right after it has
  computed its own time scale -- so the turn plays at the ATTACKER'S tempo, in the
  SAME action window as the approach (a stand-alone turn would fill a whole
  `gTurnDuration` window for itself and would still be turning when the strike
  landed). `AnimatedUnit` plays it as the usual in-place turn clip + fold;
  `Unit` (no animation) snaps the front on instantly, and a victim that is
  already acting is left alone (the strike then takes it as it stands, logged).
  `AnimatedUnit::IsTurning()` (`m_turningInPlace`, set by `StartInPlaceTurn` and
  cleared by `FinishWalk`/`Reset`) reports that turn, and the walk context's
  `victimTurning` callback hands it to the strike.
- THE STRIKE WAITS FOR THAT FRONT. `ExecStrikeState` starts the strike clip the
  frame the victim's turn is over; while it is not, the attacker HOLDS where the
  approach left it (its outgoing walk clip's root motion is switched off so it
  does not walk on through its victim, and `ctx->waitingForVictim` keeps the
  stall watchdog quiet). The wait is bounded by the strike clip's own length, so
  a turn that never ends cannot lock the turn flow. In practice the hold is
  usually empty -- a 1.0 s turn clip at the action's ~1.7x tempo finishes at
  ~0.6 s of a ~2 s pre-strike approach.
- Variants take turns: `ExecutionLibrary::Resolve` cycles through a relation's
  variants (per session, reset by `ExecutionLibrary::Reset` in `Game::OnPlay`),
  so repeated kills do not replay the same scene -- with the 13 front variants a
  player eaten thirteen times sees all thirteen. A variant that cannot be played
  on the character asking for it (its record is not on the prefab, or the clip
  carries no root travel) is skipped, logged and the next one is tried, so a
  partly authored set still performs the scenes it has instead of degrading to a
  plain step.
- Every resolved clip is measured once per LOADED ANIMATION RESOURCE (the
  catalogue's motion cache) and logged the first time, so the measurement cost
  does not grow with the number of variants.
- Flow inside the walk (nothing is duplicated): `AnimatedUnit::StartExecution`
  resolves the relation + clip and hands both to `AnimatedUnit::StartAction`,
  the single implementation behind `StartWalk` and executions. The action runs
  through the SAME walk state machine -- turn, wind-up, stride loop -- and only
  its LANDING phase differs: `ExecStrikeState` takes the place of `WalkEndState`
  (it registers under the type name `"WalkEnd"`, exactly like
  `InPlaceTurnDoneState` registers as `"WalkStart"`), so the approach stops at
  `remaining <= strike reach` instead of the tile centre and the strike clip
  covers the rest. The natural duration is measured with the strike clip
  standing in for `walk_f_end`, so the action still closes in `gTurnDuration`
  with ONE scale (`ApplyMoveTimeScale` also drives the strike clip through
  `m_execSignal`).
- AN EXECUTION CLOSES IN WITH THE FIGHT WALK. `StartAction` puts
  `fight_walk_f` in the loop phase (`ctx->loopSignal`) instead of `walk_f`
  whenever the move carries a strike, so the attacker walks into its victim on
  the combat cycle rather than the travel cycle; the wind-up it starts with and
  the strike that ends it are unaffected. The clip is chosen by MEASUREMENT, not
  by name alone: `EnsureWalkTimings` measures it into `m_timingFightLoop` and
  `StartAction` picks it only when it is loaded and carries root travel
  (`HasTravel`), logging `Exec: closing in on 'fight_walk_f' (...)`; otherwise
  it keeps `walk_f` and logs why. Because the LOOP is what covers the gap, that
  same measured profile is what `EstimateWalkDuration` times the approach with,
  so a faster or slower combat cycle sizes its own approach and the action still
  closes in `gTurnDuration`.
- The victim side: when the strike phase begins, the attacker calls
  `victim->PlayExecutionReaction(signal, m_timeScale)` (through the
  `onStrike` callback in the walk context). `AnimatedUnit` plays that clip as a
  one-shot with root motion at the ATTACKER'S tempo and returns its length, so
  both halves of the scene stay in step. A victim without the clip returns 0 and
  the scene is just the strike.
- The scene outlives the walk machine: when the strike ends, `FinishWalk` snaps
  the attacker onto the victim's tile but SKIPS the idle settle and the tempo
  reset, so the attacker holds the strike's final pose (one-shot clips hold
  their last frame) while the victim's usually longer reaction plays out.
  `AnimatedUnit` counts that scene down (`m_execActive` / `m_execT` /
  `m_execDur`, advanced at the action's scale) and `IsExecuting()` reports it
  (throughout the whole action, see below). When the scene ends the attacker
  settles back into idle at 1x -- or, when the PLAYER is the attacker, the game
  cuts that wait short with `SettleExecutionScene` the moment its action ends
  (see below), so the turn never waits on the body.
- The kill lands on the LAST beat: `Game::UpdateActing` skips a bite while
  `bite->IsExecuting()` is true, so an execution ends with the full scene and
  then `Game::EatPlayer` (`Game: a patrol caught the player. You lose!`). A
  plain bite keeps the old rule: the eat lands the moment the enemy STANDS on
  the player's tile.
- WHAT THE PLAYER'S TURN WAITS FOR: its own ACTION, never the victim's death
  animation. The player's execution is resolved the frame its WALK MACHINE ends
  -- that machine IS the action (approach + strike) -- so its victim leaves the
  GRID, a win waiting on that tile is declared and the input comes back in that
  same frame, while a reaction clip that still had a second left plays on over
  the body (which is already out of the game, see "Removing an actor (corpse
  sink)"). `AnimatedUnit::SettleExecutionScene` closes the player's scene state
  right there (drops `m_execAction`/`m_execActive`, restores 1x and crossfades
  into idle -- the settle `FinishWalk` skipped because a scene was running), so a
  move committed immediately after can never be clobbered by a scene clock that
  would otherwise run out in the middle of it. The LOSE side is deliberately
  unchanged: a patrol striking the player still holds the loss until its whole
  scene has played out, because there is no next turn to hurry to.
- Where executions are used today: a step bite (`Game::ResolvePlayerArrival`)
  and a guard's lunge (`StationaryPatrol::Lunge(victim)`), each of which falls
  back to `StartMove` when no scene can be played for the relation. With the back
  and the head on series authored, a patrol following the player along a line
  strikes its back (`ambush_*`), a patrol that walks into the player -- or the
  player walking into a patrol -- strikes head on (`execution_*`), and a strike
  that comes over the victim's shoulder turns the victim to face the attacker
  first and then plays the same head on scene (`execution_*`, see above).
- The PLAYER strikes too. `Game::HandlePlayerClick` looks up the patrol on the
  clicked tile (`Game::EnemyOnTile`) and passes it to
  `Player::TryMove(node, isOccupied, victim)`: with a victim the step is tried
  as an execution FIRST -- the walk is the approach, the strike clip carries the
  player the last stretch onto the patrol -- and only a step with no playable
  scene falls back to the plain walk (which captures the patrol on arrival, as
  before). So a player that steps onto a guard from the tile BEHIND it (walking
  the way the guard faces) performs `ambush_N` on it and the guard plays
  `ambushed_N`, while a player that comes at it over a shoulder turns the guard
  to face it and then performs `execution_N`.
- A player execution is bookkept as `Game::m_executedEnemy`. The victim is
  frozen for the turn like any patrol standing on the destination, but it is
  NOT removed at arrival: it stays on the grid until the player's own action is
  over (`Game::FinishPlayerExecution`, called the frame the player's walk
  machine ends), which then drops it from the enemy list exactly like a captured
  patrol -- its BODY is laid to rest instead of deleted, see "Removing an actor
  (corpse sink)" -- settles the player back into idle (`SettleExecutionScene`)
  and declares a win that was waiting on that tile (`Game::TryWin`).
  `Game::UpdateActing` drives the player's `Frame` while it executes, and the
  turn's settle condition (`!m_player.IsExecuting()`) is satisfied by that
  settle -- so the input comes back in the very frame the player finished its
  kill.
- `AnimatedUnit::IsExecuting()` covers the WHOLE action -- approach, strike and
  the victim's reaction -- not just the scene (`m_execAction`), so callers can
  never resolve a turn on top of a half-played kill. `m_execActive` is the
  narrower "the strike scene is running now" flag the scene clock uses.

## Removing an actor (corpse sink, in Game.cpp)

- Nobody is deleted mid-scene anymore: an actor the game takes out of play (the
  victim of the player's execution, a CAPTURED patrol, the EATEN player) leaves
  the GAME at once -- it is dropped from `m_enemies` / reset, so no turn ever
  waits on a body and nothing reacts to a corpse -- but its ROOT ENTITY is not
  removed from the scene. `Game::LayCorpse(root, waitBeforeSink)` puts it in
  `Game::m_corpses` instead, which is what makes a kill read as a body sinking
  into the ground rather than an actor popping out of existence the frame the
  kill resolves.
- THE BODY LIES DOWN FIRST. `waitBeforeSink` is
  `Unit::ActiveAnimRemaining()` read at the moment of the kill: the wall clock
  seconds left in the DEATH animation the unit is playing (the reaction clip of
  an execution, at the tempo it was played with). `AdvanceCorpses` leaves the
  corpse untouched for that long, because a body that started sinking while it
  was still falling would slide through the floor mid-airs. A unit with no
  animation support, no active clip, or only a LOOPING clip (an idle loop never
  settles) reports 0 and its body sinks right away -- which is also the case for
  the eaten player, since the loss already waited for the whole strike scene.
- THEN IT SINKS: `gCorpseSinkDepth` world units (default 1.0) straight down over
  `gCorpseSinkDuration` seconds (default 2.0), both GLOBAL floats declared in
  `Game.h` / defined in `Game.cpp` and tunable at runtime like
  `gWalkBlendDuration`. One unit is enough to hide a body that is lying flat on
  the tile surface under the tile geometry. The motion is a straight world-space
  translation of the root (`laidAt - Vec3(0, depth * progress, 0)`), so a corpse
  never fights the walk/root-motion code; when it reaches the bottom the entity
  is removed from the scene with the same `Scene::RemoveEntity` the game used to
  call immediately.
- `Game::AdvanceCorpses` is the FIRST thing `Game::Frame` does, before the
  `m_won || m_lost` early return, so bodies keep sinking whatever the phase --
  a run that just ended must not freeze a corpse half way under the floor.
  Pausing still holds them, because `Frame` is not called while the simulation is
  not running.
- `Game::OnStop` removes the entities of the bodies still on their way down: a
  corpse must never survive into the next play session, where its (still tagged)
  root entity would be picked up as a living patrol, half buried.
- The corpse is deliberately an ENTITY, not a unit: the unit object is gone (the
  `unique_ptr` is erased, the player is reset) while the ENTITY keeps its
  `AnimControllerComponent` registered with the global `AnimationPlayer`, which
  is what lets the death animation keep playing over a body that is already out
  of the game.

## Transit walking -- chaining steps without stopping (Game.cpp / Unit.cpp)

- Clicking a tile while the player is STILL WALKING chains the walk into it: the
  character strides THROUGH the tile it was landing on and carries on, so walking
  ahead reads as one continuous walk instead of a stop sequence (`walk_f_end` +
  the idle settle + waiting for the next turn) on every tile. Input is otherwise
  still locked while a turn plays out -- the transit is the one thing a click
  during `TurnPhase::Acting` can do (`Game::Frame` -> `Game::TryTransit`; the click
  itself is resolved through `Game::ClickedTile`, which both paths share).
- THE TRANSIT IS A REAL TURN for everyone. The frame the player's walk chains
  (`Game::UpdateActing` -> `Player::ConsumeWalkChain`) `Game::BeginTransitTurn`
  restarts the turn state for the chained tile: every enemy decides its reaction
  against it exactly like a clicked move, and ACTS AGAIN. A chained step is
  therefore never a free step nobody reacts to -- the enemies keep moving while the
  player does. Nothing of the tile walked through has to be settled, because the
  conditions below guarantee it was a plain step.
- A UNIT THAT IS STILL MID-STEP DECIDES FROM THE TILE IT IS WALKING TO:
  `BeginTransitTurn` calls `Unit::SyncTileToMoveDestination()` before `OnTurn`, so
  a patrol that is halfway through its own step answers with its NEXT tile instead
  of re-deciding the step it is already walking (which used to make it stand still
  once it arrived -- the log symptom was a redundant
  `Move: (A) is taken right after the running action.` for the tile it was already
  heading to, followed by a `StartMove` to the tile it had just landed on).
- A UNIT MID-TURN DECIDES FROM THE DIRECTION ITS TURN IS TAKING IT TO: a chained step
  happens at the same moment the other units reach their own landing phase, so a line
  patrol that hits the end of its line is usually MID ABOUT-FACE when the transit turn
  arrives. `AnimatedUnit::GetFacingDir` therefore reports the turn's TARGET yaw while a
  turn is in flight (the walk context's `turnYawTo`), instead of snapping a half turned
  root to whichever axis it is nearer -- that snap is what once made the patrol answer
  with a step ACROSS its own line and re-point the very turn that was already playing.
  With the target heading the answer is the one the finished turn produces ("step back
  down the line"), and when the unit cannot chain onto it (its context is an in-place
  turn) the step waits for the landing and is taken then, so the patrol neither faces the
  wrong way nor loses the whole turn to it. There is no "mid-turn, do not ask" rule any
  more.
- EVERY UNIT CHAINS, NOT JUST THE PLAYER: `AnimatedUnit::StartMove` on a unit that
  is already walking no longer starts a second walk (that used to leak the running
  state machine and snap the actor) -- it arms a chain on the walk in flight, so
  the stride never breaks, landing phase included (see WalkEndState above). Only an
  in-place turn has nothing to chain onto: there the step waits in
  `m_moveAfterLanding` and `FinishWalk` takes it the moment the turn is over, so a
  unit handed one step after another never stands still between them.
- A DEFERRED STEP TAKES THE QUEUED ARRIVAL TURN WITH IT. The turn a decision queues
  (`TurnOnArrival` / a deferred `StartTurn`) was decided from the tile the current
  walk is heading to, so when that decision ALSO carries a step it belongs to the
  landing of THAT step. `FinishWalk` therefore SKIPS the deferred-turn block while a
  step is waiting (`m_moveAfterLanding != nullptr`) and leaves the turn queued; the
  walk taking the step lands and plays it there (its own budget already counts it in,
  see StartAction). The two ways of getting this wrong both showed up in play:
  CLEARING the flag dropped the about-face (a line patrol arrived at the end of its
  line, stood facing the old way and only turned on the next turn, walking back the
  turn after that), and CONSUMING it played the about-face on the wrong tile and then
  walked the unit back the way it had just turned.
- A CHAINED LEG IS RE-TIMED, because a chained step IS a new action: the units
  re-tasked with it were given a fresh `gTurnDuration` window the moment it was taken
  (`Game::BeginTransitTurn` -> `StartMove`), so the walk takes one too
  (`Move: chained leg retimed: natural X.XX s -> Y.YY s (xS.SS) to (A).`). The natural
  length of the new leg is measured from what the context already knows -- the turn it
  starts with, the stride's own rate over the distance the landing clip will not cover,
  the landing clip, AND the arrival turn queued for the end of that very leg
  (`WalkContext::pendingArrivalTurnDur`, published by TurnOnArrival, unit member
  `m_deferredTurnDur`): leaving that one out is how a line patrol's about-face came to run
  on PAST the end of the turn -- the walk took the whole window first and the turn then
  played on top of it, so the patrol finished after everybody else and the input came back
  late. The unit is re-scaled to the sum (`WalkContext::targetDur` /
  `applyTimeScale`, set by StartAction). Without the re-timing the leg ran on the tempo
  of the PREVIOUS action and whoever chained late came out behind everybody else: a
  line patrol taking its U-turn at the end of its line, halfway through a window it had
  almost used up, was still walking when the player's next leg was already over, so the
  turn could not close until it caught up. Early finishers just wait -- the rule is that
  nobody overruns the turn.
- A TURN TAKEN ON THE WAY IS PLAYED FASTER than the action's own tempo
  (`gChainTurnSpeedUp`, default 1.5) and moves the actor by
  `gChainTurnAdvance` (default 0) times the stride. Both exist because the turn clips
  are authored as full IN-PLACE turns: their bones step where the character stands, so
  travel under them is the body sliding with its feet planted (at full stride it slid
  ~1.4 u through a 180), while the clip itself costs a second or more of the action. The
  clip's multiplier and the FSM's turn timer are scaled together, so the fold still
  lands exactly when the turn finishes. Defaults: turn on the spot, played at 1.5x.
- FOLDING A TURN KEEPS THE ACTOR'S WORLD POSITION, AND READS IT FIRST. The actor's
  local translation lives in the ROOT's frame, so re-aiming the root (a 180 above all)
  turns that offset with it: the actor is thrown to the mirror point of its own offset
  -- the further the walk has already carried it, the further the jump (a chain turn on
  a long leg moved it double digit units and the stall watchdog snapped it onto the
  tile). `FoldTurnIntoRoot` therefore reads the world position BEFORE it re-aims the
  root, then puts the actor back on that exact point. Reading it after the re-aim
  (which is how the first version of the fix was written) returns the mirrored point and
  "keeping" it teleports the actor. The fold is a no-op for an actor sitting on the root
  (the leading turn of a walk, an in-place turn), which is why the problem only showed up
  once a turn was taken MID-walk, with the offset the root motion had piled up.
- RE-AIMING THE ROOT NEVER MOVES THE ACTOR (`SetRootYawKeepActor`). Every turn path
  that has no clip to rotate through root motion goes through it: the node-only
  fallback of `ChainTurnState`/`WalkTurnState`, and the small heading residual the
  turn clips cannot express (`TurnClipFor` rounds the delta to its 90/180 steps, so
  anything under 45 degrees comes back empty and is SNAPPED instead of turned). Both
  yaw the root while keeping the actor's world position, read BEFORE the re-aim -- the
  actor's local translation lives in the root's frame, so turning the root swings the
  offset the walk piled up on it (a ~5 degree correction mid-leg threw the character
  0.9 u sideways in 0.27 s, which reads as a jump).
- IN-PLACE TURNS ARE GUARDED: an in-place turn runs through the same walk context
  and state machine but goes nowhere (`WalkContext::inPlace`), so it is never a
  chain target and never a move destination. `AnimatedUnit::StartInPlaceTurn` on a
  unit that is MID-STEP defers to its landing (`m_deferredTurn`) instead of
  clobbering the running walk, and an EXECUTION is never chainable at all (its
  landing phase IS the strike clip: chaining would walk the attacker past its
  victim). Those three guards are what make it safe to hand a unit a new step or a
  turn while it is still finishing the previous one -- which is exactly what a
  transit turn does.
- ONLY A PLAIN STEP CHAINS. `Game::TryTransit` refuses -- and the click is then
  QUEUED instead (see below) -- when anything would have to be settled on the way:
  a patrol standing on the tile walked through or on the clicked tile, a patrol on
  its way onto either of them, a bite in flight or inbound (a guard's
  `ThreatTile()` on either tile, a step bite, an active bite), the player's own
  execution scene (`m_executedEnemy` / `IsExecuting`), or the tile walked through
  being the goal (`IsTargetNode`: the win has to resolve there).
- `GridGraph::Connected` IS NOT AN ADJACENCY TEST: it only asks whether a passage
  exists between the two tiles it is given (the facing flags), and it happily
  answers true for two tiles five units apart on the same line. A chain is ONE
  step, so `TryTransit` validates adjacency with the neighbour lookup
  (`m_grid.Neighbor(*through, dir) == node` for one of the four dirs, then
  `Connected`) exactly like `Player::TryMove` does. Skipping that check let a click
  two tiles ahead chain the walk straight across the tile between them -- which is
  also how a "player moved (4,3) -> (6,3)" line turned up in the logs.
- ONE CHAIN AT A TIME: `ArmWalkChain` refuses to re-point an armed chain, so a
  second click while one is pending cannot replace the tile the walk is about to
  chain into (the game queues that click for the next turn instead). It also refuses
  the tile the walk is already heading to, an in-place turn and any execution. It
  does NOT refuse a walk that is already landing any more -- `WalkEndState` takes an
  armed chain itself and hands back to the stride loop (a turn that hands a unit its
  next step a moment too late used to leave the walk stopping there and running a
  second action for the step, which put the whole turn a beat behind) -- and it does
  not refuse a U-turn HOME either: a line patrol at the end of its line steps back
  the way it came, and the 180 rides along with that step -- and neither does the
  PLAYER's own click path: clicking the tile the walk LEFT is a U-turn that chains
  like any other step (`TryTransit`), so the player does not have to finish the step
  it is on before it can go back. Putting a chain on the tile the walk is already
  heading for stays a no-op.
- A REFUSED CLICK IS NEVER DROPPED: it is kept in `Game::m_pendingMove` and made as
  the very first move of the next turn (`StartPlayerTurn` -> `CommitPlayerMove`),
  so a click that cannot transit simply costs the normal stop it was refused a
  transit for. The queued tile is re-validated when it is played (`Player::TryMove`
  is the only path that starts a move) and dropped with a log if it is no longer a
  legal step. A win or a loss clears the queue.
- HOW THE WALK CHAINS (nothing is duplicated): `Unit::ArmWalkChain(node)` stores
  the tile in the walk context (`WalkContext::chainTo`; `GetArmedWalkChain` reads
  it back, so the same click read on a later frame cannot arm twice), and the FSM
  TAKES it at the exact moment it would hand over to its landing phase --
  `WalkLoopState` and `WalkStartState`, plus the "arrived inside the loop" branch,
  all call `TakeChainedStep` BEFORE returning `WalkEnd`. That helper retargets the
  context (`to` / `targetPos` / `totalDist`, measured from where the actor actually
  is) and re-bases the stall watchdog; the state then keeps striding, so the stride
  clip (`walk_f`) is never replayed or even blended -- the chain is invisible in
  the animation, which is the entire point. A walk that is ALREADY in its landing
  phase takes an armed chain in `WalkEndState` instead (it hands back to the stride
  loop); that is the case a transit turn lands on when the unit it is asking reaches
  its own landing at the same moment, and waiting for the walk to end would have made
  it stop and run a second action for the new step.
- The actor is NEVER snapped onto the tile it walks through: `TakeChainedStep`
  measures the new leg from the actor's own position (it is one landing reach --
  `endReach`, ~0.39 u -- short of the tile center when the chain is taken) and
  `FinishWalk` only anchors it on the FINAL tile. `WalkContext::onChained` moves the
  unit's own tile bookkeeping (`m_node`) along with the walk, so the game's idea of
  "the tile the player stands on" matches the tile the walk passed through, and the
  next transit is validated from there.
- `Game::CommitPlayerMove` is the single "start the player's move" path (extracted
  from the click handler), so a queued move is committed exactly like a clicked
  one: with a victim it is the authored execution, otherwise a plain walk.

## Uniform turn duration (time scaling)

- Rule: EVERY action of a turn closes in exactly `gTurnDuration` seconds
  (global float, default 3.0, tunable like gWalkBlendDuration), whatever it is
  made of -- an in-place turn, a walk, or a walk plus the turn it lands into.
  ONE time scale per action is applied to all of its phases, so the clips keep
  their proportions relative to each other: a phase that is naturally longer
  simply loses more seconds, a short one loses fewer. Nothing is exempt and
  nothing finishes early: if the natural action is shorter than T its clips are
  slowed down to fill the window.
- The move's NATURAL duration is measured from the animation data, not
  hardcoded. `MeasureClipMotion` (ClipMotion.h/cpp: key time = frame / fps;
  travel = running max of the signed projection of the root position onto the
  net travel axis, clamped to the playable clip duration) is cached per unit in
  `AnimatedUnit::EnsureWalkTimings` at init and lazily when the clip resources
  (re)load. `EstimateWalkDuration` then sums the machine phases exactly the way
  the FSM gates them (optional turn duration + wind-up + as many stride cycles
  as needed + landing clip) into the natural length (a 5-unit move with a turn
  measures ~4.7 s natural on the current assets; without a turn ~3.7 s). An
  execution measures the same way with the strike clip standing in for the
  landing clip (a 5-unit rear strike measures ~3.9 s with `ambush_1`).
- `AnimatedUnit::StartWalk` (and `StartExecution`, through the shared
  `StartAction`) sums the move's natural duration AND the natural length of any
  queued arrival turn, then computes `scale = actionNatural / target`;
  `ApplyMoveTimeScale` stores it in `AnimatedUnit::m_timeScale` and writes it
  into the `m_timeMultiplier` of every clip that can play during the action
  (idle + the three walk clips + the stride an execution closes in with + the
  four turn clips + an execution's strike
  clip). The FSM timers AND the clip playback -- including blend countdowns --
  advance at that same rate, so the phases stay in sync while the action is
  compressed or stretched to T. `AnimatedUnit::Frame` feeds
  `dt * m_timeScale` to the machine; `FinishWalk` restores 1x before the idle
  settle blend (an execution keeps its tempo until its whole scene is over).
  Scale is per unit (record multiplier), so units never fight over one global
  speed.
- Moves run per unit: the player's `TryMove` and every enemy's `StartMove`
  (tile step, glide fallback and bite lunge alike) close in `gTurnDuration`;
  there is no second "temporary" move duration global.
- A move that ends with a queued in-place turn (line patrol about-face, seeker
  arrival turn) counts that turn's natural length INTO the same budget:
  `StartWalk` derives the turn clip length from the yaw between the walk's end
  facing and the queued orientation, and when the turn plays after the landing
  it reuses the walk's scale (`StartInPlaceTurn(yaw, explicitScale)`) -- so the
  walk and the turn share one tempo and together close in `gTurnDuration`.
- A stand-alone in-place turn (no step at all: about-face at a dead line end,
  seeker idle stare) fills the window on its own:
  `StartInPlaceTurn(yaw)` scales its clip to `gTurnDuration` through the same
  `ApplyMoveTimeScale`.
- HURRY SPEED: `gTurnSpeed` (1 = normal, 2 = hurried) is the GLOBAL time scale of the
  whole game, and it is set PER TURN. A double click -- two clicks inside
  `kFastClickSeconds` (0.45 s) of each other, whatever tile they hit, so the second
  click of a double click can be a transit and the same rule covers both -- raises it to
  2 in `Game::NoteClickSpeed`; `Game::StartPlayerTurn` puts it back to 1 for the next
  turn (a chain keeps the hurry, since a transit turn does not reset it). EVERYTHING is
  scaled together, which is the point: the machines read it in `AnimatedUnit::Frame`
  (`dt = deltaTime * m_timeScale * gTurnSpeed`) and the CLIPS carry it in their
  `m_timeMultiplier` (`ApplyMoveTimeScale` and `PlayExecutionReaction` fold it in; a
  speed change mid-action goes through `ReapplyTimeScale`), so an action that targets
  `gTurnDuration` simply closes in half the real time with its animation in step. The
  game also scales what IT advances -- the corpse sink and the master camera follow --
  with the same factor (`Game::Frame`).

## Master camera (Codes/FollowUpCameraController.h/cpp)

- The game can be played through a camera placed in the scene BY HAND instead of
  the editor's own view camera. The hook is a tag: `Game::SetupMasterCamera`
  (called at the end of `OnPlay`) looks up `scene->GetFirstByTag("master")` and, if
  that entity really is a camera (`SafeCast<Camera>` -- in ToolKit a camera IS an
  entity, `Camera : public Entity`, so `m_node` is its transform and
  `GetViewMatrix()` is the inverse of it), it does two things: `SetCamera` on the
  plugin's viewport (the run then renders through it) and `FollowUpCameraController
  ::Init`. A scene with no `master` entity -- or one that is not a camera -- is left
  alone completely: the viewport keeps the editor's camera and no follow runs.
- The viewport's own camera is remembered (`Game::m_editorCamera`) and put back by
  `Game::RestoreViewportCamera` from `OnStop`, and the master camera is put back on
  the spot it was authored at (`m_masterCameraHome`), so a session leaves neither
  the editor's view nor the scene changed. Note `ViewportBase::SetCamera` also
  clears any attached-camera id; the editor's view is the same camera object
  afterwards, so this is invisible.
- Clicking keeps working through the new camera for free: the game unprojects
  clicks with `m_viewport->RayFromMousePosition()`, which goes through
  `Viewport::RayFromScreenSpacePoint` -> `GetCamera()`. The renderer re-derives the
  lens per frame as `SetLens(camera->Fov(), aspect, ...)`, so the authored FOV is
  kept and only the aspect follows the viewport.
- `FollowUpCameraController` deliberately FOLLOWS instead of AIMING: `Init`
  captures the camera -> target offset it finds (which is exactly the framing the
  camera was authored with) and holds that offset while the target moves, so the
  placement is preserved (rotation is never touched) and the first update wants
  the camera precisely where it already is -- no start-up jump, no look-at roll
  guessing. DAMPING is a critically damped SPRING (the classic SmoothDamp: the camera
  keeps a velocity of its own and the spring is solved per call, so it is frame rate
  independent and never overshoots), not a per frame mix: the camera accelerates into a
  move -- so it trails BEHIND while the character gets going -- and its velocity decays
  into the framing when the move ends, which is the ease in / ease out a camera on rails
  has. `SetSmoothTime` (default 0.5 s) is roughly the time it takes to close the gap.
  Height is NOT ridden by default (`SetFollowHeight`): the walk cycle's bob and a
  body sinking into the ground must not move the view.
- WHAT THE CAMERA RIDES IS THE MOVE, NOT THE CHARACTER (`Unit::GetFollowPosition`). The
  animated actor is the obvious target -- it is what root motion actually moves -- but a
  camera on it inherits every wobble the animation carries: a turn in flight, the stride's
  bob, the lateral sway, the landing dip, and the offset the walk piles up on the actor
  node. A TILE is rock steady but only changes when the move LANDS, so the camera would sit
  still and then hop. So the point is the actor's progress ACROSS THE GRID: grid moves are
  axis aligned, so the leg is the line through both tile centers at the tile's height, and
  the point keeps the actor's coordinate ALONG that line and takes the other two from the
  destination tile. The camera therefore knows where the move is going from its first frame,
  glides there with the move at the move's own speed (a transit re-points the point at the
  next tile, continuously), and a hurried turn carries it quicker because the move does
  (`Game::Frame` scales the follow's delta by `gTurnSpeed` too). The framing is captured
  against that point (`SetupMasterCamera`), the damping smooths the result (see above), and
  the entity target (`Unit::GetFollowTarget`, the unit's root) stays as the controller's
  fallback.
- `Game::Frame` updates the follow right after the `m_won || m_lost` early return,
  i.e. only while the run is live: once the run is over the PLAYER's own body sinks
  into the ground (see "Removing an actor (corpse sink)") and a live follow would
  drag the whole view down with it.
- `Codes/CMakeLists.txt` lists the new source in both the plugin and the standalone
  target lists (and in `HEADERS`); a new game source that is missing there simply
  never gets compiled into the plugin.

## Logs and failure signatures

- `Move: natural X.XX s -> Y.YY s (xS.SS, T T.TT), turn yes/no` (plus
  `, arrival turn included` when a queued about-face/arrival turn was counted
  into the window, plus `, execution strike` when the landing phase is a strike
  clip): per-action timing, one per animated unit per turn. X is the natural
  duration measured from the clips (walk phases + any queued arrival turn), Y
  the duration it actually plays (equal to T) and S the applied time scale
  (below 1.00 = slowed down, above = sped up). A missing/wrong X means the clip
  timing profiles failed to build. Reference values for a 5-unit rear strike
  (`ambush_1`): natural ~3.9 s, x1.30.
- `Move: in-place turn NN deg (clip), plays Y.YY s (xS.SS)`: a stand-alone
  about-face filling its own T window.
- `Exec: measured 'ambush_1' for a strike from behind: 1.70 s, 1.636 u of
  forward travel.`: the clip measurement the start distance comes from (logged
  the first time a clip is resolved, once per loaded animation resource).
- `Exec: closing in on 'fight_walk_f' (1.10 s per 1.911 u stride cycle).`: the
  approach walks the fight cycle instead of `walk_f` (see "Execution system").
  `Exec: 'fight_walk_f' is not on this character` / `... carrying no root
  travel; closing in with 'walk_f'.`: the fallback -- the prefab has no such
  record, or the clip cannot cover a gap, so the approach travels normally.
- `Exec: strike from the behind -> 'ambush_1' + 'ambushed_1' (starts 1.64 u
  out).` / `Exec: strike from the left -> the victim shows its front, then
  'execution_7' + 'executed_7' (starts 1.2 u out).` / `Exec: no execution
  authored for a strike from the right; plain step.`: relation resolved, or a
  side strike set up as a head on kill (the victim turns first), or nothing
  playable at all (the caller keeps the plain step: a bite for a patrol, a
  capture for the player).
- `Exec: strike from the right: the victim turns to face the attacker first, then
  the strike plays as a front.` (from `ExecutionLibrary::Resolve`) and
  `Exec: the victim turns to face the attacker at x1.67.`: the side strike's
  setup. `Exec: the victim faces the attacker instantly (no turn animation).` /
  `Exec: the victim cannot turn now; the strike takes it as it stands.` /
  `Exec: the victim is still turning after X.XX s; striking anyway.`: the
  fallbacks -- the last one means the strike gave up waiting for the front
  (check the victim's turn clips and its action state).
- `Exec: 'execution_9' is not on this character; trying the next front variant.`
  / `Exec: 'x' carries no root travel; trying the next ... variant.` /
  `Exec: none of the 13 front variants is playable here; plain step.`: a
  variant was skipped, or the whole relation had nothing playable (check the
  prefab's records and the clip's root track).
- `Exec: strike 'ambush_1' started 1.64 u from the victim (1.70 s clip, 3.73 s
  scene).` / `Exec: victim reaction 'ambushed_1' playing (3.73 s at x1.30).` /
  `Exec: strike finished (elapsed ...)` / `Exec: scene finished after ...; the
  attacker settles back.`: the execution's beats. A scene that never finishes
  means `m_execDur` never elapsed -- check that the victim's reaction clip
  length is not being read as 0 when it should not be.
- `Move: walk started (...) -> (...), gap ..., end clip reach ...`: during a
  normal walk the gap must shrink every frame. In an execution this reach is
  the strike's own reach (1.64 u for `ambush_1`), i.e. where the approach stops
  and the strike takes over.
- `Move: walk stalled (gap ... not shrinking); snapping to the tile.`: root
  motion direction/orientation mismatch; check the prefab yaw and the facing
  convention.
- `Game: a guard strikes the player on (...).` / `Game: a patrol closes in on
  the player on (...).` / `Game: a patrol ambushes the player on (...).`: an eat
  is inbound (a bite move started after the player arrived); the loss lands the
  moment that enemy STANDS on the player's tile
  (`Game: a patrol caught the player. You lose!`), even if the enemy still had
  a landing turn queued -- it bites and stops. An ambush (execution) instead
  holds the loss until its whole strike + reaction scene has played.
- `Game: the player executed a patrol on (...).`: the player's own walk machine
  ended, so the kill resolved: the patrol left the game and, if a win was waiting
  on that tile, it was declared right after. Its death animation does not have to
  have finished -- it is already out of the game and its body sinks on its own
  from here (`Exec: the attacker's action is over; it settles into idle while the
  body plays on.` is the player settling at the same moment).
- `Game: player captured a patrol.`: the plain-step capture, i.e. a step the
  execution system could not play as a scene (no animation support on the
  player, or no playable variant for the relation) -- every animated player that
  steps onto a patrol now kills it with a scene instead.
- `Game: a body is laid to rest; it sinks X.XX u down in Y.YY s once its death
  animation has settled (Z.ZZ s to go).`: an actor left play and its entity was
  kept to sink (see "Removing an actor (corpse sink)"). One per kill / capture,
  plus `Game: the devoured player is out of the game; its body sinks.` for the
  loss. A `Z` that never counts down to 0 means `Unit::ActiveAnimRemaining`
  keeps reporting time left for the body that was just killed -- check that the
  death record is a one-shot (a looping clip reports 0 by design).
- `Game: a body sank into the ground and left the scene.`: the corpse reached the
  bottom and its entity was removed. A body that never logs this either never
  finished its wait or its root entity lost its node.
- `Game: rendering with the camera tagged 'master'; it follows the player from
  X.XX u away (smoothing Y.YY/s).`: the run took a scene camera over (see "Master
  camera"). `X` is the framing that will be kept, i.e. how far the camera was
  authored from the player -- keep an eye on it if the follow ever looks wrong
  (a camera placed at the player's own tile gives an offset of ~0). The two
  fallbacks, `Game: no entity tagged 'master'; keeping the viewport's own camera.`
  and `... the entity tagged 'master' is not a camera; ...`, mean the scene has
  nothing to play through and the editor's camera is untouched.
- `Move: walk chains through (A) -> (B), N.NN u to go.` (plus `, turning on the
  way` when the new leg points elsewhere): a TRANSIT was taken (see "Transit
  walking"): the walk passed through the tile it was landing on and strides on into
  the clicked one, with the stride clip never interrupted and the landing phase
  skipped. One line per chained step, from the walk machine itself.
  `Game: transit armed -- the walk through (A) goes on to (B).` is the click being
  accepted (before the walk gets there); `Game: transit turn -- the walk goes on to
  (B) heading +X; everyone acts again.` is the chained step opening a REAL turn for
  the enemies; `WalkState: transit turn done (...), N.NN u to (B) left.` closes the
  walk -> turn phase; `Move: (A) chains into the walk in flight instead of
  stopping.` / `Move: (A) is taken right after the running action.` are another
  unit's step joining a walk that is still running (the second one means it had to
  wait for the landing, i.e. that unit stops for at most one action). And
  `Game: (A) is not a transit; queued as the next move.` / `Game: playing the move
  queued during the last turn.` are the refused click being kept and then made as
  the next turn's move instead.
- `Move: in-place turn deferred to the landing (the unit is mid-step).`: a turn was
  asked of a unit that is walking; it is taken when that step lands (the guard that
  keeps a unit handed a new step mid-move from clobbering -- and leaking -- its own
  walk). The deferral has to run BEFORE `StartInPlaceTurn`'s "a state machine is
  already running" early return: walking means `m_walkSM != nullptr`, so checking
  that first made the turn vanish instead of being queued -- a line patrol that
  reached the end of its line during a transit turn just stood there facing the old
  way and only turned on the following turn. `Move: dropping the step queued during
  an execution -> (A).`: the opposite guard, for a step that waited behind a walk
  whose scene is still holding its strike pose -- it is dropped, together with the
  turn that went with it, rather than clobbered by that scene's settle.
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
- NEVER stage or commit agent/skill bookkeeping files: `.dsh-skill-memory.json`,
  `.dsh-skill-memory.config.json` and anything similar stay untracked -- the
  human commits those manually. Do not add them to `.gitignore` either, so they
  remain visible for that manual commit.
- If work is finished and commits are pending, summarize what is ready to be
  committed and ask whether to commit (and push) -- do not do it unilaterally.
