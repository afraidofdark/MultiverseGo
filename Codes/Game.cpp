/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Game.h"

#include <Logger.h>
#include <MathUtil.h>
#include <Scene.h>
#include <Viewport.h>

ToolKit::Game Self;

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance() { return &Self; }

namespace ToolKit
{
  void Game::Init(Main* master) { Main::SetProxy(master); }

  void Game::Destroy() {}

  void Game::Frame(float deltaTime)
  {
    if (m_phase != TurnPhase::Player)
    {
      return;
    }

    // A left click on a connected neighbour tile moves the player one tile.
    for (Event* e : Main::GetInstance()->m_eventPool)
    {
      if (e->m_type != Event::EventType::Mouse)
      {
        continue;
      }

      MouseEvent* me = static_cast<MouseEvent*>(e);
      if (me->m_action != EventAction::LeftClick || me->m_release)
      {
        continue;
      }

      HandlePlayerClick(Vec2((float) me->absolute[0], (float) me->absolute[1]));
      break;
    }
  }

  void Game::OnLoad(XmlDocumentPtr state) {}

  void Game::OnUnload(XmlDocumentPtr state) {}

  void Game::OnPlay()
  {
    m_phase    = TurnPhase::Idle;
    m_won      = false;
    m_enemies.clear();
    m_grid.Clear();
    m_target = nullptr;
    m_player.Reset();

    ScenePtr scene = GetSceneManager()->GetCurrentScene();
    if (scene == nullptr)
    {
      TK_LOG("Game::OnPlay: no current scene to play.");
      return;
    }

    // Grid: the master "GridNode" entity parents every tile.
    EntityPtr gridRoot = scene->GetFirstByName("GridNode");
    if (gridRoot == nullptr)
    {
      TK_LOG("Game::OnPlay: no GridNode in the scene.");
      return;
    }

    m_grid.LoadFromScene(gridRoot);
    if (m_grid.Nodes().empty())
    {
      TK_LOG("Game::OnPlay: grid has no tiles.");
      return;
    }

    // Player: the placed prefab root tagged "player".
    EntityPtrArray playerRoots = scene->GetByTag("player");
    if (playerRoots.empty())
    {
      TK_LOG("Game::OnPlay: no entity tagged 'player' in the scene.");
      return;
    }

    if (!m_player.Init(playerRoots[0], &m_grid))
    {
      TK_LOG("Game::OnPlay: player is not on the grid.");
      return;
    }

    // Enemies: every placed prefab root tagged "stationary-patrol".
    EntityPtrArray enemyRoots = scene->GetByTag("stationary-patrol");
    for (EntityPtr root : enemyRoots)
    {
      StationaryPatrol enemy;
      if (enemy.Init(root, &m_grid))
      {
        m_enemies.push_back(enemy);
      }
    }

    // Optional target marker: an entity tagged "target" standing on a tile.
    EntityPtrArray targets = scene->GetByTag("target");
    if (!targets.empty())
    {
      m_target = targets[0];
    }

    StartPlayerTurn();
  }

  void Game::OnPause() {}

  void Game::OnResume() {}

  void Game::OnStop()
  {
    m_phase    = TurnPhase::Idle;
    m_won      = false;
    m_enemies.clear();
    m_grid.Clear();
    m_target = nullptr;
    m_player.Reset();
  }

  void Game::StartPlayerTurn()
  {
    m_phase = TurnPhase::Player;
    m_player.SetActive(true);
    m_player.OnTurn(); // Clears the move flag.
    TK_LOG("Game: player turn.");
  }

  void Game::EndPlayerTurn()
  {
    m_player.SetActive(false);
    m_phase = TurnPhase::Enemies;

    // Every enemy takes its fixed action, then the turn returns to the player.
    for (StationaryPatrol& enemy : m_enemies)
    {
      enemy.SetActive(true);
      enemy.OnTurn(); // No-op for now (StationaryPatrol stands in place).
      enemy.SetActive(false);
    }

    StartPlayerTurn();
  }

  void Game::HandlePlayerClick(const Vec2& mousePos)
  {
    if (m_won)
    {
      return;
    }

    // Unproject the click into a ray and intersect it with the tile-top plane.
    Ray ray                    = m_viewport->RayFromScreenSpacePoint(mousePos);
    PlaneEquation plane        = PlaneFrom(Vec3(0.0f, m_grid.TopPlaneY(), 0.0f), Y_AXIS);

    float t = 0.0f;
    if (!RayPlaneIntersection(ray, plane, t))
    {
      return;
    }

    Vec3 point = ray.position + ray.direction * t;
    GridNode* node = m_grid.NodeAtPoint(point);
    if (node == nullptr)
    {
      return; // Clicked outside the grid.
    }

    if (m_player.TryMove(node, [this](GridNode* n) { return IsNodeOccupied(n); }))
    {
      if (IsTargetNode(m_player.GetNode()))
      {
        m_won = true;
        TK_LOG("Game: player reached the target. You win!");
        return;
      }

      EndPlayerTurn();
    }
  }

  bool Game::IsNodeOccupied(GridNode* node) const
  {
    if (node == nullptr)
    {
      return false;
    }

    if (m_player.GetNode() == node)
    {
      return true;
    }

    for (const StationaryPatrol& enemy : m_enemies)
    {
      if (enemy.GetNode() == node)
      {
        return true;
      }
    }

    return false;
  }

  bool Game::IsTargetNode(GridNode* node) const
  {
    if (node == nullptr || m_target == nullptr)
    {
      return false;
    }

    Vec3 targetPos        = m_target->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    const GridNode* targetNode = m_grid.NodeAtPoint(targetPos);
    return targetNode != nullptr && targetNode == node;
  }

} // namespace ToolKit
