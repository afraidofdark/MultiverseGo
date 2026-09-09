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
    // Turn flow: the player acts first and every enemy reacts to the committed
    // move. All moves of a turn then play out at the same time (the player
    // walks with its clips while enemies glide their tile steps); only eating
    // resolves after the player actually arrived on the destination tile.
    enum class TurnPhase
    {
      Idle,   // Not set up yet (no grid / player in the scene).
      Player, // Waiting for the player's single move.
      Acting  // The committed turn is playing out (parallel moves + bites).
    };

    void StartPlayerTurn();
    void HandlePlayerClick();

    // True when the node is blocked for the player's move: only the player's
    // own tile. Every other tile is reachable -- a free tile is a move, and a
    // patrol's tile is a capture attempt (resolved when the player arrives).
    bool IsMoveBlocked(GridNode* node) const;

    // Starts the acting phase for a committed player move toward dest: decides
    // every enemy's reaction NOW (against the destination the player will stand
    // on) and starts their non-bite moves so they run at the same time as the
    // player's walk. Eating moves are held back until the player arrives.
    void BeginPlayerMove(GridNode* dest);

    // Runs the part of the resolution that happens once the player physically
    // stands on its destination: patrols standing on the tile are captured and
    // removed, guards whose threat tile it is lunge, the win is checked, and
    // the patrols that decided to step onto the player's tile start their bite.
    void ResolvePlayerArrival();

    // Advances the acting phase: drives the player's walk and every enemy
    // glide, and settles the turn once nothing moves anymore.
    void UpdateActing(float deltaTime);

    // Direction the player faces when moving from -> to (grid axis).
    GridDir FacingToward(GridNode* from, GridNode* to) const;

    // True while any enemy is gliding a move.
    bool AnyEnemyMoving() const;

    // Ends the run with the player devoured. The player leaves the scene exactly
    // the way a captured patrol does -- its root entity is removed and the unit
    // forgets its tile -- so being eaten is visible instead of a figure frozen on
    // a tile.
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
    // other (several may occupy the same tile).
    std::vector<std::unique_ptr<Unit>> m_enemies;

    // Resolution bookkeeping for the turn being acted out.
    bool m_arrivalProcessed = false;   // Player already on its destination tile.
    // Patrols standing on the destination when the move committed: the player
    // captures them on arrival (they never act this turn).
    std::vector<Unit*> m_capturedEnemies;
    // Patrols that decided to STEP onto the player's destination: their bite
    // starts only once the player has arrived.
    std::vector<Unit*> m_stepBites;
    // Bite moves currently gliding (a guard's lunge or a step bite); the first
    // one to land eats the player.
    std::vector<Unit*> m_activeBites;

    EntityPtr m_target;      // Optional entity tagged "target".

    TurnPhase m_phase = TurnPhase::Idle;
    bool m_won        = false;
    bool m_lost       = false;
  };

} // namespace ToolKit

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance();
