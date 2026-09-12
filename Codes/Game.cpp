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

#include <algorithm>

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
    if (m_won || m_lost)
    {
      return;
    }

    // A committed turn plays out over the coming frames: the player's walk and
    // every enemy glide advance together, and the turn settles (or resolves a
    // bite) once everything has moved. Input stays locked while acting.
    if (m_phase == TurnPhase::Acting)
    {
      UpdateActing(deltaTime);
      return;
    }

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
    m_capturedEnemies.clear();
    m_stepBites.clear();
    m_activeBites.clear();
    m_arrivalProcessed = false;
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

    // Remember where the player stood before its move, so the arrival log can
    // print the actual move ("from -> to heading") for the seeker logs.
    m_prevPlayerNode = m_player.GetNode();
    TK_LOG("Game: player turn.");
  }

  void Game::BeginPlayerMove(GridNode* dest)
  {
    if (m_won || m_lost || dest == nullptr)
    {
      return;
    }

    m_phase            = TurnPhase::Acting;
    m_arrivalProcessed = false;
    m_capturedEnemies.clear();
    m_stepBites.clear();
    m_activeBites.clear();

    // The player acts first: every enemy reacts to the tile the player WILL
    // stand on (its committed destination) and to the heading it will face
    // there. Each enemy decides NOW; the game starts the actual (gliding) moves
    // afterwards, so every unit of the turn moves at the same time.
    GridNode* from = m_player.GetNode();
    GridDir facing = FacingToward(from, dest);

    for (auto& enemy : m_enemies)
    {
      Unit* u = enemy.get();

      // A patrol standing on the destination is captured the moment the player
      // steps there. Freeze it for this turn (it never acts) and remove it when
      // the player arrives.
      if (u->GetNode() == dest)
      {
        m_capturedEnemies.push_back(u);
        continue;
      }

      u->OnTurn(dest, facing);
      GridNode* target = u->GetIntendedMove();

      // A step onto the player's destination is a bite: like a guard's lunge it
      // waits for the player to actually arrive before it starts, so an enemy
      // never strikes a tile the player has not reached yet. The eat ends the
      // turn, so any follow-up the enemy had planned for its landing (a line
      // patrol's about-face) is dropped: it bites and stops.
      if (target == dest)
      {
        u->CancelArrivalTurn();
        m_stepBites.push_back(u);
      }
      else if (target != nullptr)
      {
        // Any other step starts now and runs at the same time as the player's
        // walk: every entity's action of the turn lasts the same length (the
        // animated move scales to gTurnDuration), so the whole tableau starts
        // and stops together.
        u->StartMove(target);
      }
    }

    // The player's own walk was already started by Player::TryMove. Actors
    // without animation support stand on the tile at once, so the arrival
    // resolution runs right here.
    if (!m_player.IsWalking())
    {
      ResolvePlayerArrival();
    }
  }

  void Game::ResolvePlayerArrival()
  {
    if (m_won || m_lost || m_arrivalProcessed)
    {
      return;
    }
    m_arrivalProcessed = true;

    GridNode* playerNode = m_player.GetNode();
    if (playerNode == nullptr)
    {
      return;
    }

    // Log the move the player actually made this turn. The seeker logs below
    // carry the "heading" (player's current facing), so printing the real
    // from->to step next to it makes every heading checkable by eye.
    if (m_prevPlayerNode != playerNode)
    {
      GridDir facing = m_player.GetFacingDir();
      TK_LOG("Game: player moved (%d, %d) -> (%d, %d) heading %s.",
             m_prevPlayerNode != nullptr ? m_prevPlayerNode->ix : -1,
             m_prevPlayerNode != nullptr ? m_prevPlayerNode->iz : -1,
             playerNode->ix,
             playerNode->iz,
             GridDirName(facing));
    }

    // 1) The player captures every patrol that stood on the tile it moved to.
    //    (Capture before the guards react, so a tile that is both an enemy's
    //    own and another's threat tile resolves as a trade.)
    if (!m_capturedEnemies.empty())
    {
      ScenePtr scene = GetSceneManager()->GetCurrentScene();
      for (auto it = m_enemies.begin(); it != m_enemies.end();)
      {
        bool captured = std::find(m_capturedEnemies.begin(),
                                  m_capturedEnemies.end(),
                                  it->get()) != m_capturedEnemies.end();
        if (!captured)
        {
          ++it;
          continue;
        }

        if (EntityPtr root = (*it)->GetRoot())
        {
          if (scene != nullptr)
          {
            scene->RemoveEntity(root);
          }
        }
        TK_LOG("Game: player captured a patrol.");
        it = m_enemies.erase(it);
      }
      m_capturedEnemies.clear();
    }

    // 2) Guards whose threat tile is the player's tile lunge NOW: the strike
    //    waits until the player stands there. The player is eaten when the
    //    lunge lands (UpdateActing); while a lunge is inbound the outcome is a
    //    loss, so the win check below is skipped.
    for (const auto& enemy : m_enemies)
    {
      if (enemy->ThreatTile() == playerNode)
      {
        enemy->Lunge();
        TK_LOG("Game: a guard strikes the player on (%d, %d). You lose!", playerNode->ix, playerNode->iz);
        m_activeBites.push_back(enemy.get());
      }
    }
    if (!m_activeBites.empty())
    {
      return;
    }

    // 3) Win check -- only when no guard strike is coming.
    if (IsTargetNode(playerNode))
    {
      m_won = true;
      TK_LOG("Game: player reached the target. You win!");

      // Nothing may stay frozen mid-glide when the run ends.
      for (auto& enemy : m_enemies)
      {
        enemy->LandMove();
      }
      return;
    }

    // 4) Patrols that decided to STEP onto the player's tile start their bite
    //    now, with the player already standing there.
    if (!m_stepBites.empty())
    {
      for (Unit* u : m_stepBites)
      {
        u->StartMove(playerNode);
        TK_LOG("Game: a patrol closes in on the player on (%d, %d).", playerNode->ix, playerNode->iz);
        m_activeBites.push_back(u);
      }
      m_stepBites.clear();
    }
  }

  void Game::UpdateActing(float deltaTime)
  {
    // Drive the player's walk and every enemy glide in parallel.
    if (m_player.IsWalking())
    {
      m_player.Frame(deltaTime);
    }
    for (auto& enemy : m_enemies)
    {
      enemy->Frame(deltaTime);
    }

    if (m_won || m_lost)
    {
      return;
    }

    // The player physically arrived: run the arrival resolution (captures,
    // guard lunges, win check, delayed bites).
    if (!m_arrivalProcessed && !m_player.IsWalking())
    {
      ResolvePlayerArrival();
      if (m_won || m_lost)
      {
        return;
      }
    }

    // A bite is the moment the biting enemy STANDS on the player's tile: the
    // eat does not wait for anything else the enemy had queued (a landing turn,
    // a fade) to play out -- it bites and stops there. The first bite wins; the
    // remaining enemies are left where they are (EatPlayer lands any unit that
    // is still mid-move).
    {
      GridNode* playerNode = m_player.GetNode();
      for (Unit* bite : m_activeBites)
      {
        if (bite == nullptr)
        {
          continue;
        }

        bool landedOnPlayer = (playerNode != nullptr) && (bite->GetNode() == playerNode);
        if (landedOnPlayer || !bite->IsMoving())
        {
          TK_LOG("Game: a patrol caught the player. You lose!");
          EatPlayer();
          return;
        }
      }
    }

    // Everything settled: the player arrived, no bite is pending or in flight,
    // and every unit finished its move. The turn comes back to the player.
    if (m_arrivalProcessed && m_activeBites.empty() && m_stepBites.empty() && !AnyEnemyMoving())
    {
      StartPlayerTurn();
    }
  }

  GridDir Game::FacingToward(GridNode* from, GridNode* to) const
  {
    if (from == nullptr || to == nullptr)
    {
      return GridDir::Zm;
    }

    Vec3 d = to->center - from->center;
    if (std::fabs(d.x) >= std::fabs(d.z))
    {
      return (d.x < 0.0f) ? GridDir::Xm : GridDir::Xp;
    }
    return (d.z < 0.0f) ? GridDir::Zm : GridDir::Zp;
  }

  bool Game::AnyEnemyMoving() const
  {
    for (const auto& enemy : m_enemies)
    {
      if (enemy->IsMoving())
      {
        return true;
      }
    }
    return false;
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
      // The player's move is committed: enemies react to it right now and every
      // move of the turn plays out in parallel. Arrival-based resolution runs
      // when the walk finishes (or immediately for actors without animation).
      BeginPlayerMove(node);
      return;
    }
    else
    {
      TK_LOG("Game: move to (%d, %d) rejected", node->ix, node->iz);
    }
  }

  bool Game::IsMoveBlocked(GridNode* node) const
  {
    if (node == nullptr)
    {
      return true;
    }

    // Only the player's own tile blocks the move. Free tiles are moves, and a
    // patrol's tile is a capture attempt resolved when the player arrives
    // (ResolvePlayerArrival).
    return m_player.GetNode() == node;
  }

  void Game::EatPlayer()
  {
    m_lost  = true;
    m_phase = TurnPhase::Idle;

    // The bite landed while other enemies may still be mid-glide: land them on
    // their tiles so nothing stays frozen between two tiles when the run ends.
    for (auto& enemy : m_enemies)
    {
      enemy->LandMove();
    }

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
