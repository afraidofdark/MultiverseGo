/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

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

  // How long an enemy tile step (or a guard's lunge) glides, in seconds.
  // Temporary stand-in until patrols get their own walk state machines /
  // animation; the player keeps its real root-motion walk clips. Defined in
  // Unit.cpp. (Bite lunges happen AFTER the action window, so they keep this
  // length instead of the turn duration.)
  extern const float gPatrolGlideTime;

  // Measured timing model of one walk clip: how much horizontal root travel the
  // clip's root key makes over its key frames, and when. Built from the actual
  // animation data (never hardcoded) so the walk's natural duration can be
  // predicted for time scaling.
  struct WalkClipTiming
  {
    float duration = 0.0f;        // Clip duration (seconds).
    float totalTravel = 0.0f;     // Net horizontal root travel over the clip.
    std::vector<float> keyTimes;  // Time of each root key (seconds).
    std::vector<float> keyTravel; // Cumulative horizontal travel at each key.

    // Clip time (seconds) the clip needs to travel the given horizontal
    // distance from its first frame. 0 when the clip has no usable root track.
    float TimeToTravel(float distance) const;
  };

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

    // Lands a running move immediately (snaps the unit onto its destination
    // tile). Used when the run ends mid-move so no unit stays frozen between
    // two tiles. No-op when the unit is not moving.
    virtual void LandMove();

    // The tile whose occupation would make this unit eat the player: a static
    // guard zone. Null for units with no static threat (moving patrols
    // threaten by walking onto the player, not by a fixed zone).
    virtual GridNode* ThreatTile() const { return nullptr; }

    // Performs this unit's bite: the forward lunge onto the tile it threatens,
    // made just before the player standing there is eaten, so the kill reads as
    // a real strike instead of an invisible rule. Only a guard that eats from a
    // static zone needs it -- a moving patrol already lunges onto the player as
    // its step, so it keeps this empty default.
    virtual void Lunge() {}

    // True while this unit is the active one and may act.
    void SetActive(bool active) { m_active = active; }
    bool IsActive() const { return m_active; }

    // The root entity of the placed prefab (carries the type tag).
    EntityPtr GetRoot() const { return m_root; }

    // The tag that identifies this unit's type on its root entity.
    String GetTypeTag() const;

    // Grid node the unit occupies, or null when it is not on the grid.
    GridNode* GetNode() { return m_node; }
    GridNode* GetNode() const { return m_node; }

    // World position of the root entity.
    Vec3 GetWorldPosition() const;

    // The grid-axis direction the unit faces: its world forward (local -Z)
    // snapped to the nearest axis. Grid movement is axis-aligned, so a unit
    // faces either straight along X or straight along Z.
    GridDir GetFacingDir() const;

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

    // True while any move (animated walk or glide) is running.
    bool IsMoving() const override { return m_walkSM != nullptr || Unit::IsMoving(); }

    // Drives a running walk state machine; advances the glide fallback when no
    // walk is running.
    void Frame(float deltaTime) override;

    // Starts a move to node. With an animation controller the move plays the
    // shared walk FSM, time-scaled so it finishes in exactly targetDuration
    // (default gTurnDuration); without one the unit glides for the same time.
    // Every non-bite tile step and every bite/lunge goes through here, so all
    // units of a turn use identical movement.
    void StartMove(GridNode* node, float targetDuration = -1.0f) override;

    // Lands whatever move is running (walk or glide) onto its destination tile.
    void LandMove() override;

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

    // Ends the current walk: tears down the state machine and settles the
    // actor onto the destination node. forceSnap teleports (stalled/failed
    // walk); otherwise the actor is already within snap range of the center.
    void FinishWalk(bool forceSnap);

    // Builds/refreshes the walk clip timing profiles (m_timingStart/Loop/End)
    // from the animation controller's loaded clips. Rebuilt whenever the clip
    // resources change (they load once per session, so this runs at most a few
    // times).
    void EnsureWalkTimings();

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

    // Measured timing of the three walk clips (see WalkClipTiming). Cached per
    // AnimRecord instance; EnsureWalkTimings rebuilds a profile when its clip
    // record changes.
    WalkClipTiming m_timingStart;
    WalkClipTiming m_timingLoop;
    WalkClipTiming m_timingEnd;
    const AnimRecord* m_timedStartRec = nullptr;
    const AnimRecord* m_timedLoopRec = nullptr;
    const AnimRecord* m_timedEndRec = nullptr;

    // Time scale of the running walk: real duration = natural FSM duration /
    // m_timeScale. Set by StartWalk so the whole move (turn + walk) finishes in
    // exactly the requested target duration; reset to 1.0 when the walk ends.
    float m_timeScale = 1.0f;
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
    bool TryMove(GridNode* node, const std::function<bool(GridNode*)>& isOccupied);

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

    // The bite: starts the animated step (or glide fallback) onto the watched
    // tile. Does nothing when there is no watched tile to lunge into.
    void Lunge() override;
  };

  // A patrol that walks its line: one tile per turn along its facing direction,
  // turning 180 degrees in place when the connected line ends, then walking
  // back along it. Enemies never block each other, so it walks straight through
  // occupied tiles and eats the player by landing on its tile.
  class LinearPatrol : public AnimatedUnit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override;

   private:
    // Rotates the unit 180 degrees around Y, to face back along its line.
    void FlipFacing();
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
