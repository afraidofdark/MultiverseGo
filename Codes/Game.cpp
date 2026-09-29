/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Game.h"

#include "Execution.h"

#include <Logger.h>
#include <MathUtil.h>
#include <ObjectFactory.h>
#include <Scene.h>
#include <Viewport.h>

#include <algorithm>

ToolKit::Game Self;

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance() { return &Self; }

namespace ToolKit
{
  // See Game.h. A body that has finished dying sinks one unit down over two
  // seconds, which is enough to hide it under the tile surface, and is then
  // removed from the scene. Tunables like the walk's own globals.
  float gCorpseSinkDepth    = 1.0f;
  float gCorpseSinkDuration = 2.0f;

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

    // Debug helper: "(x, z)" for a tile, "(none)" for a null one.
    String NodeText(const GridNode* n)
    {
      if (n == nullptr)
      {
        return "(none)";
      }
      return "(" + std::to_string(n->ix) + ", " + std::to_string(n->iz) + ")";
    }

    // Two clicks inside this many seconds are a DOUBLE CLICK: the player asked for the
    // turn to play at double speed (see Game::NoteClickSpeed). Measured in real seconds,
    // so it does not change with the machine tempo.
    constexpr float kFastClickSeconds = 0.45f;
  } // namespace

  void Game::Init(Main* master) { Main::SetProxy(master); }

  void Game::Destroy() {}

  void Game::Frame(float deltaTime)
  {
    // The clock the double-click window is measured with (real seconds, whatever the
    // machine tempo is at the moment).
    m_clickClock += deltaTime * 0.001f;

    // Bodies of actors the game took out of play keep leaving it on their own:
    // they sink into the ground and are removed from the scene. This runs before
    // everything else -- and whatever the phase -- so a run that just ended (a
    // win or a loss) can never freeze a body half way under the floor. They sink at
    // the game's current speed like everything else.
    AdvanceCorpses(deltaTime * gTurnSpeed);

    // The exclamation marks of the NOTIFIED state are the same kind of thing: a unit
    // raised one, the mark pops and takes itself out. It runs here, before the turn
    // phases, because a mark belongs to its unit and not to the turn being played out --
    // a patrol that noticed the player while it was standing still pops even though
    // nothing is acting.
    UpdateNotices();

    if (m_won || m_lost)
    {
      return;
    }

    // A camera tagged "master" (see SetupMasterCamera) rides the point the player's move
    // is on (see Unit::GetFollowPosition): its progress across the grid, animation wobble
    // removed, so the framing glides from tile to tile with the move -- the destination is
    // known from the move's first frame, a transit re-points it at the next tile, and a
    // hurried turn carries it there quicker because the move itself does. It is
    // deliberately NOT updated once the run ended: a body sinking into the ground must not
    // drag the view down with it.
    m_followCamera.SetFollowPoint(m_player.GetFollowPosition());
    m_followCamera.Update(deltaTime * gTurnSpeed);

    // A committed turn plays out over the coming frames: the player's walk and
    // every enemy glide advance together, and the turn settles (or resolves a
    // bite) once everything has moved. Input stays locked while acting -- except
    // for the TRANSIT: a click on the tile beyond the one the player is walking to
    // keeps the walk going instead of letting it stop there (see TryTransit).
    if (m_phase == TurnPhase::Acting)
    {
      if (HasLeftClick())
      {
        NoteClickSpeed();

        if (GridNode* node = ClickedTile())
        {
          if (!TryTransit(node))
          {
            m_pendingMove = node;
            TK_LOG("Game: (%d, %d) is not a transit; queued as the next move.",
                   node->ix,
                   node->iz);
          }
        }
      }

      UpdateActing(deltaTime);
      return;
    }

    if (m_phase != TurnPhase::Player)
    {
      return;
    }

    // A left click on a connected neighbour tile moves the player one tile.
    if (HasLeftClick())
    {
      NoteClickSpeed();
      HandlePlayerClick();
    }
  }

  void Game::NoteClickSpeed()
  {
    // Two clicks inside kFastClickSeconds of each other are a DOUBLE CLICK: the player
    // asked for this turn to play at double speed. The tile does not matter -- the second
    // click of a double click is usually the next step (which makes it a transit) or the
    // very tile the walk is already heading to (which arms nothing) -- and the speed is
    // set for the TURN, not for one move, so a chain taken during it runs fast too.
    const bool fast = (m_clickClock - m_lastClickTime) <= kFastClickSeconds;
    m_lastClickTime = m_clickClock;

    if (fast)
    {
      SetTurnSpeed(2.0f);
    }
  }

  void Game::SetTurnSpeed(float speed)
  {
    if (glm::abs(gTurnSpeed - speed) < 0.001f)
    {
      return;
    }

    gTurnSpeed = speed;

    // Everything already in flight picks the new tempo up: the clips carry their own
    // multipliers (the engine drives them), and the machines read the global every frame.
    m_player.ReapplyTimeScale();
    for (auto& enemy : m_enemies)
    {
      enemy->ReapplyTimeScale();
    }

    TK_LOG("Game: turn speed x%.1f.", gTurnSpeed);
  }

  void Game::OnLoad(XmlDocumentPtr state) {}

  void Game::OnUnload(XmlDocumentPtr state) {}

  void Game::OnPlay()
  {
    m_phase    = TurnPhase::Idle;
    m_won      = false;
    m_lost     = false;
    m_enemies.clear();
    m_corpses.clear();
    m_grid.Clear();
    m_target = nullptr;
    m_player.Reset();
    m_pendingMove = nullptr;

    // A fresh run starts at normal speed and with no click history: a double click from
    // a previous session must not hurry its first turn.
    gTurnSpeed      = 1.0f;
    m_clickClock    = 0.0f;
    m_lastClickTime = -100.0f;

    // Execution clips are measured per loaded resource, so a fresh session
    // measures them again and the variant cycle starts over.
    ExecutionLibrary::Reset();

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

    // A camera tagged "master" takes the render over and follows the player (a
    // scene without one keeps the viewport camera it had).
    SetupMasterCamera();

    StartPlayerTurn();
  }

  // Nothing game specific happens on pause / resume. Pausing has to hold the
  // engine's animation playback as well, but that is the same for every project
  // and the editor applies it when the simulation state changes
  // (App::SetGameMod -> App::ApplySimulationServices), so these hooks stay free
  // for the game's own work.
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

    // Bodies still on their way down belong to the session that just ended: their
    // entities leave the scene with it, so a corpse can never be picked up as a
    // living patrol (same tag, half buried) by the next play session.
    if (ScenePtr scene = GetSceneManager()->GetCurrentScene())
    {
      for (const Corpse& body : m_corpses)
      {
        if (body.root != nullptr)
        {
          scene->RemoveEntity(body.root);
        }
      }
    }
    m_corpses.clear();

    m_executedEnemy    = nullptr;
    m_arrivalProcessed = false;
    m_pendingMove      = nullptr;
    m_grid.Clear();
    m_target = nullptr;
    m_prevPlayerNode = nullptr;
    m_player.StopAnimation();
    m_player.Reset();

    // The run's camera put the editor's view back (see SetupMasterCamera).
    RestoreViewportCamera();
  }

  void Game::SetupMasterCamera()
  {
    m_editorCamera     = nullptr;
    m_masterCameraHome = Vec3(0.0f);

    if (m_viewport == nullptr)
    {
      return; // Nothing renders through a viewport this session: leave it alone.
    }

    ScenePtr scene = GetSceneManager()->GetCurrentScene();
    if (scene == nullptr)
    {
      return;
    }

    // Optional: a scene camera tagged "master" is the one the game is played
    // through. Everything below is skipped without it, so a scene that does not
    // care about cameras keeps the editor's own view.
    EntityPtr master = scene->GetFirstByTag("master");
    if (master == nullptr)
    {
      TK_LOG("Game: no entity tagged 'master'; keeping the viewport's own camera.");
      return;
    }

    CameraPtr camera = SafeCast<Camera>(master);
    if (camera == nullptr)
    {
      TK_LOG("Game: the entity tagged 'master' is not a camera; keeping the "
             "viewport's own camera.");
      return;
    }

    // The run renders through it from now on. The camera the viewport used is kept
    // so the editor gets its own view back on stop (RestoreViewportCamera).
    m_editorCamera = m_viewport->GetCamera();
    m_viewport->SetCamera(camera);

    // Follow the POINT the player's move is on, not the animated character and not a tile
    // that only changes when the move lands (see Unit::GetFollowPosition): the point
    // travels across the grid with the walk -- its destination is known from the first
    // frame -- so the framing glides along instead of waiting for the landing.
    m_masterCameraHome = master->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    m_followCamera.Init(camera, m_player.GetFollowTarget());
    m_followCamera.SetFollowPoint(m_player.GetFollowPosition(), true);

    TK_LOG("Game: rendering with the camera tagged 'master'; it follows the player's "
           "move from %.2f u away (damping %.2f s).",
           glm::length(m_followCamera.GetOffset()),
           m_followCamera.GetSmoothTime());
  }

  void Game::RestoreViewportCamera()
  {
    if (m_followCamera.IsValid())
    {
      // The follow moved the camera across the grid: put it back where it was
      // authored, so a play session leaves the scene as it found it.
      if (CameraPtr camera = m_followCamera.GetCamera())
      {
        if (camera->m_node != nullptr)
        {
          camera->m_node->SetTranslation(m_masterCameraHome,
                                         TransformationSpace::TS_WORLD);
        }
      }
    }

    m_followCamera = FollowUpCameraController();

    if (m_viewport != nullptr && m_editorCamera != nullptr)
    {
      m_viewport->SetCamera(m_editorCamera);
    }
    m_editorCamera = nullptr;
  }

  void Game::StartPlayerTurn()
  {
    // A new turn starts at NORMAL speed: the hurry a double click asked for belongs to
    // the turn it was clicked in, and a click that starts this one may raise it again.
    SetTurnSpeed(1.0f);

    m_phase = TurnPhase::Player;
    m_player.SetActive(true);
    m_player.OnTurn(nullptr, GridDir::Zm); // Clears the move flag.

    // Remember where the player stood before its move, so the arrival log can
    // print the actual move ("from -> to heading") for the seeker logs.
    m_prevPlayerNode = m_player.GetNode();
    TK_LOG("Game: player turn.");

    // A click that arrived while the last turn was still playing out and could not
    // transit into its tile is made now, as this turn's move: the player asked for
    // that tile, it just costs the stop sequence the transit would have skipped.
    if (GridNode* pending = m_pendingMove)
    {
      m_pendingMove = nullptr;
      TK_LOG("Game: playing the move queued during the last turn.");
      CommitPlayerMove(pending);
    }
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

    // The player's own execution is committed BEFORE this (Player::TryMove
    // starts the strike when the step lands on a patrol), so m_executedEnemy is
    // already set here and survives the bookkeeping reset.

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

      TK_LOG("Game: turn decision -- %s against (%d, %d) heading %s: intended %s -- [%s]",
             u->GetTypeTag().c_str(),
             dest->ix,
             dest->iz,
             GridDirName(facing),
             target != nullptr ? NodeText(target).c_str() : "stay",
             u->DescribeState().c_str());

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
    //    own and another's threat tile resolves as a trade.) The patrol the
    //    player is EXECUTING is the exception: it is the victim of a strike
    //    scene that is still playing, so it stays on the grid until the scene
    //    is over (FinishPlayerExecution) instead of popping out mid-animation.
    if (!m_capturedEnemies.empty())
    {
      for (auto it = m_enemies.begin(); it != m_enemies.end();)
      {
        bool captured = std::find(m_capturedEnemies.begin(),
                                  m_capturedEnemies.end(),
                                  it->get()) != m_capturedEnemies.end();
        if (!captured || it->get() == m_executedEnemy)
        {
          ++it;
          continue;
        }

        // The captured patrol leaves the game now -- it is out of m_enemies, so
        // nothing reacts to it anymore -- but its entity is not deleted: it is
        // laid to rest and sinks into the ground like any other body the game
        // takes out of play (see LayCorpse).
        LayCorpse((*it)->GetRoot(), (*it)->ActiveAnimRemaining());
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
        enemy->Lunge(&m_player);
        TK_LOG("Game: a guard strikes the player on (%d, %d). You lose!", playerNode->ix, playerNode->iz);
        m_activeBites.push_back(enemy.get());
      }
    }
    if (!m_activeBites.empty())
    {
      return;
    }

    // 3) Win check -- only when no guard strike is coming, and not while the
    //    player is still playing its own execution scene on that tile: the
    //    victim has to die on screen before the run can end, so the win waits
    //    for FinishPlayerExecution.
    if (!m_player.IsExecuting() && TryWin())
    {
      return;
    }

    // 4) Patrols that decided to STEP onto the player's tile start their bite
    //    now, with the player already standing there. A strike from the player's
    //    back (a patrol that walked up behind it) is performed with the authored
    //    execution -- the patrol closes in and the ambush clip covers the last
    //    stretch; a relation with no clips authored keeps the plain bite.
    if (!m_stepBites.empty())
    {
      for (Unit* u : m_stepBites)
      {
        if (u->StartExecution(&m_player))
        {
          TK_LOG("Game: a patrol ambushes the player on (%d, %d).", playerNode->ix, playerNode->iz);
        }
        else
        {
          u->StartMove(playerNode);
          TK_LOG("Game: a patrol closes in on the player on (%d, %d).", playerNode->ix, playerNode->iz);
        }
        m_activeBites.push_back(u);
      }
      m_stepBites.clear();
    }
  }

  bool Game::TryWin()
  {
    if (m_won || m_lost || !IsTargetNode(m_player.GetNode()))
    {
      return false;
    }

    m_won = true;
    TK_LOG("Game: player reached the target. You win!");

    // Nothing may stay frozen mid-glide when the run ends -- and no queued step
    // may be played in a turn that will never come.
    m_pendingMove = nullptr;
    for (auto& enemy : m_enemies)
    {
      enemy->LandMove();
    }
    return true;
  }

  void Game::FinishPlayerExecution()
  {
    Unit* victim    = m_executedEnemy;
    m_executedEnemy = nullptr;
    if (victim == nullptr)
    {
      return;
    }

    // The player's strike has played out: the patrol it hit leaves the GAME
    // exactly the way a captured one does -- it is dropped from the enemy list,
    // so it is already out of the game whether its death animation had time to
    // finish or not. What is different is its BODY: instead of being deleted the
    // frame the turn ends, it is laid to rest -- it stays in the scene while its
    // death animation settles (a strike kills mid-fall, so the body still owes
    // the scene a moment) and then sinks into the ground and is removed by
    // AdvanceCorpses.
    GridNode* tile = victim->GetNode();
    LayCorpse(victim->GetRoot(), victim->ActiveAnimRemaining());

    for (auto it = m_enemies.begin(); it != m_enemies.end(); ++it)
    {
      if (it->get() != victim)
      {
        continue;
      }

      m_enemies.erase(it);
      break;
    }

    // The player's action is over even though its scene clock is not (the body it
    // was holding its pose for is gone): settle it back into idle now, so the turn
    // can hand the input back and a move committed later can never be clobbered by
    // a scene that would otherwise end in the middle of it.
    m_player.SettleExecutionScene();

    TK_LOG("Game: the player executed a patrol on (%d, %d).",
           tile != nullptr ? tile->ix : -1,
           tile != nullptr ? tile->iz : -1);

    // The kill may have landed the player on the target tile: the win waited
    // for exactly this moment.
    TryWin();
  }

  void Game::UpdateActing(float deltaTime)
  {
    // Drive the player's walk (or the strike scene that outlives it) and every
    // enemy move/glide in parallel.
    if (m_player.IsWalking() || m_player.IsExecuting())
    {
      m_player.Frame(deltaTime);
    }
    // The walk CHAINED (a click kept it going through the tile it was landing on):
    // that chained step is a REAL turn, so every enemy reacts to it and moves
    // again. A unit still finishing its own step chains its walk into the new one
    // instead of stopping (AnimatedUnit::StartMove), so nobody stands still while
    // the player keeps walking.
    {
      GridNode* passed = nullptr;
      GridNode* dest   = nullptr;
      if (m_player.ConsumeWalkChain(&passed, &dest))
      {
        BeginTransitTurn(dest);
      }
    }

    for (auto& enemy : m_enemies)
    {
      enemy->Frame(deltaTime);
    }

    if (m_won || m_lost)
    {
      return;
    }

    // The player's own execution is resolved the frame its WALK MACHINE ends --
    // that machine IS the player's action (the approach and the strike). The turn
    // waits for the player, never for the death animation that follows the kill:
    // the victim is out of the game the moment the strike lands on it, so the
    // patrol leaves the grid now and a win that was waiting on that tile is
    // declared now, while whatever the body still plays is left to play. This runs
    // before the arrival resolution below, so a kill that ends in the same frame
    // the player landed on its tile still resolves in one go.
    if (m_executedEnemy != nullptr && !m_player.IsWalking())
    {
      FinishPlayerExecution();
      if (m_won || m_lost)
      {
        return;
      }
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
    // a fade) to play out -- it bites and stops there. A bite performed as an
    // EXECUTION instead waits for its whole scene: the strike AND the victim's
    // reaction are one kill, and the loss lands on its last beat. The first bite
    // wins; the remaining enemies are left where they are (EatPlayer lands any
    // unit that is still mid-move).
    {
      GridNode* playerNode = m_player.GetNode();
      for (Unit* bite : m_activeBites)
      {
        if (bite == nullptr || bite->IsExecuting())
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

    // Everything settled: the player arrived, its own strike scene (if any) is
    // over, no bite is pending or in flight, and every unit finished its move.
    // The turn comes back to the player.
    if (m_arrivalProcessed && m_executedEnemy == nullptr && !m_player.IsExecuting() &&
        m_activeBites.empty() && m_stepBites.empty() && !AnyEnemyMoving())
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

  GridNode* Game::ClickedTile()
  {
    if (m_viewport == nullptr || m_grid.Nodes().empty())
    {
      return nullptr;
    }

    // Unproject the click into a ray and intersect it with the tile-top plane.
    // RayFromMousePosition uses the viewport's own tracked mouse position, so
    // the click stays in the viewport's coordinate space (no window/title-bar
    // offset) -- and the viewport's CAMERA, so it follows a master camera too.
    Ray ray             = m_viewport->RayFromMousePosition();
    PlaneEquation plane = PlaneFrom(Vec3(0.0f, m_grid.TopPlaneY(), 0.0f), Y_AXIS);

    Vec3 point(0.0f);
    float t = 0.0f;
    if (!RayPlaneIntersection(ray, plane, t))
    {
      return nullptr;
    }

    point = ray.position + ray.direction * t;
    return m_grid.NodeAtPoint(point);
  }

  bool Game::HasLeftClick() const
  {
    for (Event* e : Main::GetInstance()->m_eventPool)
    {
      if (e->m_type != Event::EventType::Mouse)
      {
        continue;
      }

      MouseEvent* me = static_cast<MouseEvent*>(e);
      if (me->m_action == EventAction::LeftClick && !me->m_release)
      {
        return true;
      }
    }
    return false;
  }

  bool Game::CommitPlayerMove(GridNode* node)
  {
    if (m_won || m_lost || node == nullptr)
    {
      return false;
    }

    // A patrol standing on the clicked tile is the player's prey: the step is
    // offered to the execution first (the same strike an enemy would use), so
    // sneaking up on a patrol's back kills it with the authored scene instead
    // of the instant capture a plain step would give.
    Unit* victim = EnemyOnTile(node);

    if (!m_player.TryMove(node, [this](GridNode* n) { return IsMoveBlocked(n); }, victim))
    {
      TK_LOG("Game: move to (%d, %d) rejected", node->ix, node->iz);
      return false;
    }

    // The player's move is committed: enemies react to it right now and every
    // move of the turn plays out in parallel. Arrival-based resolution runs
    // when the walk finishes (or immediately for actors without animation).
    // A player that came in striking keeps its victim until its scene is over.
    m_executedEnemy = m_player.IsExecuting() ? victim : nullptr;
    BeginPlayerMove(node);
    return true;
  }

  bool Game::TryTransit(GridNode* node)
  {
    if (m_won || m_lost || node == nullptr)
    {
      return false;
    }

    // Only a walk in flight can chain, and the player may neither be playing its
    // own strike scene nor be the victim of one.
    GridNode* through = m_player.GetMoveDestination();
    GridNode* from    = m_player.GetNode();
    if (!m_player.IsWalking() || through == nullptr || from == nullptr ||
        m_player.IsExecuting() || m_executedEnemy != nullptr)
    {
      return false;
    }

    // Already chained into this very tile (the same click read on a later frame):
    // nothing to do, and nothing to log twice.
    if (m_player.GetArmedWalkChain() == node)
    {
      return true;
    }

    // Clicking the tile the walk is already heading for is not a request for another
    // step: nothing to chain and nothing to queue.
    if (node == through)
    {
      return true;
    }

    // Clicking the tile the walk LEFT is a U-TURN, and it chains exactly like any other
    // step: at the landing threshold the walk turns on the spot (the turn clips are
    // played with no travel under them, see gChainTurnAdvance) and strides back the way
    // it came. The player therefore does not have to finish the step it is on before it
    // can go back -- turning around IS a move it can chain, the same machinery a line
    // patrol uses at the end of its line. The checks below still apply to both tiles: a
    // patrol standing on (or heading to) the tile behind, a tooth waiting there or the
    // goal itself all keep the normal turn.

    // Nothing may be at stake on the tile the player is walking THROUGH: no
    // patrol being captured there, no tooth waiting on it (a guard's threat tile
    // or a step bite), no goal whose win has to be declared there.
    if (!m_capturedEnemies.empty() || !m_activeBites.empty() ||
        !m_stepBites.empty() || IsTargetNode(through))
    {
      return false;
    }

    // The clicked tile must be the tile NEXT to the one being walked through: a
    // chain is ONE step, and `GridGraph::Connected` only asks whether a passage
    // exists (it happily answers true for two tiles five units apart along the
    // same line), so adjacency has to be checked with the neighbour lookup --
    // exactly like Player::TryMove validates its step. Without this a click two
    // tiles ahead was accepted and the walk cut straight across the tile between
    // them.
    GridNode* step = nullptr;
    const GridDir dirs[4] = {GridDir::Xm, GridDir::Xp, GridDir::Zm, GridDir::Zp};
    for (GridDir dir : dirs)
    {
      if (m_grid.Neighbor(*through, dir) == node)
      {
        step = node;
        break;
      }
    }

    if (step == nullptr || !m_grid.Connected(*through, *step))
    {
      return false;
    }

    for (const auto& enemy : m_enemies)
    {
      Unit* u = enemy.get();

      // A guard watching the tile being walked through (or the clicked one)
      // lunges the moment the player stands there: that is an attack, and an
      // attack is not a transit.
      if (u->ThreatTile() == through || u->ThreatTile() == node)
      {
        return false;
      }

      // A patrol standing on the clicked tile -- or already on its way onto it, or
      // onto the tile being walked through -- makes this step a strike or a
      // capture. Those resolve on arrival, so they keep the normal turn.
      GridNode* occupied = u->GetNode();
      GridNode* heading  = u->GetMoveDestination();
      if (occupied == node || heading == node || heading == through)
      {
        return false;
      }
    }

    if (!m_player.ArmWalkChain(node))
    {
      return false; // The walk is already landing, or gone: queue it instead.
    }

    TK_LOG("Game: transit armed -- the walk through (%d, %d) goes on to (%d, %d).",
           through->ix,
           through->iz,
           node->ix,
           node->iz);
    return true;
  }

  void Game::BeginTransitTurn(GridNode* dest)
  {
    if (m_won || m_lost || dest == nullptr)
    {
      return;
    }

    // The chained step is a REAL turn: the enemies decide their reaction against
    // the tile the walk now chains into, exactly as they do for a clicked move, and
    // act again. Nothing from the tile walked through has to be settled -- the
    // transit's conditions guaranteed that (see TryTransit) -- so the turn state
    // simply restarts here.
    m_capturedEnemies.clear();
    m_stepBites.clear();
    m_activeBites.clear();
    m_arrivalProcessed = false;

    // The heading the player walks the new leg with: the direction of the leg
    // itself, since a chained step turns on the way to match it.
    GridNode* from = m_player.GetNode();
    GridDir facing = FacingToward(from, dest);

    // The arrival log reads from where the LAST leg started, so the tile the walk
    // passed through is the "from" of the move the turn will end on.
    m_prevPlayerNode = from;

    TK_LOG("Game: transit turn -- the walk goes on to (%d, %d) heading %s; everyone acts again.",
           dest->ix,
           dest->iz,
           GridDirName(facing));

    for (auto& enemy : m_enemies)
    {
      Unit* u = enemy.get();

      // A patrol standing on the chained tile is captured the moment the player
      // arrives there (defensive: TryTransit refuses a transit into one).
      if (u->GetNode() == dest)
      {
        m_capturedEnemies.push_back(u);
        continue;
      }

      // A unit in the middle of a TURN decides from the direction that turn is taking it
      // TO (`Unit::GetFacingDir` reads the turn's target while one is in flight), so it
      // is asked like any other here: a line patrol mid about-face at the end of its line
      // answers "step back down the line" -- the step is then taken when the turn is over
      // (see AnimatedUnit::StartMove) instead of the patrol losing the whole turn to it.
      // The decision log below carries the unit's state, so a mid-turn answer is visible
      // in it ("turning in place" / "turning-on-the-way").

      // A unit that is still finishing its own step decides from the tile that
      // step lands on, so its answer is the NEXT tile -- and that step chains into
      // the walk in flight (AnimatedUnit::StartMove) instead of the unit standing
      // still once it arrives.
      u->SyncTileToMoveDestination();

      u->OnTurn(dest, facing);
      GridNode* target = u->GetIntendedMove();

      TK_LOG("Game: transit turn decision -- %s against (%d, %d) heading %s: intended %s -- [%s]",
             u->GetTypeTag().c_str(),
             dest->ix,
             dest->iz,
             GridDirName(facing),
             target != nullptr ? NodeText(target).c_str() : "stay",
             u->DescribeState().c_str());

      // A step onto the player's destination is a bite: held back until the player
      // actually arrives, exactly like the first leg of the turn.
      if (target == dest)
      {
        TK_LOG("Game: transit turn -- %s steps onto the player's tile: a held bite. [%s]",
               u->GetTypeTag().c_str(),
               u->DescribeState().c_str());
        u->CancelArrivalTurn();
        m_stepBites.push_back(u);
      }
      else if (target != nullptr)
      {
        // A unit still finishing its previous step CHAINS its walk into this new
        // one (see AnimatedUnit::StartMove), so the whole board keeps moving
        // instead of standing still through the player's extra step.
        u->StartMove(target);
      }
    }
  }

  void Game::HandlePlayerClick()
  {
    if (m_won || m_lost)
    {
      return;
    }

    // Clicked outside the grid, or no grid at all: nothing to move onto.
    if (GridNode* node = ClickedTile())
    {
      CommitPlayerMove(node);
    }
  }

  Unit* Game::EnemyOnTile(GridNode* node) const
  {
    if (node == nullptr)
    {
      return nullptr;
    }

    for (const auto& enemy : m_enemies)
    {
      if (enemy->GetNode() == node)
      {
        return enemy.get();
      }
    }
    return nullptr;
  }

  bool Game::IsMoveBlocked(GridNode* node) const
  {
    if (node == nullptr)
    {
      return true;
    }

    // Only the player's own tile blocks the move. Free tiles are moves, and a
    // patrol's tile is a capture attempt -- or, from its back, the player's own
    // execution -- resolved when the player arrives (ResolvePlayerArrival).
    return m_player.GetNode() == node;
  }

  void Game::EatPlayer()
  {
    m_lost  = true;
    m_phase = TurnPhase::Idle;

    // The player died: whatever it was executing dies with it (the prey stays
    // on the grid, since nothing resolves after a loss), and so does the step it
    // was queuing -- there is no turn left to play it in.
    m_executedEnemy = nullptr;
    m_pendingMove   = nullptr;

    // The bite landed while other enemies may still be mid-glide: land them on
    // their tiles so nothing stays frozen between two tiles when the run ends.
    for (auto& enemy : m_enemies)
    {
      enemy->LandMove();
    }

    // The devoured player leaves the game exactly like a patrol the player
    // captures: the unit forgets its tile, so nothing keeps driving a player that
    // has been eaten -- and its BODY is laid to rest like any other, sinking into
    // the ground and leaving the scene (the loss has already waited for the whole
    // strike scene, so it is lying down by now and sinks right away).
    EntityPtr playerRoot = m_player.GetRoot();
    if (playerRoot != nullptr)
    {
      LayCorpse(playerRoot, m_player.ActiveAnimRemaining());
      TK_LOG("Game: the devoured player is out of the game; its body sinks.");
    }
    m_player.Reset();
  }

  void Game::LayCorpse(EntityPtr root, float waitBeforeSink)
  {
    if (root == nullptr || root->m_node == nullptr)
    {
      // Nothing to sink: an actor without a root entity leaves no body behind.
      return;
    }

    Corpse body;
    body.root     = root;
    body.laidAt   = root->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    body.waitLeft = (waitBeforeSink > 0.0f) ? waitBeforeSink : 0.0f;
    m_corpses.push_back(body);

    TK_LOG("Game: a body is laid to rest; it sinks %.2f u down in %.2f s once its "
           "death animation has settled (%.2f s to go).",
           gCorpseSinkDepth,
           gCorpseSinkDuration,
           body.waitLeft);
  }

  void Game::UpdateNotices()
  {
    // The NOTIFIED state (see Unit::Notify): an exclamation pops at the unit's base and
    // the unit takes it out again when the pop is over. One tick per unit per frame, in
    // every phase -- a mark raised by a standing patrol has to pop while the player is
    // still planning their move.
    m_player.UpdateNotice();

    for (auto& enemy : m_enemies)
    {
      if (enemy != nullptr)
      {
        enemy->UpdateNotice();
      }
    }
  }

  void Game::AdvanceCorpses(float deltaTime)
  {
    if (m_corpses.empty())
    {
      return;
    }

    ScenePtr scene = GetSceneManager()->GetCurrentScene();

    // Engine frame deltas arrive in milliseconds; the sink runs in seconds.
    const float dt = deltaTime * 0.001f;

    for (auto it = m_corpses.begin(); it != m_corpses.end();)
    {
      Corpse& body = *it;
      if (body.root == nullptr || body.root->m_node == nullptr)
      {
        it = m_corpses.erase(it);
        continue;
      }

      // The body lies down FIRST: while its death animation still has time left
      // (a one-shot holds its final frame at its end) the corpse is left exactly
      // as the animation put it. Sinking now would slide it through the floor
      // while it is still falling.
      if (body.waitLeft > 0.0f)
      {
        body.waitLeft -= dt;
        ++it;
        continue;
      }

      body.sinkT += dt;
      float progress =
          (gCorpseSinkDuration > 0.0f) ? body.sinkT / gCorpseSinkDuration : 1.0f;
      if (progress > 1.0f)
      {
        progress = 1.0f;
      }

      // Straight down from where the body died: at 1.0 it sits a full
      // gCorpseSinkDepth under the tile surface, which is what hides it.
      body.root->m_node->SetTranslation(
          body.laidAt - Vec3(0.0f, gCorpseSinkDepth * progress, 0.0f),
          TransformationSpace::TS_WORLD);

      if (progress < 1.0f)
      {
        ++it;
        continue;
      }

      if (scene != nullptr)
      {
        scene->RemoveEntity(body.root);
      }
      TK_LOG("Game: a body sank into the ground and left the scene.");
      it = m_corpses.erase(it);
    }
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
