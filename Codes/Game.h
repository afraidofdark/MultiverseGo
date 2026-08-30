/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include <Plugin.h>
#include <ToolKit.h>

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

    // True when a unit already stands on the node.
    bool IsNodeOccupied(GridNode* node) const;

    // True when the node is the one the target marker stands on.
    bool IsTargetNode(GridNode* node) const;

    GridGraph m_grid;
    Player m_player;
    std::vector<StationaryPatrol> m_enemies;
    EntityPtr m_target;      // Optional entity tagged "target".

    TurnPhase m_phase = TurnPhase::Idle;
    bool m_won        = false;
  };

} // namespace ToolKit

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance();
