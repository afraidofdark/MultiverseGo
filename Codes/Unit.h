/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include "ClipMotion.h"
#include "GridGraph.h"

#include <Entity.h>

#include <functional>

namespace ToolKit
{
  // Forward declarations for the animation walk state machine. The engine
  // provides the State/StateMachine building blocks and the
  // AnimControllerComponent that owns the animation records.
  class AnimControllerComponent;
  class AnimRecord;
  class StateMachine;

  // Defined in Execution.h: the plan of the authored strike StartAction plays
  // when a move is an execution instead of a plain step (only the pointer /
  // reference crosses here).
  struct ExecutionPlan;

  // Crossfade length (seconds) used whenever the walk state machine switches
  // clips (idle -> walk_f_start -> walk_f -> walk_f_end -> idle). Kept as a
  // global so it can be tuned at runtime (e.g. bound to a settings value);
  // defined in Unit.cpp, defaults to 0.2.
  extern float gWalkBlendDuration;

  // Target length (seconds) of every turn's action window. The player's walk
  // (including an in-place turn) is time-scaled to finish in exactly this long,
  // and every enemy tile step glides for the same duration, so all units of a
  // turn start and stop together. Tunable at runtime like gWalkBlendDuration.
  // Defined in Unit.cpp.
  extern float gTurnDuration;

  // How much of its stride a unit KEEPS while a chained step turns on the way (the
  // transit turn: walk -> turn -> walk instead of walk -> stop -> turn -> walk). 1
  // keeps the full stride and the character glides through the corner; 0 turns it on
  // the spot, which is what the turn clips are AUTHORED for -- their bones step where
  // the character stands, so any travel under them reads as the body sliding with its
  // feet planted. A small value gives a slight drift into the turn. Tunable at runtime
  // like gWalkBlendDuration; defined in Unit.cpp, defaults to 0.
  extern float gChainTurnAdvance;

  // How much FASTER a chained step plays the turn it takes on the way (1 = the
  // action's own tempo). The turn clips are authored as full in-place turns -- a
  // second or more for a 180 -- and spending that inside a chained step both delays
  // the stride and eats the action's window, so a turn taken on the way runs at this
  // multiple of the tempo (the clip and the state machine's turn timer are kept in
  // step, so the fold still lands exactly when the turn finishes). Tunable at runtime
  // like gWalkBlendDuration; defined in Unit.cpp, defaults to 1.5.
  extern float gChainTurnSpeedUp;

  // GLOBAL time scale for the whole game: 1 is normal play, 2 plays EVERYTHING at
  // double speed -- the walk machines and their scene clocks (AnimatedUnit::Frame),
  // every clip's playback (folded into the multipliers ApplyMoveTimeScale writes) and
  // whatever the game itself advances with the frame delta (the corpse sink, the camera
  // follow). ANIMATIONS AND TURN FLOW STAY IN STEP because both sides are scaled: a move
  // that targets gTurnDuration simply closes in half the real time. The game sets it per
  // TURN (a quick follow-up click means "hurry": see Game::NoteClickSpeed) and puts it
  // back to 1 when the turn ends.
  extern float gTurnSpeed;

  // Base class for every actor placed on the grid (player, enemies).
  //
  // Wraps the root entity of a placed prefab instance. The root node sits at
  // the top of the prefab hierarchy, holds the actor's world position, and
  // carries a tag that identifies the concrete type ("player",
  // "stationary-patrol"). Subclasses add their own behaviour on top.
  class Unit
  {
   public:
    Unit() = default;
    virtual ~Unit() = default;

    // Binds the unit to its root entity and the grid it lives on, then snaps it
    // onto the node it currently stands at. Returns false when the entity is
    // null or its position is not on the grid.
    virtual bool Init(EntityPtr root, GridGraph* grid);

    // Called when it becomes this unit's turn to act. playerNode is the
    // player's current tile and playerFacing the direction it is heading;
    // chasing units (SeekerPatrol) use them to see and pursue the player.
    // A unit that moves this turn does NOT move here: it records the tile it
    // intends to step to (GetIntendedMove) and the game starts the actual
    // (animated or gliding) move afterwards, so every unit of a turn can move
    // at the same time.
    virtual void OnTurn(GridNode* playerNode, GridDir playerFacing) {}

    // Called every frame while the unit is acting (walking or gliding a move).
    // The base implementation advances a running glide; AnimatedUnit overrides
    // it to drive the shared root-motion walk state machine instead.
    virtual void Frame(float deltaTime);

    // The node the unit decided to move to this turn (OnTurn output), or null
    // when it decided to stay. Cleared again by StartPlayerTurn.
    GridNode* GetIntendedMove() const { return m_intendedMove; }

    // True while the unit is animating/gliding a move between two nodes. The
    // turn flow keeps the acting phase open until every moving unit is done.
    virtual bool IsMoving() const { return m_gliding; }

    // Starts an animated step from the unit's current node to node over
    // duration seconds: the root glides along the gap and snaps onto the exact
    // node center on arrival. During the glide the unit's logical node (m_node)
    // stays the departure tile; PlaceOnNode updates it when the glide lands.
    // Enemies glide their tile steps until they get real walk state machines;
    // the player never glides (it walks with root motion instead). The game
    // starts the glides so every unit of a turn moves at the same time.
    void StartGlide(GridNode* node, float duration);

    // Starts a move to node. The base implementation glides for the target
    // length (targetDuration < 0 means gTurnDuration); AnimatedUnit overrides
    // it with the shared animated walk when the unit has an animation
    // controller. The game drives every enemy step through this, so all units
    // of a turn move the same way.
    virtual void StartMove(GridNode* node, float targetDuration = -1.0f);

    // Starts an EXECUTION of victim: instead of walking onto its tile, the unit
    // closes in and finishes the approach with the authored strike clip for the
    // relation between the two (which side of the victim it comes from), while
    // the victim plays the paired reaction -- one scene, one tempo.
    //
    // The clip itself says where the strike must start: its measured root
    // motion reach is the distance the unit keeps from the victim, so the
    // animation lands it exactly on top of its prey. Returns false when nothing
    // is authored for that relation (or the clips are not loaded), and the
    // caller keeps its plain bite: walk onto the victim's tile and eat.
    virtual bool StartExecution(Unit* victim, float targetDuration = -1.0f);

    // True while an execution started by this unit is still playing: the strike
    // and the victim's reaction are one scene, and the kill lands when it ends.
    virtual bool IsExecuting() const { return false; }

    // Ends the execution SCENE this unit holds -- the pose it keeps while its
    // victim's death animation plays out -- WITHOUT waiting for that animation.
    // The ACTION (the approach and the strike) is what a turn waits for; what the
    // victim plays afterwards is the victim's own business, so the attacker
    // settles back into idle the moment its own walk state machine ends, exactly
    // as it does at the end of any other move. No-op while no scene is running.
    virtual void SettleExecutionScene() {}

    // Plays the victim side of an execution -- the reaction clip paired with the
    // attacker's strike -- at the attacker's tempo, and returns its length in
    // machine seconds. 0 (and no-op) when this unit has no such clip, so the
    // attacker only has to wait for its own strike.
    virtual float PlayExecutionReaction(const String& signal, float scale) { return 0.0f; }

    // REAL seconds left in the animation this unit is playing right now -- its
    // DEATH animation, while this unit is dying. 0 when there is nothing to wait
    // for: no animation support, no active clip, or a LOOPING clip (an idle loop
    // never settles, so it is not a death animation).
    //
    // The game reads it the moment a kill resolves and waits that long before the
    // body starts sinking into the ground (Game::LayCorpse): the death animation
    // lays the victim down first, and a body that started sinking while it was
    // still falling would slide through the floor mid-airs.
    virtual float ActiveAnimRemaining() const { return 0.0f; }

    // Picks up a change of the global time scale (gTurnSpeed) in the middle of an
    // action. A unit with nothing running does not care.
    virtual void ReapplyTimeScale() {}

    // Turns in place to face a grid direction. The base implementation snaps
    // instantly (RotationTo on the top root); AnimatedUnit overrides it with
    // the shared in-place turn animation (turn clip + fold) when the unit has
    // turn clips, so every unit turns the same way the player does.
    virtual void StartTurn(GridDir dir);

    // Side strike setup: turns the unit in place so its FRONT points along
    // towardAttacker, the direction that leads from here to the attacker closing
    // in on it. This is the FIRST step of a strike that comes over this unit's
    // shoulder (see ExecutionPlan::victimTurnsToAttacker): the head on scene that
    // follows has to find the two facing each other. `scale` is the attacker's
    // action tempo -- the turn belongs to the SAME action window as the approach,
    // so unlike a stand-alone turn it must not fill a turn window of its own
    // (it would still be turning when the strike lands). The base implementation
    // snaps the root onto that heading: instant, so nothing has to wait for it.
    virtual void TurnToFaceAttacker(GridDir towardAttacker, float scale);

    // True while an in-place turn this unit is playing has not finished. An
    // attacker whose strike needs the victim's front (a side strike, see
    // TurnToFaceAttacker) holds its strike until this is false, so the reaction
    // clip never cuts a half finished turn.
    virtual bool IsTurning() const { return false; }

    // Lands a running move immediately (snaps the unit onto its destination
    // tile). Used when the run ends mid-move so no unit stays frozen between
    // two tiles. No-op when the unit is not moving.
    virtual void LandMove();

    // Drops any turn the unit queued to play when its move lands (a line
    // patrol's about-face, a seeker's arrival look). The game calls this when
    // the move turns out to be a bite: the eat ends the turn, so the unit
    // should bite and stop instead of turning afterwards. No-op on units with
    // nothing queued.
    virtual void CancelArrivalTurn() {}

    // The tile whose occupation would make this unit eat the player: a static
    // guard zone. Null for units with no static threat (moving patrols
    // threaten by walking onto the player, not by a fixed zone).
    virtual GridNode* ThreatTile() const { return nullptr; }

    // Performs this unit's bite: the forward lunge onto the tile it threatens,
    // made just before the player standing there is eaten, so the kill reads as
    // a real strike instead of an invisible rule. Only a guard that eats from a
    // static zone needs it -- a moving patrol already lunges onto the player as
    // its step, so it keeps this empty default. victim is the unit about to be
    // eaten, so the guard can perform the strike with the authored execution
    // for the relation between them instead of a plain step.
    virtual void Lunge(Unit* victim) {}

    // True while this unit is the active one and may act.
    void SetActive(bool active) { m_active = active; }
    bool IsActive() const { return m_active; }

    // The root entity of the placed prefab (carries the type tag).
    EntityPtr GetRoot() const { return m_root; }

    // The tag that identifies this unit's type on its root entity.
    String GetTypeTag() const;

    // One line of internal state, for the logs: where the unit stands, where it
    // faces, and everything it has in flight (the walk it is playing and its phase,
    // an armed chain, a queued arrival turn, a step waiting for a landing). The turn
    // logs print it so a whole turn can be read back from the log alone -- "it did
    // not turn around" is only diagnosable if the log says what the unit was doing.
    virtual String DescribeState() const;

    // Grid node the unit occupies, or null when it is not on the grid.
    GridNode* GetNode() { return m_node; }
    GridNode* GetNode() const { return m_node; }

    // The tile a running move is heading to, or null when the unit is standing. A
    // moving unit's own node is still the DEPARTURE tile until the move lands, so
    // this is the only way to ask where it is going. The game reads it for the
    // transit: the tile a walk is walking to is the tile it will be walked THROUGH
    // when the next click chains into the one beyond it.
    virtual GridNode* GetMoveDestination() const
    {
      return m_gliding ? m_glideNode : nullptr;
    }

    // Moves the unit's own tile bookkeeping onto the tile its running move is
    // heading to (no-op when it is standing, or when the action is an in-place turn
    // that goes nowhere).
    //
    // A unit handed a new turn while it is STILL MOVING has to decide from the tile
    // it will be standing on when that step lands -- not from the tile it is
    // leaving, which would make it decide the very step it is already walking
    // (Game::BeginTransitTurn does this before asking, so a patrol mid-step picks
    // its NEXT tile and chains into it instead of standing still on arrival).
    virtual void SyncTileToMoveDestination()
    {
      if (GridNode* dest = GetMoveDestination())
      {
        m_node = dest;
      }
    }

    // TRANSIT (see Game::TryTransit). Arms the WALK this unit is playing so that,
    // instead of landing on the tile it is walking to, it strides straight on into
    // node: the stride clip keeps playing across the boundary and the whole landing
    // phase -- the stop clip and the idle settle after it -- is skipped, so a chain
    // of straight steps reads as ONE continuous walk instead of a stop on every
    // tile. Returns false when there is nothing to chain (no walk in flight: a
    // glide, an actor without animation, a walk already in its landing phase) or
    // when node is already the tile being walked to. The CALLER owns the
    // conditions -- that the chained step is plain, straight and uninterrupted --
    // and the walk only carries it out.
    virtual bool ArmWalkChain(GridNode* node) { return false; }

    // The tile an armed chain will stride into instead of landing (see
    // ArmWalkChain), or null when nothing is armed. The walk clears it itself the
    // moment the chain is taken, so the game can read it to tell whether a chain
    // is still pending.
    virtual GridNode* GetArmedWalkChain() const { return nullptr; }

    // True when the walk in flight just CHAINED (see ArmWalkChain) since the last
    // call, reporting the tile it walked through (passed) and the tile it now
    // heads for (dest). The game consumes it to turn the chained step into a REAL
    // turn for everyone else (Game::BeginTransitTurn): a chained step is not a
    // free step nobody reacts to.
    virtual bool ConsumeWalkChain(GridNode** passed, GridNode** dest) { return false; }

    // The entity a follow camera should watch (Game::SetupMasterCamera). Animated
    // units return their ACTOR -- the skinned child that root motion actually
    // MOVES while the unit walks -- because the prefab top root only jumps to the
    // destination tile when the walk lands. Units without an actor fall back to
    // the root entity.
    virtual EntityPtr GetFollowTarget() const { return m_root; }

    // World position of the root entity.
    Vec3 GetWorldPosition() const;

    // The grid-axis direction the unit faces: its world forward (local -Z)
    // snapped to the nearest axis. Grid movement is axis-aligned, so a unit
    // faces either straight along X or straight along Z. AnimatedUnit overrides
    // it to report the direction a turn IN FLIGHT is taking the unit TO, so a
    // decision made in the middle of a turn is not read off a half rotated root.
    virtual GridDir GetFacingDir() const;

    // Clears the unit (used when a play session ends).
    virtual void Reset();

   protected:
    // Snaps the root entity onto the given node (world position = node center).
    void PlaceOnNode(GridNode* node);

    // Rotates the root entity so its forward (-Z) points along direction. Used
    // by PlaceOnNode whenever a unit steps from one node to another, so every
    // moving unit faces where it is going.
    void FaceTowards(const Vec3& direction);

    // Records an orientation to apply the moment a running move (glide or
    // animated walk) lands, after the step-facing -- e.g. a seeker that must
    // arrive already turned toward its held heading.
    void SetArrivalOrientation(const Quaternion& worldOrient);

    EntityPtr m_root;
    GridGraph* m_grid = nullptr;
    GridNode* m_node = nullptr;
    bool m_active = false;

    // The tile the unit decided to step to this turn (see OnTurn /
    // GetIntendedMove). Null while standing.
    GridNode* m_intendedMove = nullptr;

    // Glide state (see StartGlide / Frame). Only non-animated units glide.
    bool m_gliding = false;
    float m_glideDur = 1.0f;  // Total glide duration (seconds).
    float m_glideT = 0.0f;    // Progress in [0, 1].
    Vec3 m_glideFrom;         // Departure world position.
    Vec3 m_glideTo;           // Destination node center.
    GridNode* m_glideNode = nullptr; // Destination node, snapped on arrival.
    Quaternion m_arriveOrient;       // Orientation to apply on arrival.
    bool m_hasArriveOrient = false;
  };

  // A unit that moves its tile steps with the SHARED root-motion animation
  // state machine (optional in-place turn + wind-up / stride / landing clips),
  // falling back to a plain glide when its prefab has no animation controller.
  // The player and every patrol derive from this class so the whole movement
  // implementation -- FSM, timing measurement, per-turn duration scaling --
  // lives in one place instead of being duplicated per actor.
  class AnimatedUnit : public Unit
  {
   public:
    ~AnimatedUnit() override;

    // Binds the unit to its prefab top root, then finds the skinned actor
    // (the child entity that carries the animation controller) below it. When
    // such an actor exists the unit walks its steps with root motion; without
    // one it keeps gliding. Also settles the character into the idle loop and
    // measures the walk clip timings once.
    bool Init(EntityPtr root, GridGraph* grid) override;

    // True while a root-motion walk (a live walk state machine) is running, as
    // opposed to a glide.
    bool IsWalking() const { return m_walkSM != nullptr; }

    // Re-writes the current action's clip multipliers from the scale it was started
    // with, picking up a change of gTurnSpeed in the middle of a move (the game asks
    // for that when a double click hurries the turn).
    void ReapplyTimeScale() override;

    // The ACTOR entity when this unit has one, so a follow camera rides the
    // character root motion moves instead of the tile the top root stands on
    // (see Unit::GetFollowTarget).
    EntityPtr GetFollowTarget() const override
    {
      return (m_actor != nullptr) ? m_actor : m_root;
    }

    // True while any move (animated walk or glide) is running.
    bool IsMoving() const override { return m_walkSM != nullptr || Unit::IsMoving(); }

    // The tile the running walk is heading to (see Unit::GetMoveDestination); the
    // glide fallback answers through the base implementation.
    GridNode* GetMoveDestination() const override;

    // Arms a TRANSIT for the walk in flight (see Unit::ArmWalkChain): the walk
    // strides into node instead of landing on the tile it is walking to, skipping
    // the stop clip and the idle settle, so the two steps read as one walk. A
    // chained step into another direction turns on the way (walk -> turn) instead
    // of stopping first.
    bool ArmWalkChain(GridNode* node) override;
    GridNode* GetArmedWalkChain() const override;
    bool ConsumeWalkChain(GridNode** passed, GridNode** dest) override;

    // Drives a running walk state machine; advances the glide fallback when no
    // walk is running.
    void Frame(float deltaTime) override;

    // Starts a move to node. With an animation controller the move plays the
    // shared walk FSM, time-scaled so it finishes in exactly targetDuration
    // (default gTurnDuration); without one the unit glides for the same time.
    // Every non-bite tile step and every bite/lunge goes through here, so all
    // units of a turn use identical movement.
    void StartMove(GridNode* node, float targetDuration = -1.0f) override;

    // True while an execution this unit started is still playing: from the
    // approach, through the strike, until the victim's reaction is over (see
    // StartExecution). Nothing may resolve a turn on top of a half-played kill.
    bool IsExecuting() const override { return m_execAction; }

    // Ends the execution scene this unit holds without waiting for the victim's
    // death animation to finish (see Unit::SettleExecutionScene): it drops the
    // scene state, restores 1x and crossfades into the idle loop, exactly the way
    // the scene clock settles the attacker when the scene runs its course.
    void SettleExecutionScene() override;

    // Performs an execution of victim with the authored clip for the relation
    // between the two. The approach runs through the SAME walk state machine as
    // a normal step -- only its landing phase is the strike clip instead of
    // walk_f_end -- so the whole action keeps one tempo and closes in
    // gTurnDuration.
    bool StartExecution(Unit* victim, float targetDuration = -1.0f) override;

    // Plays the victim side of an execution and returns its length (see
    // Unit::PlayExecutionReaction).
    float PlayExecutionReaction(const String& signal, float scale) override;

    // Real seconds left in the clip this unit is playing, measured from the
    // animation controller's active record (see Unit::ActiveAnimRemaining). The
    // record's own time multiplier is divided out, so the caller gets wall clock
    // seconds whatever tempo the scene ran at.
    float ActiveAnimRemaining() const override;

    // Turns in place to face dir with the shared in-place turn animation
    // (turn clip + fold), or snaps instantly when no turn clips exist.
    void StartTurn(GridDir dir) override;

    // Turns to face the attacker of a strike that comes over this unit's
    // shoulder, with the shared in-place turn animation played at the attacker's
    // tempo (see Unit::TurnToFaceAttacker). Falls back to the instant snap when
    // the unit has no turn clips, and leaves a unit that is already acting alone
    // (the strike then takes it as it stands instead of hijacking its move).
    void TurnToFaceAttacker(GridDir towardAttacker, float scale) override;

    // True while the in-place turn this unit started is still playing (see
    // Unit::IsTurning): the strike that waits for this unit's front gates on it.
    bool IsTurning() const override { return m_turningInPlace; }

    // While a turn is IN FLIGHT the root sits between two grid axes, so this reports
    // the direction the turn is taking the unit TO (see Unit::GetFacingDir): a
    // decision taken in the middle of a turn is then the one the finished turn
    // produces, instead of a heading read off a half rotated root.
    GridDir GetFacingDir() const override;

    // State line for the logs (see Unit::DescribeState): adds the walk in flight and
    // its phase, the armed chain, the queued arrival turn and the queued step.
    String DescribeState() const override;

    // True when turn clips are loaded on this unit's animation controller, so
    // in-place turns (and the arrival turn of a landing patrol) can animate.
    bool HasAnimatedTurn() const;

    // Requests an ANIMATED in-place turn to face worldOrient the moment the
    // current move lands (used by patrols that must arrive and turn to their
    // held heading in one go). Falls back to the instant arrival orientation
    // (Unit::SetArrivalOrientation) when the unit cannot animate a turn.
    // `speedUp` plays that turn faster than the action's own tempo (the line
    // patrol's about-face uses it: it rides the arrival turn, so it has to be
    // quick to stay inside the same window).
    void TurnOnArrival(const Quaternion& worldOrient, float speedUp = 1.0f);

    // Lands whatever move is running (walk or glide) onto its destination tile.
    void LandMove() override;

    // Drops a queued arrival turn (m_deferredTurn) so the landed move does not
    // turn afterwards -- used when that move turned out to be a bite.
    void CancelArrivalTurn() override;

    // Stops the animation controller (used when play ends in the editor while
    // the scene entities are still alive).
    void StopAnimation();

    void Reset() override;

    // Shared data the walk state machine operates on. One instance lives per
    // walk (created by StartWalk, owned by the unit); the FSM states only
    // read/write this context and never reach into the unit. Defined in
    // Unit.cpp.
    struct WalkContext;

   protected:
    // Starts a root-motion walk toward node. Returns false when the unit has no
    // usable animation support (the caller picks the glide/instant fallback).
    // targetDuration < 0 means gTurnDuration.
    bool StartWalk(GridNode* node, float targetDuration);

    // True while the walk machine is in its LANDING phase (the stop clip is
    // playing): the walk ends on the tile it is on, so a chain armed now could
    // never be taken -- ArmWalkChain refuses and the caller queues the step.
    bool IsLanding() const;

    // The single implementation behind StartWalk and StartExecution: the shared
    // walk state machine, where the landing phase is either the walk's own stop
    // clip or -- when the plan carries a strike -- an authored execution strike
    // whose measured reach replaces the landing clip's. Keeping both in one place
    // is what makes an execution "a step that ends in a strike" instead of a
    // second movement implementation. A plan that asks for it also starts the
    // VICTIM's pre-strike turn (see ExecutionPlan::victimTurnsToAttacker) at this
    // action's tempo, and the strike phase then waits for that turn to end.
    bool StartAction(GridNode* node,
                     float targetDuration,
                     const ExecutionPlan& plan,
                     Unit* victim);

    // Ends the current walk: tears down the state machine and settles the
    // actor onto the destination node. forceSnap teleports (stalled/failed
    // walk); otherwise the actor is already within snap range of the center.
    void FinishWalk(bool forceSnap);

    // Builds/refreshes the walk clip timing profiles (m_timingStart/Loop/End)
    // from the animation controller's loaded clips. Rebuilt whenever the clip
    // resources change (they load once per session, so this runs at most a few
    // times).
    void EnsureWalkTimings();

    // Plays an in-place turn to the given absolute world yaw (radians) with
    // the shared turn phase: the turn clip rotates the actor through root
    // motion and the fold lands the top root exactly on the target yaw. Runs
    // the same StateMachine building blocks as a normal walk, only without the
    // walking phases.
    //
    // Rule: every action closes in gTurnDuration. A stand-alone turn therefore
    // scales its clip to fill that window (explicitScale < 0, the default); a
    // turn that belongs to an action already running -- an arrival turn whose
    // natural length was budgeted into the walk -- passes that action's scale
    // (explicitScale > 0) so the whole action keeps ONE tempo.
    void StartInPlaceTurn(float targetYaw, float explicitScale = -1.0f);

    // Applies a move time scale: stores it for the FSM timers and writes it into
    // the m_timeMultiplier of every clip that can play during a move (idle +
    // the three walk clips + the stride an execution closes in with + the four
    // turn clips + the strike). 1.0 restores normal speed. gTurnSpeed is folded in
    // here, so the clips always run at the tempo the machine does.
    void ApplyMoveTimeScale(float scale);

    StateMachine* m_walkSM = nullptr;        // Walk FSM while a move animates.
    WalkContext* m_walkCtx = nullptr;        // Shared data for the FSM states.
    AnimControllerComponent* m_walkAnim = nullptr; // Actor's animation controller.

    // The skinned actor that carries the animation controller. It is a child
    // of the prefab top root (m_root) in the character prefabs; root motion
    // moves its node in local space while the top root gives the direction.
    EntityPtr m_actor;
    // The actor's authored local translation inside the prefab. Restored when
    // a walk ends so the accumulated root-motion offset folds back into the
    // top root and future turns start from a clean frame.
    Vec3 m_actorLocalBase;

    // Measured timing of the three walk clips (see ClipMotion). Cached per
    // AnimRecord instance; EnsureWalkTimings rebuilds a profile when its clip
    // record changes.
    ClipMotion m_timingStart;
    ClipMotion m_timingLoop;
    ClipMotion m_timingEnd;
    const AnimRecord* m_timedStartRec = nullptr;
    const AnimRecord* m_timedLoopRec = nullptr;
    const AnimRecord* m_timedEndRec = nullptr;

    // Measured motion of the stride an EXECUTION closes in with (fight_walk_f),
    // measured exactly like the walk clips. No travel means the clip cannot
    // cover a gap, and the approach keeps walk_f (see StartAction).
    ClipMotion m_timingFightLoop;
    const AnimRecord* m_timedFightLoopRec = nullptr;

    // Time scale of the running walk: real duration = natural FSM duration /
    // m_timeScale. Set by StartWalk so the whole move (turn + walk) finishes in
    // exactly the requested target duration; reset to 1.0 when the walk ends.
    float m_timeScale = 1.0f;

    // Animated in-place turn requested to run the moment the current move
    // lands (see TurnOnArrival). Consumed by FinishWalk.
    Quaternion m_deferredTurn;
    bool m_hasDeferredTurn = false;
    // How much faster than the action's own tempo that turn plays (1 = same tempo).
    float m_deferredTurnSpeedUp = 1.0f;
    // How long that turn takes at the clip level (the clip the yaw picks for the turn,
    // or the node-only fallback length, divided by the speed-up). A CHAINED leg needs it
    // to keep room for the queued turn inside its own window (see TakeChainedStep):
    // without it the walk filled the whole window and the about-face ran on past the end
    // of the turn, which is what made a line patrol finish after everybody else.
    float m_deferredTurnDur = 0.0f;

    // A step handed to this unit while its walk was LANDING (so it was too late to
    // chain, see StartMove): taken by FinishWalk the moment the walk is over, so a
    // unit that is given one step after another never stands still between them.
    GridNode* m_moveAfterLanding = nullptr;

    // The last chained step of the walk in flight, waiting to be consumed by the
    // game (see ConsumeWalkChain): set by the walk context's chain callback.
    bool m_chainEvent     = false;
    GridNode* m_chainPassed = nullptr;
    GridNode* m_chainDest   = nullptr;

    // True while an in-place turn this unit started (a stand-alone about-face, a
    // seeker's stare, or the pre-strike turn of a side strike) is still playing.
    // Reported by IsTurning() and cleared the moment that turn's action ends.
    bool m_turningInPlace = false;

    // Execution state (see StartExecution). The action itself runs in the walk
    // state machine; m_execActive tracks the SCENE that outlives it -- the
    // attacker holds its strike pose while the victim's (usually longer)
    // reaction plays out -- so IsExecuting() stays true until the last beat of
    // the kill, from the very start of the approach.
    bool m_execAction = false;    // The action being played is an execution.
    bool m_execActive = false;    // Its strike scene is running right now.
    float m_execT     = 0.0f;     // Scene time so far (machine seconds, scaled).
    float m_execDur   = 0.0f;     // Length of the whole scene (machine seconds).
    // Strike clip of the running scene, added to the clips ApplyMoveTimeScale
    // drives so the strike keeps the action's tempo like every walk phase.
    String m_execSignal;
  };

  // The player. Moves one tile per turn along connected tiles, driven by mouse
  // input. Movement is the player rule: exactly one tile, on a connected edge,
  // animated by the shared AnimatedUnit walk (root motion) -- or an instant
  // snap on actors without animation support.
  class Player : public AnimatedUnit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override;

    // True once the player has moved this turn.
    bool HasMoved() const { return m_hasMoved; }

    // Attempts to move one tile toward node. Valid only when node is the
    // current neighbour, both sides flag the facing connection, and isOccupied
    // (when provided) reports the node as free. On the animated player this
    // starts a root-motion walk instead of snapping; returns true when the
    // move was accepted (walking or, without animation support, already
    // landed). Only the first successful move of a turn counts.
    //
    // victim is the unit standing on node, when there is one: the step is then
    // FIRST tried as an execution (the walk approaches and the authored strike
    // clip carries the player the last stretch onto its prey), which is how the
    // player sneaks up on a patrol's back. Anything the execution does not fit
    // (a frontal/side step, a missing clip) falls through to the plain walk,
    // and the patrol standing there is captured on arrival as before.
    bool TryMove(GridNode* node,
                 const std::function<bool(GridNode*)>& isOccupied,
                 Unit* victim = nullptr);

    void Reset() override;

   private:
    bool m_hasMoved = false;
  };

  // A stationary enemy guard. It holds its post and watches the single tile its
  // facing passage opens onto: the tile is only threatened when the two tiles
  // are connected, because navigation is exclusively over connections. Any unit
  // that steps onto the watched tile is eaten, and the guard does not eat from
  // where it stands -- it lunges the one tile forward onto its prey as it
  // strikes. A unit that reaches the patrol itself from any other direction
  // captures it instead.
  class StationaryPatrol : public AnimatedUnit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override {}

    // The tile the patrol watches: its neighbour in the facing direction, but
    // only when the two tiles are connected. A player standing on it is eaten.
    // Null when the passage is blocked or the patrol stands at the grid edge.
    GridNode* ThreatTile() const override;

    // The bite: the animated step (or glide fallback) onto the watched tile, or
    // -- when an execution is authored for the relation between the guard and
    // its prey -- the authored strike instead. Does nothing when there is no
    // watched tile to lunge into.
    void Lunge(Unit* victim) override;
  };

  // A patrol that walks its line: one tile per turn along its facing direction,
  // turning 180 degrees in place (animated, like the player) when the connected
  // line ends, then walking back along it. Enemies never block each other, so
  // it walks straight through occupied tiles and eats the player by landing on
  // its tile.
  class LinearPatrol : public AnimatedUnit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override;
  };

  // A patrol that stares at a fixed point across the grid and investigates what
  // it sees. Vision is live: in every state it watches its line of sight (a
  // straight, connected passage in its facing direction) and chases the
  // freshest sighting. The player's heading is refreshed while it is visible
  // and frozen at the moment sight is lost -- the direction the player was
  // moving as it left the view, not the stale heading from the last visible
  // tile (that one is the direction the player arrived FROM, usually straight
  // toward the patrol). The patrol turns to that frozen heading and looks down
  // it on the very turn it lands on the last sighting tile -- arriving, turning
  // and seeing are one turn, never three -- so a player running straight ahead
  // of that heading is caught the instant the patrol gets there. Catching is the
  // end of the hunt: a patrol that lands on the player's tile has eaten it and
  // stops dead where it arrived, keeping the facing it walked in with instead of
  // turning on the body. An empty line is not given up on at once: the patrol
  // stands on that angle, motionless, for as many turns as kWatchTurns counts,
  // taking a fresh look down the same line each one, and only hands itself to
  // Returning once the wait is spent -- so the homeward walk never shares a turn
  // with the wait. Each homeward step likewise arrives already turned toward the
  // next step. If the player shows up it keeps chasing, even mid-wait or
  // mid-return. Back at the start it resumes the idle stare.
  // Enemies do not block each other.
  class SeekerPatrol : public AnimatedUnit
  {
   public:
    bool Init(EntityPtr root, GridGraph* grid) override;
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override;
    void Reset() override;

   private:
    enum class State
    {
      Idle,          // Staring at its fixed point.
      Chasing,       // Walking to the freshest tile where it sees the player.
      Watching,      // Standing on the arrival tile, staring down its held heading. Still watching.
      Returning      // One homeward step per turn; each step arrives facing the next step. Still watching.
    };

    // How many turns the patrol stands motionless on the tile it arrived at,
    // staring down the heading frozen at sight loss and taking one fresh look per
    // turn, before it gives the chase up and turns back. Raise it to make the
    // patrol hang around the last sighting longer; a player that steps back into
    // that line during the wait is caught and the chase resumes immediately.
    static constexpr int kWatchTurns = 1;

    // True when the player's tile lies along a straight, connected line in the
    // current facing direction (a standing look from the patrol's own tile).
    bool CanSee(GridNode* playerNode) const;

    // Line of sight from an explicit origin tile along an explicit grid
    // direction. Used for the look taken the turn the patrol lands on the last
    // seen tile, which is decided before the move actually lands.
    bool SeesAlong(GridNode* origin, GridDir dir, GridNode* playerNode) const;

    // Records a fresh sighting: the tile the player is on and the direction it
    // is heading this turn. The player typically crosses the line of sight in
    // a single turn, so the heading must be captured at the moment of the
    // sighting -- there may never be a second sighting to learn it from.
    void SpotPlayer(GridNode* playerNode, GridDir playerFacing);

    // Breadth-first path from the current node to the target along connected
    // neighbours. Empty when the target is unreachable or is the current node.
    std::vector<GridNode*> FindPath(GridNode* to) const;

    // Walks one tile toward the freshest sighting. Landing on the player's tile
    // is the bite: the patrol stops there facing the way it walked in, with no
    // turn and no look. Otherwise, on the turn it lands on the target (or finds
    // it unreachable) it turns to the frozen heading and looks down it in that
    // same turn: seen again means the chase goes on, an empty line leaves the
    // patrol holding that heading for its kWatchTurns watching turns.
    void StepChase(GridNode* playerNode, GridDir playerFacing);

    // Walks one tile back along the recorded path; arrival and turning toward the
    // next step happen together. Returns to Idle at the start.
    void StepReturn();

    // Rotates in place to face a grid direction.
    void TurnTo(GridDir dir);

    // World orientation that makes the unit's forward (-Z) point along dir.
    Quaternion HeadingRotation(GridDir dir) const;

    // Restores the authored orientation used when staring in Idle.
    void TurnToIdle();

    State m_state = State::Idle;
    GridNode* m_startNode = nullptr;      // Tile the patrol starts at and returns to.
    GridNode* m_lastSeen  = nullptr;      // Freshest tile the player was seen on.
    GridDir m_lastHeading = GridDir::Zm;  // Player's heading, refreshed while visible and frozen at the instant sight is lost.
    bool m_sighted = false;               // True while sight is live; triggers the heading snapshot exactly when LOS breaks.
    int  m_watchLeft = 0;                 // Turns of the wait still owed on the held heading.
    std::vector<GridNode*> m_trail;       // Nodes walked since leaving Idle.
    Quaternion m_idleOrientation;         // Authoring rotation, restored in Idle.
  };

} // namespace ToolKit
