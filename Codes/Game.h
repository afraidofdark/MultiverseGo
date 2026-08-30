/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include <Plugin.h>
#include <ToolKit.h>

#include <memory>

#include "GridGraph.h"
#include "Unit.h"

namespace ToolKit
{

  class Game : public GamePlugin
  {
   public:
    void Init(Main* master) override;
    void Destroy() override;
    void Frame(float deltaTime) override;
    void OnLoad(XmlDocumentPtr state) override;
    void OnUnload(XmlDocumentPtr state) override;
    void OnPlay() override;
    void OnPause() override;
    void OnResume() override;
    void OnStop() override;

   private:
    // Turn flow: the player acts, then every enemy takes its fixed action, then
    // the turn comes back to the player.
    enum class TurnPhase
    {
      Idle,   // Not set up yet (no grid / player in the scene).
      Player, // Waiting for the player's single move.
      Enemies // Enemies acting in sequence.
    };

    void StartPlayerTurn();
    void EndPlayerTurn();
    void HandlePlayerClick();

    // True when the node is blocked for the player's move: only the player's
    // own tile. Every other tile is reachable -- a free tile is a move, and a
    // patrol's tile is a capture attempt (see ResolvePatrolContact). This is
    // the predicate Player::TryMove treats as "occupied".
    bool IsMoveBlocked(GridNode* node) const;

    // Resolves the patrol reactions to the player's move, in stacked order:
    // first a patrol the player stepped onto is captured and leaves the grid,
    // then the remaining patrols eat the player if any watches its tile. So a
    // single move can both capture a patrol and die to another. Returns true
    // when the game is over.
    bool ResolvePatrolContact();

    // Ends the run with the player devoured. The player leaves the scene exactly
    // the way a captured patrol does -- its root entity is removed and the unit
    // forgets its tile -- so being eaten is visible instead of a figure frozen on
    // a tile. Every loss goes through here: a guard's bite in
    // ResolvePatrolContact and a moving patrol landing on the player in
    // EndPlayerTurn.
    void EatPlayer();

    // True when the node is the one the target marker stands on.
    bool IsTargetNode(GridNode* node) const;

    GridGraph m_grid;
    Player m_player;

    // The tile the player stood on at the start of its turn, before its move.
    // Used to log the actual move of each turn (from -> to + heading), so the
    // enemy logs that freeze the player's heading can be read against it.
    GridNode* m_prevPlayerNode = nullptr;

    // Every enemy in the scene, regardless of type. Enemies never block each
    // other (several may occupy the same tile), so the enemy phase lets them
    // all act from the state at the start of the phase.
    std::vector<std::unique_ptr<Unit>> m_enemies;

    EntityPtr m_target;      // Optional entity tagged "target".

    TurnPhase m_phase = TurnPhase::Idle;
    bool m_won        = false;
    bool m_lost       = false;
  };

} // namespace ToolKit

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance();
