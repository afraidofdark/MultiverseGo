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
  namespace
  {
    // Debug helper: readable name for a grid direction (same table as Unit.cpp).
    const char* GridDirName(GridDir d)
    {
      switch (d)
      {
        case GridDir::Xm: return "-X";
        case GridDir::Xp: return "+X";
        case GridDir::Zm: return "-Z";
        default: return "+Z";
      }
    }
  } // namespace

  void Game::Init(Main* master) { Main::SetProxy(master); }

  void Game::Destroy() {}

  void Game::Frame(float deltaTime)
  {
    if (m_phase != TurnPhase::Player)
    {
      return;
    }

    // An accepted move walks to its tile over the coming frames. Input is
    // locked and the rest of the turn is deferred until the character stands
    // exactly on the destination node.
    if (m_player.IsWalking())
    {
      m_player.Frame(deltaTime);
      if (!m_player.IsWalking())
      {
        CompletePlayerMove();
      }
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

      HandlePlayerClick();
      break;
    }
  }

  void Game::OnLoad(XmlDocumentPtr state) {}

  void Game::OnUnload(XmlDocumentPtr state) {}

  void Game::OnPlay()
  {
    m_phase    = TurnPhase::Idle;
    m_won      = false;
    m_lost     = false;
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

    // Enemies: every placed prefab root tagged "stationary-patrol", which
    // stand in place and guard the tile in front of them.
    EntityPtrArray enemyRoots = scene->GetByTag("stationary-patrol");
    for (EntityPtr root : enemyRoots)
    {
      auto enemy = std::make_unique<StationaryPatrol>();
      if (enemy->Init(root, &m_grid))
      {
        m_enemies.push_back(std::move(enemy));
      }
    }

    // And every "linear-patrol", which walks its line back and forth.
    EntityPtrArray linearRoots = scene->GetByTag("linear-patrol");
    for (EntityPtr root : linearRoots)
    {
      auto enemy = std::make_unique<LinearPatrol>();
      if (enemy->Init(root, &m_grid))
      {
        m_enemies.push_back(std::move(enemy));
      }
    }

    // And every "seeker-patrol", which stares at a fixed point, chases what it
    // sees, and returns the way it came.
    EntityPtrArray seekerRoots = scene->GetByTag("seeker-patrol");
    for (EntityPtr root : seekerRoots)
    {
      auto enemy = std::make_unique<SeekerPatrol>();
      if (enemy->Init(root, &m_grid))
      {
        m_enemies.push_back(std::move(enemy));
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
    m_lost     = false;
    m_enemies.clear();
    m_grid.Clear();
    m_target = nullptr;
    m_prevPlayerNode = nullptr;
    m_player.StopAnimation();
    m_player.Reset();
  }

  void Game::StartPlayerTurn()
  {
    m_phase = TurnPhase::Player;
    m_player.SetActive(true);
    m_player.OnTurn(nullptr, GridDir::Zm); // Clears the move flag.

    // Remember where the player stood before its move, so the next enemy phase
    // can log the actual move ("from -> to heading") for the seeker logs.
    m_prevPlayerNode = m_player.GetNode();
    TK_LOG("Game: player turn.");
  }

  void Game::EndPlayerTurn()
  {
    m_player.SetActive(false);
    m_phase = TurnPhase::Enemies;

    // All enemies act on this phase, from the state at its start. Enemies do
    // not block each other, so each unit's move depends only on the grid and
    // the player's fixed position -- order does not matter. Seeker patrols also
    // need the direction the player is heading, memorized at each sighting.
    GridNode* playerNode = m_player.GetNode();
    GridDir playerFacing = m_player.GetFacingDir();
    bool caught          = false;

    // Log the move the player actually made this turn. The seeker logs below
    // carry the "heading" (player's current facing), so printing the real
    // from->to step next to it makes every heading checkable by eye.
    if (m_prevPlayerNode != playerNode)
    {
      TK_LOG("Game: player moved (%d, %d) -> (%d, %d) heading %s.",
             m_prevPlayerNode != nullptr ? m_prevPlayerNode->ix : -1,
             m_prevPlayerNode != nullptr ? m_prevPlayerNode->iz : -1,
             playerNode->ix,
             playerNode->iz,
             GridDirName(playerFacing));
    }

    for (auto& enemy : m_enemies)
    {
      enemy->SetActive(true);
      enemy->OnTurn(playerNode, playerFacing);
      enemy->SetActive(false);

      // A patrol that walks onto the player's tile eats them.
      if (enemy->GetNode() == playerNode)
      {
        caught = true;
      }
    }

    if (caught)
    {
      // The patrol is already standing on the player's tile -- that step was its
      // bite -- so the player is simply gone.
      TK_LOG("Game: a patrol caught the player. You lose!");
      EatPlayer();
      return;
    }

    StartPlayerTurn();
  }

  void Game::HandlePlayerClick()
  {
    if (m_won || m_lost)
    {
      return;
    }

    // Unproject the click into a ray and intersect it with the tile-top plane.
    // RayFromMousePosition uses the viewport's own tracked mouse position, so
    // the click stays in the viewport's coordinate space (no window/title-bar
    // offset).
    Ray ray             = m_viewport->RayFromMousePosition();
    PlaneEquation plane = PlaneFrom(Vec3(0.0f, m_grid.TopPlaneY(), 0.0f), Y_AXIS);

    Vec3 point(0.0f);
    float t = 0.0f;
    bool onGrid = RayPlaneIntersection(ray, plane, t);
    if (onGrid)
    {
      point = ray.position + ray.direction * t;
    }

    if (!onGrid)
    {
      return;
    }

    GridNode* node = m_grid.NodeAtPoint(point);
    if (node == nullptr)
    {
      return; // Clicked outside the grid.
    }

    if (m_player.TryMove(node, [this](GridNode* n) { return IsMoveBlocked(n); }))
    {
      // An animated walk finishes over the coming frames, on arrival running
      // CompletePlayerMove from Game::Frame. Actors without animation support
      // land instantly and resolve right here.
      if (!m_player.IsWalking())
      {
        CompletePlayerMove();
      }
      return;
    }
    else
    {
      TK_LOG("Game: move to (%d, %d) rejected", node->ix, node->iz);
    }
  }

  void Game::CompletePlayerMove()
  {
    if (m_won || m_lost)
    {
      return;
    }

    // The patrol rule resolves before the win check: a patrol eats the player
    // that steps onto its watched tile, even when that tile also holds the
    // target.
    if (ResolvePatrolContact())
    {
      return;
    }

    if (IsTargetNode(m_player.GetNode()))
    {
      m_won = true;
      TK_LOG("Game: player reached the target. You win!");
      return;
    }

    EndPlayerTurn();
  }

  bool Game::IsMoveBlocked(GridNode* node) const
  {
    if (node == nullptr)
    {
      return true;
    }

    // Only the player's own tile blocks the move. Free tiles are moves, and a
    // patrol's tile is a capture attempt resolved by ResolvePatrolContact.
    return m_player.GetNode() == node;
  }

  bool Game::ResolvePatrolContact()
  {
    GridNode* playerNode = m_player.GetNode();
    if (playerNode == nullptr)
    {
      return false;
    }

    // Stacked order, capture first. Stepping onto an enemy's own tile removes
    // it from the grid.
    for (auto it = m_enemies.begin(); it != m_enemies.end(); ++it)
    {
      if ((*it)->GetNode() == playerNode)
      {
        EntityPtr root = (*it)->GetRoot();
        if (root != nullptr)
        {
          GetSceneManager()->GetCurrentScene()->RemoveEntity(root);
        }
        TK_LOG("Game: player captured a patrol.");
        m_enemies.erase(it);
        break;
      }
    }

    // Then the remaining enemies react: any static guard whose threat tile is
    // the player's tile eats the player. Moving patrols threaten by walking
    // onto the player during the enemy phase, so they report no static threat.
    // Because this runs after the capture, a tile that is both an enemy's own
    // and another's threat tile resolves as a trade -- the player captures it
    // and still gets eaten by the other patrol.
    for (const auto& enemy : m_enemies)
    {
      if (enemy->ThreatTile() == playerNode)
      {
        // The guard does not eat from its post: it lunges onto the player's tile
        // first, so the strike is visible, and only then is the player gone.
        enemy->Lunge();
        TK_LOG("Game: patrol ate the player. You lose!");
        EatPlayer();
        return true;
      }
    }

    return false;
  }

  void Game::EatPlayer()
  {
    m_lost  = true;
    m_phase = TurnPhase::Idle;

    // The devoured player leaves the scene exactly like a patrol the player
    // captures: the root entity is removed and the unit forgets its tile, so
    // nothing keeps drawing or driving a player that has been eaten.
    EntityPtr playerRoot = m_player.GetRoot();
    if (playerRoot != nullptr)
    {
      ScenePtr scene = GetSceneManager()->GetCurrentScene();
      if (scene != nullptr)
      {
        scene->RemoveEntity(playerRoot);
        TK_LOG("Game: the player has been removed from the scene.");
      }
    }
    m_player.Reset();
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
