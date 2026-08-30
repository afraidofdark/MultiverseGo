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
    virtual void OnTurn(GridNode* playerNode, GridDir playerFacing) {}

    // Called every frame while this unit is the active one (player input).
    virtual void Frame(float deltaTime) {}

    // The tile whose occupation would make this unit eat the player: a static
    // guard zone. Null for units with no static threat (moving patrols
    // threaten by walking onto the player, not by a fixed zone).
    virtual GridNode* ThreatTile() const { return nullptr; }

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

    EntityPtr m_root;
    GridGraph* m_grid = nullptr;
    GridNode* m_node = nullptr;
    bool m_active = false;
  };

  // The player. Moves one tile per turn along connected tiles, driven by mouse
  // input. Movement is the player rule: exactly one tile, on a connected edge.
  class Player : public Unit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override;

    // True once the player has moved this turn.
    bool HasMoved() const { return m_hasMoved; }

    // Attempts to move one tile toward node. Valid only when node is the
    // current neighbour, both sides flag the facing connection, and isOccupied
    // (when provided) reports the node as free. Returns true when the player
    // moved. Only the first successful move of a turn counts.
    bool TryMove(GridNode* node, const std::function<bool(GridNode*)>& isOccupied);

    void Reset() override;

   private:
    bool m_hasMoved = false;
  };

  // A stationary enemy guard. It stands in place and watches the single tile
  // its facing passage opens onto: the tile is only threatened when the two
  // tiles are connected, because navigation is exclusively over connections.
  // Any unit that steps onto the watched tile is eaten; a unit that reaches
  // the patrol itself from any other direction captures it instead.
  class StationaryPatrol : public Unit
  {
   public:
    void OnTurn(GridNode* playerNode, GridDir playerFacing) override {}

    // The tile the patrol watches: its neighbour in the facing direction, but
    // only when the two tiles are connected. A player standing on it is eaten.
    // Null when the passage is blocked or the patrol stands at the grid edge.
    GridNode* ThreatTile() const override;
  };

  // A patrol that walks its line: one tile per turn along its facing direction,
  // turning 180 degrees in place when the connected line ends, then walking
  // back along it. Enemies never block each other, so it walks straight through
  // occupied tiles and eats the player by landing on its tile.
  class LinearPatrol : public Unit
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
  // of that heading is caught the instant the patrol gets there. Each homeward
  // step likewise arrives already turned toward the next step. If the player
  // shows up it keeps chasing, even mid-return. Back at the start it resumes
  // the idle stare.
  // Enemies do not block each other.
  class SeekerPatrol : public Unit
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
      Returning      // One homeward step per turn; each step arrives facing the next step. Still watching.
    };

    // True when the player's tile lies along a straight, connected line in the
    // current facing direction.
    bool CanSee(GridNode* playerNode) const;

    // Records a fresh sighting: the tile the player is on and the direction it
    // is heading this turn. The player typically crosses the line of sight in
    // a single turn, so the heading must be captured at the moment of the
    // sighting -- there may never be a second sighting to learn it from.
    void SpotPlayer(GridNode* playerNode, GridDir playerFacing);

    // Breadth-first path from the current node to the target along connected
    // neighbours. Empty when the target is unreachable or is the current node.
    std::vector<GridNode*> FindPath(GridNode* to) const;

    // Walks one tile toward the freshest sighting. On the turn it lands on the
    // target (or finds it unreachable) it turns to the frozen heading and looks
    // down it in that same turn: seen again means the chase goes on, an empty
    // line hands the patrol over to Returning.
    void StepChase(GridNode* playerNode, GridDir playerFacing);

    // Walks one tile back along the recorded path; arrival and turning toward the
    // next step happen together. Returns to Idle at the start.
    void StepReturn();

    // Rotates in place to face a grid direction.
    void TurnTo(GridDir dir);

    // Restores the authored orientation used when staring in Idle.
    void TurnToIdle();

    State m_state = State::Idle;
    GridNode* m_startNode = nullptr;      // Tile the patrol starts at and returns to.
    GridNode* m_lastSeen  = nullptr;      // Freshest tile the player was seen on.
    GridDir m_lastHeading = GridDir::Zm;  // Player's heading, refreshed while visible and frozen at the instant sight is lost.
    bool m_sighted = false;               // True while sight is live; triggers the heading snapshot exactly when LOS breaks.
    std::vector<GridNode*> m_trail;       // Nodes walked since leaving Idle.
    Quaternion m_idleOrientation;         // Authoring rotation, restored in Idle.
  };

} // namespace ToolKit
