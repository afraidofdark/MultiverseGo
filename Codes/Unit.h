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

    // Called when it becomes this unit's turn to act.
    virtual void OnTurn() {}

    // Called every frame while this unit is the active one (player input).
    virtual void Frame(float deltaTime) {}

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

    // Clears the unit (used when a play session ends).
    virtual void Reset();

   protected:
    // Snaps the root entity onto the given node (world position = node center).
    void PlaceOnNode(GridNode* node);

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
    void OnTurn() override;

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

  // A stationary enemy guard. Its fixed patrol behaviour comes later; for now it
  // stands in place and simply ends its turn.
  class StationaryPatrol : public Unit
  {
   public:
    void OnTurn() override {}
  };

} // namespace ToolKit
