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

#include "FollowUpCameraController.h"
#include "GridGraph.h"
#include "Unit.h"

namespace ToolKit
{

  // How a body the game took out of play leaves the scene: once its death
  // animation has settled it slides straight down gCorpseSinkDepth world units
  // over gCorpseSinkDuration seconds and is then removed. About one unit is
  // enough to bury a body lying on the tile surface, so a killed actor reads as
  // sinking into the ground instead of popping out of existence the frame the
  // kill resolves. Tunable at runtime like gWalkBlendDuration / gTurnDuration;
  // defined in Game.cpp.
  extern float gCorpseSinkDepth;
  extern float gCorpseSinkDuration;

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

    // Takes over the render when the scene carries a camera tagged "master": that
    // camera is set on the viewport (the game then renders through it) and made to
    // follow the player smoothly. A scene without one -- or with a "master" entity
    // that is not a camera -- is left exactly as it is: the viewport keeps the
    // camera the editor gave it and no follow runs.
    void SetupMasterCamera();

    // Puts the editor's own camera back on the viewport and the master camera back
    // on the spot it was authored at, so a play session leaves nothing behind.
    void RestoreViewportCamera();

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

    // The patrol standing on a tile, or null when the tile is free. Used to
    // offer the step as an execution when the player is about to land on one.
    Unit* EnemyOnTile(GridNode* node) const;

    // Ends the player's own execution: the strike scene has played out, so the
    // patrol the player hit leaves the game (it is dropped from m_enemies and a
    // win that was waiting on that very tile is declared now) while its BODY is
    // laid to rest -- it stays in the scene and sinks into the ground (see
    // LayCorpse) exactly the way a captured one does.
    void FinishPlayerExecution();

    // A body the game has taken out of play and is still removing. The actor's
    // root entity STAYS in the scene, so the death animation it is playing keeps
    // playing, but the unit itself is already gone from m_enemies: nothing reacts
    // to a corpse and no turn may resolve on top of one.
    struct Corpse
    {
      EntityPtr root;    // Root entity of the removed actor.
      Vec3 laidAt;       // World position the body was left at.
      float waitLeft = 0.0f; // Wall clock seconds it still owes its death animation.
      float sinkT    = 0.0f; // Sink time so far (seconds).
    };

    // Turns a removed actor into a CORPSE instead of deleting it. The entity
    // stays where it died and sinks into the ground once its death animation has
    // settled; AdvanceCorpses then drops it from the scene. waitBeforeSink is how
    // long the body still owes that animation, i.e. typically
    // Unit::ActiveAnimRemaining() read at the moment of the kill, so a body lies
    // down first instead of sliding through the floor while it is still falling.
    void LayCorpse(EntityPtr root, float waitBeforeSink);

    // Advances every corpse: waits out the rest of its death animation, sinks the
    // body gCorpseSinkDepth units straight down over gCorpseSinkDuration seconds
    // and removes its entity from the scene once it is under the ground. Runs in
    // EVERY phase -- a win or a loss must not freeze a body half way under the
    // floor -- so it is the first thing Frame does.
    void AdvanceCorpses(float deltaTime);

    // Declares the win when the player stands on the target tile. Returns true
    // when the run was won; the caller returns immediately, because nothing
    // else may resolve after the run ends.
    bool TryWin();

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

    // The patrol the PLAYER is executing this turn (its own strike scene), or
    // null. It is never removed at arrival like a captured patrol: it stays
    // until the scene has played out, then FinishPlayerExecution drops it.
    Unit* m_executedEnemy = nullptr;

    // Bodies on their way under the ground: actors the game removed from play
    // whose root entity is still in the scene, sinking (see LayCorpse).
    std::vector<Corpse> m_corpses;

    // The camera tagged "master" (when the scene has one) and the follow that
    // rides the player with it. IsValid() stays false without such a camera, and
    // then the game never touches the viewport's camera.
    FollowUpCameraController m_followCamera;
    // Where that camera was authored, put back on stop so the editor keeps its
    // placement after a session that moved it.
    Vec3 m_masterCameraHome = Vec3(0.0f);
    // The viewport's own camera before the master camera took over, restored on
    // stop (the editor gets its view back).
    CameraPtr m_editorCamera = nullptr;

    EntityPtr m_target;      // Optional entity tagged "target".

    TurnPhase m_phase = TurnPhase::Idle;
    bool m_won        = false;
    bool m_lost       = false;
  };

} // namespace ToolKit

extern "C" TK_PLUGIN_API ToolKit::Game* TK_STDCAL GetInstance();
