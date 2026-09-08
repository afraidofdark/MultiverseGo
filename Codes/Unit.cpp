/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Unit.h"

#include <Animation.h>
#include <AnimationControllerComponent.h>
#include <Logger.h>
#include <MathUtil.h>
#include <StateMachine.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ToolKit
{
  namespace
  {
    // Debug helper: readable name for a grid direction.
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

  // Data the player's walk state machine operates on. One instance lives per
  // walk (created by Player::StartWalk, owned by the player). The FSM states
  // only read/write this context; they never reach into the Player.
  struct Player::WalkContext
  {
    Node* actorNode = nullptr;              // Node root motion moves.
    AnimControllerComponent* anim = nullptr; // Controller playing the clips.
    GridNode* to = nullptr;                 // Destination tile.
    Vec3 startPos;                          // Actor position at walk start.
    Vec3 targetPos;                         // Destination node center.
    float startDur = 0.0f;                  // walk_f_start duration (seconds).
    float endDur = 0.0f;                    // walk_f_end duration (seconds).
    float endReach = 0.0f;                  // Root travel of the full end clip.
    float totalDist = 0.0f;                 // Horizontal gap start -> target.
    float bestRemaining = 0.0f;             // Smallest gap seen (stall watchdog).
    float sinceProgress = 0.0f;             // Seconds since the gap last shrank.
    bool arrived = false;                   // True once the walk reached the tile.
  };

  // Crossfade length (seconds) used whenever the walk state machine switches
  // clips (idle -> walk_f_start -> walk_f -> walk_f_end -> idle). A global so
  // it can be tuned at runtime (e.g. bound to a settings value) instead of
  // being an inline constant; declared in Unit.h.
  float gWalkBlendDuration = 0.2f;

  namespace
  {
    // Tolerance (engine units) for "reached the node". Below this the actor is
    // snapped onto the exact node center, so the walk ends cleanly whatever the
    // grid spacing is.
    constexpr float kWalkArriveEps = 0.03f;

    // If the gap does not shrink for this long the walk is assumed broken
    // (e.g. root motion axes misaligned with the prefab orientation) and the
    // player snaps over.
    constexpr float kWalkStallTimeout = 1.0f;

    // Every clip the controller plays keeps m_loop = true, so a clip wraps to
    // its first frame when its time passes its duration. End the walk a hair
    // before that happens so the last rendered pose is the end clip's final
    // (stopped) frame instead of a wrapped first stride.
    constexpr float kWalkEndStopMargin = 0.02f;

    // Signals the walk states use to move the machine through its phases.
    enum WalkSignal : SignalId
    {
      WalkLoop = 1, // Start clip finished; stride in the loop clip.
      WalkEnd  = 2  // Close enough; play the end clip to the stop.
    };

    // Horizontal distance between two world points.
    float HorizontalDistance(const Vec3& a, const Vec3& b)
    {
      float dx = b.x - a.x;
      float dz = b.z - a.z;
      return glm::sqrt(dx * dx + dz * dz);
    }

    // Remaining horizontal distance from the moving actor to the target.
    float WalkRemaining(const Player::WalkContext& ctx)
    {
      if (ctx.actorNode == nullptr)
      {
        return 0.0f;
      }

      Vec3 pos = ctx.actorNode->GetTranslation(TransformationSpace::TS_WORLD);
      return HorizontalDistance(pos, ctx.targetPos);
    }

    // Net horizontal root travel a clip makes when played from its first to its
    // last key. The engine applies per-frame deltas whose sum telescopes to
    // (last - first), so this is the exact distance the clip walks its actor.
    // Zero when the animation has no usable root track.
    float ClipRootTravel(AnimRecordPtr record)
    {
      AnimationPtr anim = record != nullptr ? record->m_animation : nullptr;
      if (anim == nullptr || anim->m_rootKey.empty())
      {
        return 0.0f;
      }

      const KeyArray* keys = anim->m_keys.Find(anim->m_rootKey);
      if (keys == nullptr || keys->size() < 2)
      {
        return 0.0f;
      }

      Vec3 delta = keys->back().m_position - keys->front().m_position;
      return glm::sqrt(delta.x * delta.x + delta.z * delta.z);
    }

    // Switches the clip the animation controller plays using a short pose
    // crossfade (AnimControllerComponent::SmoothTransition fills the record
    // blending data; the engine fades the skeleton pose between the outgoing
    // and the incoming clip). The outgoing record must stop contributing root
    // motion for the blend: while it still sits in the animation player it
    // would otherwise drive the actor together with the incoming clip and
    // double the travelled distance.
    void BlendTo(AnimControllerComponent* anim, const String& signal)
    {
      if (anim == nullptr)
      {
        return;
      }

      // File name only (no folder) for readable logs.
      auto fileNameOf = [](AnimRecordPtr rec) -> String
      {
        const String& file = (rec != nullptr && rec->m_animation != nullptr) ? rec->m_animation->GetFile() : String();
        size_t sep         = file.find_last_of('/');
        return (sep != String::npos) ? file.substr(sep + 1) : file;
      };

      AnimRecordPtr prev = anim->GetActiveRecord();
      const String from  = fileNameOf(prev);
      bool prevRootMotion = (prev != nullptr) && prev->m_applyRootMotion;

      // The outgoing record must stop contributing root motion for the blend:
      // while it still sits in the animation player it would otherwise drive
      // the actor together with the incoming clip and double the travelled
      // distance.
      if (prev != nullptr)
      {
        prev->m_applyRootMotion = false;
      }

      anim->SmoothTransition(signal, gWalkBlendDuration);

      TK_LOG("WalkBlend: '%s' -> '%s', fade %.2f s (outgoing root motion %s).",
             from.c_str(),
             signal.c_str(),
             gWalkBlendDuration,
             prevRootMotion ? "on, now off" : "off");
    }

    // First walk phase: plays the wind-up clip (walk_f_start) once with root
    // motion. Hands over to the stride loop (or straight to the end clip when
    // the remaining gap already fits it) once the clip has played through.
    class PlayerWalkStartState : public State
    {
     public:
      explicit PlayerWalkStartState(Player::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        if (m_ctx != nullptr && m_ctx->anim != nullptr)
        {
          BlendTo(m_ctx->anim, "walk_f_start");
        }
      }

      void TransitionOut(State* nextState) override {}

      SignalId Update(float deltaTime) override
      {
        if (m_ctx == nullptr)
        {
          return State::NullSignal;
        }

        m_elapsed += deltaTime;
        float remaining = WalkRemaining(*m_ctx);

        // A gap shorter than the wind-up clip: finish the moment the actor
        // reaches the node (the final snap closes the leftover distance).
        if (remaining <= kWalkArriveEps)
        {
          TK_LOG("WalkState: start -> arrived inside wind-up (elapsed %.2f, remaining %.3f).",
                 m_elapsed,
                 remaining);
          m_ctx->arrived = true;
          return State::NullSignal;
        }

        // Wind-up played through. The fade to the stride loop may start right
        // at the clip end: the engine now holds a fading-out clip at its final
        // frame instead of wrapping it, so the outgoing pose stays continuous.
        if (m_elapsed >= m_ctx->startDur)
        {
          if (remaining <= m_ctx->endReach)
          {
            TK_LOG("WalkState: start -> end (elapsed %.2f/%.2f, remaining %.2f <= end reach %.2f).",
                   m_elapsed,
                   m_ctx->startDur,
                   remaining,
                   m_ctx->endReach);
            return WalkEnd;
          }

          TK_LOG("WalkState: start -> loop (elapsed %.2f/%.2f, remaining %.2f).",
                 m_elapsed,
                 m_ctx->startDur,
                 remaining);
          return WalkLoop;
        }

        return State::NullSignal;
      }

      String Signaled(SignalId signal) override
      {
        switch (signal)
        {
          case WalkLoop: return "WalkLoop";
          case WalkEnd: return "WalkEnd";
          default: return "";
        }
      }

      String GetType() override { return "WalkStart"; }

     private:
      Player::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // Middle phase: keeps the stride clip (walk_f) looping while root motion
    // covers the gap. Leaves for the end clip the moment the remaining distance
    // fits the end clip's own root travel, so the walk can stop exactly on the
    // destination node no matter how long the gap is.
    class PlayerWalkLoopState : public State
    {
     public:
      explicit PlayerWalkLoopState(Player::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        if (m_ctx != nullptr && m_ctx->anim != nullptr)
        {
          BlendTo(m_ctx->anim, "walk_f");
        }
      }

      void TransitionOut(State* nextState) override {}

      SignalId Update(float deltaTime) override
      {
        if (m_ctx == nullptr)
        {
          return State::NullSignal;
        }

        float remaining = WalkRemaining(*m_ctx);
        if (remaining <= kWalkArriveEps)
        {
          // No end-clip data or a gap closed by a loop boundary: stop here and
          // let the final snap take over.
          TK_LOG("WalkState: loop -> arrived (remaining %.3f).", remaining);
          m_ctx->arrived = true;
          return State::NullSignal;
        }

        if (m_ctx->endReach > kWalkArriveEps && remaining <= m_ctx->endReach)
        {
          TK_LOG("WalkState: loop -> end (remaining %.2f <= end reach %.2f).",
                 remaining,
                 m_ctx->endReach);
          return WalkEnd;
        }

        return State::NullSignal;
      }

      String Signaled(SignalId signal) override
      {
        switch (signal)
        {
          case WalkEnd: return "WalkEnd";
          default: return "";
        }
      }

      String GetType() override { return "WalkLoop"; }

     private:
      Player::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // Final phase: plays the landing clip (walk_f_end) with root motion. The
    // state ends the walk (ctx->arrived) when the actor reaches the node or the
    // clip plays through; Player::FinishWalk then snaps the actor onto the
    // exact node center and returns it to the idle loop.
    class PlayerWalkEndState : public State
    {
     public:
      explicit PlayerWalkEndState(Player::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        if (m_ctx != nullptr && m_ctx->anim != nullptr)
        {
          BlendTo(m_ctx->anim, "walk_f_end");
        }
      }

      void TransitionOut(State* nextState) override {}

      SignalId Update(float deltaTime) override
      {
        if (m_ctx == nullptr)
        {
          return State::NullSignal;
        }

        m_elapsed += deltaTime;
        float remaining = WalkRemaining(*m_ctx);

        // Arrive when the actor reaches the node, or a hair before the end
        // clip wraps (records loop): the final rendered pose must be the
        // clip's last frame, not a wrapped first stride.
        bool reached  = remaining <= kWalkArriveEps;
        bool clipDone = m_elapsed >= (m_ctx->endDur - kWalkEndStopMargin);
        if (reached || clipDone)
        {
          TK_LOG("WalkState: end -> arrived (elapsed %.2f/%.2f, remaining %.3f).",
                 m_elapsed,
                 m_ctx->endDur,
                 remaining);
          m_ctx->arrived = true;
        }

        return State::NullSignal;
      }

      String Signaled(SignalId signal) override { return ""; }

      String GetType() override { return "WalkEnd"; }

     private:
      Player::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };
  } // namespace

  bool Unit::Init(EntityPtr root, GridGraph* grid)
  {
    m_root  = root;
    m_grid  = grid;
    m_node  = nullptr;
    m_active = false;

    if (m_root == nullptr || m_grid == nullptr)
    {
      return false;
    }

    // The root entity is nested under its prefab/tile, so its world position
    // already points at the tile it stands on. Snap it onto the exact node
    // center so movement math starts from a known state.
    Vec3 pos           = m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    GridNode* node = m_grid->NodeAtPoint(pos);
    if (node == nullptr)
    {
      return false;
    }

    PlaceOnNode(node);
    return true;
  }

  String Unit::GetTypeTag() const
  {
    if (m_root == nullptr)
    {
      return "";
    }

    return m_root->GetTagVal();
  }

  Vec3 Unit::GetWorldPosition() const
  {
    if (m_root == nullptr)
    {
      return Vec3(0.0f);
    }

    return m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD);
  }

  GridDir Unit::GetFacingDir() const
  {
    if (m_root == nullptr)
    {
      return GridDir::Zm; // Fallback: model forward is -Z.
    }

    // World forward is the local -Z rotated by the root's world orientation.
    Quaternion q = m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD);
    Vec3 fwd     = glm::normalize(glm::vec3(q * Vec3(0.0f, 0.0f, -1.0f)));

    // Snap to the nearest grid axis. Grid movement is horizontal, so the Y
    // component of the facing is ignored.
    if (std::fabs(fwd.x) >= std::fabs(fwd.z))
    {
      return (fwd.x < 0.0f) ? GridDir::Xm : GridDir::Xp;
    }
    return (fwd.z < 0.0f) ? GridDir::Zm : GridDir::Zp;
  }

  void Unit::Reset()
  {
    m_root   = nullptr;
    m_grid   = nullptr;
    m_node   = nullptr;
    m_active = false;
  }

  void Unit::PlaceOnNode(GridNode* node)
  {
    GridNode* prev = m_node;
    m_node = node;
    if (m_root != nullptr && node != nullptr)
    {
      m_root->m_node->SetTranslation(node->center, TransformationSpace::TS_WORLD);

      // Moving between two nodes turns the unit toward its step. Init snaps
      // onto the starting node with prev == nullptr, so the unit keeps the
      // rotation it was authored with until it first moves.
      if (prev != nullptr)
      {
        FaceTowards(node->center - prev->center);
      }
    }
  }

  void Unit::FaceTowards(const Vec3& direction)
  {
    if (m_root == nullptr)
    {
      return;
    }

    Vec3 dir = direction;
    if (glm::length(dir) < 0.0001f)
    {
      return;
    }

    // Model convention: forward is -Z. Rotate the root so its -Z looks along
    // the movement direction. World space, because grid movement is
    // axis-aligned in world space.
    Quaternion rot = RotationTo(Vec3(0.0f, 0.0f, -1.0f), glm::normalize(dir));
    m_root->m_node->SetOrientation(rot, TransformationSpace::TS_WORLD);
  }

  bool Player::Init(EntityPtr root, GridGraph* grid)
  {
    // The unit stays bound to the prefab's top root (the tagged "root" node):
    // the game rotates this node to aim the character and anchors it on the
    // grid tiles. The skinned character (mesh + skeleton + animation
    // controller) hangs under it as a child and is the actor root motion
    // plays on. Actors without an animation controller (legacy simple actors)
    // simply have no walk animation.
    if (!Unit::Init(root, grid))
    {
      return false;
    }

    m_actor    = root;
    m_walkAnim = nullptr;

    if (root != nullptr)
    {
      AnimControllerComponentPtr anim = root->GetComponent<AnimControllerComponent>();
      if (anim == nullptr)
      {
        TraverseEntityHierarchyBottomUp(
            root,
            [&](EntityPtr ntt) -> void
            {
              if (m_walkAnim == nullptr && ntt != nullptr)
              {
                if (AnimControllerComponentPtr c = ntt->GetComponent<AnimControllerComponent>())
                {
                  m_walkAnim = c.get();
                  m_actor    = ntt;
                }
              }
            });
      }
      else
      {
        m_walkAnim = anim.get();
        m_actor    = root;
      }
    }

    // Remember the actor's authored local pose inside the prefab. Root motion
    // accumulates local translation on this node while it walks; restoring the
    // base folds the travelled distance back into the top root on arrival.
    if (m_actor != nullptr && m_actor->m_node != m_root->m_node)
    {
      m_actorLocalBase = m_actor->m_node->GetTranslation(TransformationSpace::TS_LOCAL);
    }
    else
    {
      m_actorLocalBase = Vec3(0.0f);
    }

    m_hasMoved = false;

    // Settle the character into the idle loop between turns.
    if (m_walkAnim != nullptr)
    {
      if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
      {
        idle->m_applyRootMotion = false;
      }
      BlendTo(m_walkAnim, "idle");
    }

    return true;
  }

  Player::~Player() { Reset(); }

  void Player::OnTurn(GridNode* playerNode, GridDir playerFacing)
  {
    m_hasMoved = false;
  }

  void Player::Frame(float deltaTime)
  {
    if (m_walkSM == nullptr || m_walkCtx == nullptr)
    {
      return;
    }

    // Engine frame deltas arrive in milliseconds; clip durations and the state
    // machine timers work in seconds, and the AnimationPlayer advances records
    // with the same millisecond-to-second conversion.
    float dt = deltaTime * 0.001f;

    WalkContext* ctx = m_walkCtx;
    if (ctx->actorNode == nullptr)
    {
      FinishWalk(true);
      return;
    }

    // Stall watchdog: root motion that never converges (misaligned direction,
    // missing walk data) must not lock the turn forever. The gap shrinking at
    // least a little every frame keeps the timer at zero.
    if (dt < 0.5f)
    {
      float remaining = WalkRemaining(*ctx);
      if (remaining < ctx->bestRemaining - 0.001f)
      {
        ctx->bestRemaining = remaining;
        ctx->sinceProgress = 0.0f;
      }
      else
      {
        ctx->sinceProgress += dt;
      }

      if (ctx->sinceProgress >= kWalkStallTimeout)
      {
        TK_LOG("Player: walk stalled (gap %.3f not shrinking); snapping to the tile.",
               remaining);
        FinishWalk(true);
        return;
      }
    }

    m_walkSM->Update(dt);
    if (ctx->arrived)
    {
      FinishWalk(false);
    }
  }

  bool Player::TryMove(GridNode* node, const std::function<bool(GridNode*)>& isOccupied)
  {
    if (m_root == nullptr || m_grid == nullptr || m_hasMoved || IsWalking())
    {
      return false;
    }

    if (node == nullptr || node == m_node)
    {
      return false;
    }

    if (isOccupied && isOccupied(node))
    {
      return false;
    }

    GridNode* current = m_node;
    if (current == nullptr)
    {
      return false;
    }

    // The player rule: exactly one tile per turn, along a connected edge. The
    // target must be a direct neighbour AND both sides must open the passage.
    bool connected = false;
    const GridDir dirs[4] = {GridDir::Xm, GridDir::Xp, GridDir::Zm, GridDir::Zp};
    for (GridDir dir : dirs)
    {
      if (GridNode* nb = m_grid->Neighbor(*current, dir))
      {
        if (nb == node && m_grid->Connected(*current, *nb))
        {
          connected = true;
          break;
        }
      }
    }

    if (!connected)
    {
      return false;
    }

    if (!StartWalk(node))
    {
      TK_LOG("Player: move to (%d, %d) rejected", node->ix, node->iz);
      return false;
    }

    m_hasMoved = true;
    return true;
  }

  bool Player::StartWalk(GridNode* node)
  {
    if (m_walkAnim == nullptr || m_actor == nullptr || m_root == nullptr)
    {
      // Legacy actor without animation support: keep the instant step.
      PlaceOnNode(node);
      return true;
    }

    AnimRecordPtr startRec = m_walkAnim->GetAnimRecord("walk_f_start");
    AnimRecordPtr loopRec  = m_walkAnim->GetAnimRecord("walk_f");
    AnimRecordPtr endRec   = m_walkAnim->GetAnimRecord("walk_f_end");
    if (startRec == nullptr || loopRec == nullptr || endRec == nullptr)
    {
      TK_LOG("Player: walk clips are missing on the animation controller; stepping instantly.");
      PlaceOnNode(node);
      return true;
    }

    if (startRec->m_animation == nullptr || endRec->m_animation == nullptr)
    {
      TK_LOG("Player: walk clip resources are not loaded; stepping instantly.");
      PlaceOnNode(node);
      return true;
    }

    // Aim the prefab's top root at the step before the walk begins. Root
    // motion is applied on the actor node in its local space (the engine
    // AnimationPlayer), so this rotation is what points the walk in the
    // direction of the destination.
    Vec3 stepDir = (m_node != nullptr) ? (node->center - m_node->center) : (node->center - m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD));
    FaceTowards(stepDir);

    // Enable root motion on the three walk clips; the idle loop stays put.
    startRec->m_applyRootMotion = true;
    loopRec->m_applyRootMotion  = true;
    endRec->m_applyRootMotion   = true;

    WalkContext* ctx  = new WalkContext();
    m_walkCtx         = ctx;
    ctx->actorNode    = m_actor->m_node;
    ctx->anim         = m_walkAnim;
    ctx->to           = node;
    ctx->startPos     = m_actor->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    ctx->targetPos    = node->center;
    ctx->startDur     = startRec->m_animation->m_duration;
    ctx->endDur       = endRec->m_animation->m_duration;
    ctx->endReach     = ClipRootTravel(endRec);
    ctx->totalDist    = HorizontalDistance(ctx->startPos, ctx->targetPos);
    ctx->bestRemaining = ctx->totalDist;
    ctx->sinceProgress = 0.0f;
    ctx->arrived       = false;

    m_walkSM = new StateMachine();
    m_walkSM->PushState(new PlayerWalkStartState(ctx));
    m_walkSM->PushState(new PlayerWalkLoopState(ctx));
    m_walkSM->PushState(new PlayerWalkEndState(ctx));
    m_walkSM->m_currentState = m_walkSM->QueryState("WalkStart");
    m_walkSM->m_currentState->TransitionIn(nullptr);

    TK_LOG("Player: walk started (%d, %d) -> (%d, %d), gap %.2f, end clip reach %.2f.",
           m_node != nullptr ? m_node->ix : -1,
           m_node != nullptr ? m_node->iz : -1,
           node->ix,
           node->iz,
           ctx->totalDist,
           ctx->endReach);
    return true;
  }

  void Player::FinishWalk(bool forceSnap)
  {
    WalkContext* ctx = m_walkCtx;
    GridNode* dest   = (ctx != nullptr) ? ctx->to : nullptr;
    Vec3 targetPos   = (ctx != nullptr) ? ctx->targetPos : Vec3(0.0f);

    delete m_walkSM;
    m_walkSM  = nullptr;
    m_walkCtx = nullptr;

    if (ctx != nullptr)
    {
      // How far the actor was from the node center when the walk ended; a
      // normal arrival should be within a few centimeters, anything bigger
      // means the end clip overshot or the walk was interrupted.
      if (m_actor != nullptr && m_actor->m_node != nullptr)
      {
        Vec3 pos = m_actor->m_node->GetTranslation(TransformationSpace::TS_WORLD);
        TK_LOG("Player: arrival snap residual %.3f u (%s).",
               HorizontalDistance(pos, targetPos),
               forceSnap ? "teleport" : "walk");
      }

      if (m_root != nullptr)
      {
        if (forceSnap)
        {
          // Broken/teleport path: the regular snap also turns the actor
          // toward the step like any instant tile move.
          PlaceOnNode(dest);
        }
        else
        {
          // The walk reached the node: anchor the prefab top root on the exact
          // destination center. The actor node still carries the root motion
          // offset in its local translation; it is cleared right below.
          m_root->m_node->SetTranslation(targetPos, TransformationSpace::TS_WORLD);
          m_node = dest;
        }
      }
      delete ctx;
    }

    // Fold the walked distance back into the top root: restoring the actor's
    // authored local pose removes the accumulated root-motion offset, so the
    // character stands exactly on the anchored node and future rotations of
    // the prefab top root start from a clean frame.
    if (m_actor != nullptr && m_root != nullptr && m_actor->m_node != m_root->m_node)
    {
      m_actor->m_node->SetTranslation(m_actorLocalBase, TransformationSpace::TS_LOCAL);
    }

    // Settle the character back into the idle loop with a crossfade. BlendTo
    // also turns off the root motion of the outgoing walk clip, so the
    // snapped-to-node actor does not drift while the end clip fades out.
    if (m_walkAnim != nullptr)
    {
      if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
      {
        idle->m_applyRootMotion = false;
      }
      BlendTo(m_walkAnim, "idle");
    }

    if (dest != nullptr)
    {
      TK_LOG("Player: walk finished on (%d, %d).", dest->ix, dest->iz);
    }
  }

  void Player::StopAnimation()
  {
    if (m_walkAnim != nullptr)
    {
      m_walkAnim->Stop();
    }
  }

  void Player::Reset()
  {
    // Playback and the walk state machine never outlive the actor. The
    // animation controller removes its active record itself when its entity is
    // destroyed, so no dereference of m_walkAnim happens here.
    delete m_walkSM;
    m_walkSM  = nullptr;
    delete m_walkCtx;
    m_walkCtx = nullptr;
    m_walkAnim = nullptr;
    m_actor    = nullptr;
    m_actorLocalBase = Vec3(0.0f);
    m_hasMoved = false;

    Unit::Reset();
  }

  GridNode* StationaryPatrol::ThreatTile() const
  {
    if (m_node == nullptr || m_grid == nullptr)
    {
      return nullptr;
    }

    // Navigation is exclusively over connections, so the patrol's threat is
    // too: a blocked passage means the watched tile is not reachable through
    // the patrol's side and the patrol sees nothing there.
    GridNode* watched = m_grid->Neighbor(*m_node, GetFacingDir());
    if (watched == nullptr || !m_grid->Connected(*m_node, *watched))
    {
      return nullptr;
    }

    return watched;
  }

  void StationaryPatrol::Lunge()
  {
    GridNode* watched = ThreatTile();
    if (watched == nullptr)
    {
      // Nothing lungeable in front -- the passage is blocked or the guard stands
      // at the grid edge. Whether that still counts as a bite is the caller's
      // call, which already decided it from ThreatTile().
      TK_LOG("Guard: holds its post, no open passage to lunge through.");
      return;
    }

    // The one step forward onto the prey's tile. ThreatTile() only answers with
    // a connected neighbour, so this is a legal move and not a reach across a
    // wall. The guard already faces this way and PlaceOnNode keeps that heading,
    // so the step reads purely as a strike.
    PlaceOnNode(watched);
    TK_LOG("Guard: lunges forward onto (%d, %d) and bites.",
           watched->ix,
           watched->iz);
  }

  void LinearPatrol::OnTurn(GridNode* playerNode, GridDir playerFacing)
  {
    if (m_node == nullptr || m_grid == nullptr)
    {
      return;
    }

    // One tile per turn along the facing line. A connected neighbour keeps the
    // patrol moving; a missing or blocked one means the line ends, so the
    // patrol turns 180 degrees in place and walks back next turn. Enemies do
    // not block each other, so the tile ahead is only checked for a connection.
    GridNode* next = m_grid->Neighbor(*m_node, GetFacingDir());
    if (next != nullptr && m_grid->Connected(*m_node, *next))
    {
      PlaceOnNode(next);
    }
    else
    {
      FlipFacing();
    }
  }

  void LinearPatrol::FlipFacing()
  {
    if (m_root == nullptr)
    {
      return;
    }

    // Face back along the line: the current world forward, negated. RotationTo
    // handles the 180-degree (antiparallel) case.
    Quaternion q   = m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD);
    Vec3 fwd       = glm::normalize(glm::vec3(q * Vec3(0.0f, 0.0f, -1.0f)));
    Quaternion rot = RotationTo(Vec3(0.0f, 0.0f, -1.0f), -fwd);
    m_root->m_node->SetOrientation(rot, TransformationSpace::TS_WORLD);
  }

  bool SeekerPatrol::Init(EntityPtr root, GridGraph* grid)
  {
    if (!Unit::Init(root, grid))
    {
      return false;
    }

    m_startNode       = m_node;
    m_idleOrientation = m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD);
    m_state           = State::Idle;
    m_lastSeen        = nullptr;
    m_lastHeading     = GridDir::Zm;
    m_watchLeft       = 0;
    m_trail.clear();
    return true;
  }

  void SeekerPatrol::OnTurn(GridNode* playerNode, GridDir playerFacing)
  {
    if (m_node == nullptr || m_grid == nullptr)
    {
      return;
    }

    TK_LOG("Seeker: turn, state=%d at (%d, %d) facing %s, player at (%d, %d) heading %s.",
           (int) m_state,
           m_node->ix,
           m_node->iz,
           GridDirName(GetFacingDir()),
           playerNode != nullptr ? playerNode->ix : -1,
           playerNode != nullptr ? playerNode->iz : -1,
           GridDirName(playerFacing));

    switch (m_state)
    {
      case State::Idle:
        // First sighting: start chasing from here, remembering the way back.
        if (CanSee(playerNode))
        {
          TK_LOG("Seeker: spotted the player at (%d, %d) heading %s; memorized and chasing.",
                 playerNode->ix,
                 playerNode->iz,
                 GridDirName(playerFacing));
          m_trail.clear();
          m_trail.push_back(m_node);
          SpotPlayer(playerNode, playerFacing);
          m_sighted = true;
          m_state = State::Chasing;
          StepChase(playerNode, playerFacing);
        }
        break;

      case State::Chasing:
        // Live sight: while the player is visible, the chase target and the
        // known heading stay fresh, so the pursuit follows every turn.
        if (CanSee(playerNode))
        {
          TK_LOG("Seeker: player still in sight at (%d, %d) heading %s.",
                 playerNode->ix,
                 playerNode->iz,
                 GridDirName(playerFacing));
          SpotPlayer(playerNode, playerFacing);
          m_sighted = true;
        }
        else
        {
          // The player just left the view. The heading that matters is the one
          // it was moving with at the moment it disappeared -- not the heading
          // from the last visible tile, which is stale by that one step (it is
          // the direction the player arrived FROM, usually straight toward the
          // patrol). Snapshot the current heading exactly when sight is lost;
          // later out-of-sight turns keep it frozen.
          if (m_sighted)
          {
            m_lastHeading = playerFacing;
            m_sighted = false;
            TK_LOG("Seeker: lost sight; player left the view heading %s; memorizing that.",
                   GridDirName(m_lastHeading));
          }

          TK_LOG("Seeker: player out of sight; walking to last seen (%d, %d), heading %s frozen at sight loss.",
                 m_lastSeen->ix,
                 m_lastSeen->iz,
                 GridDirName(m_lastHeading));
        }
        StepChase(playerNode, playerFacing);
        break;

      case State::Watching:
        // The waiting turns. The patrol landed here already turned to the frozen
        // heading, so it holds that angle by staying put and looks down the same
        // line once per turn. The player's step lands before the patrol acts, so
        // a body that walked back into the held line is taken. Nothing moves on a
        // watching turn: giving up only hands the patrol over to Returning, whose
        // first homeward step comes on the following turn.
        if (CanSee(playerNode))
        {
          TK_LOG("Seeker: player walked into the held angle at (%d, %d); re-engaging.",
                 playerNode->ix,
                 playerNode->iz);
          SpotPlayer(playerNode, playerFacing);
          m_sighted   = true;
          m_watchLeft = 0;
          m_state     = State::Chasing;
          StepChase(playerNode, playerFacing);
        }
        else if (m_watchLeft > 1)
        {
          --m_watchLeft;
          TK_LOG("Seeker: still nobody along %s; holding the angle for %d more turn(s).",
                 GridDirName(m_lastHeading),
                 m_watchLeft);
        }
        else
        {
          TK_LOG("Seeker: nobody along %s after the wait; giving up and turning back.",
                 GridDirName(m_lastHeading));
          m_watchLeft = 0;
          m_state     = State::Returning;
        }
        break;

      case State::Returning:
        // The stare never sleeps: a player crossing its view on the way back
        // re-engages the same chase from right here. The trail keeps growing,
        // so the eventual return still finds its way home.
        if (CanSee(playerNode))
        {
          TK_LOG("Seeker: player crossed the view on the way back at (%d, %d); re-engaging.",
                 playerNode->ix,
                 playerNode->iz);
          SpotPlayer(playerNode, playerFacing);
          m_sighted = true;
          m_state = State::Chasing;
          StepChase(playerNode, playerFacing);
        }
        else
        {
          StepReturn();
        }
        break;
    }
  }

  bool SeekerPatrol::CanSee(GridNode* playerNode) const
  {
    if (m_node == nullptr || m_grid == nullptr || playerNode == nullptr)
    {
      return false;
    }

    // Line of sight runs along the facing direction through connected tiles,
    // until a blocked passage, the grid edge, or the player.
    GridDir dir        = GetFacingDir();
    GridNode* cursor   = m_node;
    while (cursor != nullptr)
    {
      GridNode* next = m_grid->Neighbor(*cursor, dir);
      if (next == nullptr || !m_grid->Connected(*cursor, *next))
      {
        return false;
      }
      if (next == playerNode)
      {
        return true;
      }
      cursor = next;
    }

    return false;
  }

  void SeekerPatrol::SpotPlayer(GridNode* playerNode, GridDir playerFacing)
  {
    // Memorize both the tile and the way the player is going right now, so the
    // investigation can face that way even if the player is never seen again.
    m_lastSeen    = playerNode;
    m_lastHeading = playerFacing;
  }

  std::vector<GridNode*> SeekerPatrol::FindPath(GridNode* to) const
  {
    std::vector<GridNode*> result;
    if (m_node == nullptr || m_grid == nullptr || to == nullptr || m_node == to)
    {
      return result;
    }

    // Breadth-first search over connected neighbours.
    std::unordered_map<GridNode*, GridNode*> cameFrom;
    cameFrom[m_node] = nullptr;
    std::vector<GridNode*> frontier = {m_node};
    bool found                       = false;
    while (!frontier.empty() && !found)
    {
      std::vector<GridNode*> next;
      for (GridNode* n : frontier)
      {
        const GridDir dirs[4] = {GridDir::Xm, GridDir::Xp, GridDir::Zm, GridDir::Zp};
        for (GridDir d : dirs)
        {
          GridNode* nb = m_grid->Neighbor(*n, d);
          if (nb == nullptr || !m_grid->Connected(*n, *nb) || cameFrom.count(nb) != 0)
          {
            continue;
          }
          cameFrom[nb] = n;
          if (nb == to)
          {
            found = true;
            break;
          }
          next.push_back(nb);
        }
        if (found)
        {
          break;
        }
      }
      frontier = next;
    }

    if (!found)
    {
      return result;
    }

    for (GridNode* n = to; n != nullptr; n = cameFrom[n])
    {
      result.push_back(n);
    }
    std::reverse(result.begin(), result.end());
    return result; // [m_node, ..., to]
  }

  void SeekerPatrol::StepChase(GridNode* playerNode, GridDir playerFacing)
  {
    std::vector<GridNode*> path = FindPath(m_lastSeen);
    if (path.size() > 1)
    {
      GridNode* next = path[1];
      PlaceOnNode(next);
      m_trail.push_back(next);
      TK_LOG("Seeker: chase step to (%d, %d), %d tile(s) to go.", next->ix, next->iz, (int) path.size() - 2);
    }

    // Landing on the player's tile IS the bite, so the chase ends right here:
    // there is nobody left to look for. The patrol holds exactly where it
    // stopped, still facing the way it walked in -- it does not turn to the
    // memorized heading, does not take an arrival look down a line it is
    // standing in, and never enters the wait. It stays in Chasing; the game
    // resolves the loss on this same turn.
    if (m_node == playerNode)
    {
      TK_LOG("Seeker: caught the player at (%d, %d); holding position.", m_node->ix, m_node->iz);
      return;
    }

    // The chase leg ends when the patrol lands on the last sighting tile, or at
    // once when that tile turns out to be unreachable. Landing, turning to the
    // heading frozen at sight loss and looking down it are ONE turn: the look is
    // taken before the player gets another step in, so a player fleeing straight
    // ahead of that heading is caught the moment the patrol arrives. Handing the
    // look to a later turn let it stand having already turned, watch the player
    // walk out of the very line it was staring down, and give up.
    if (m_node != m_lastSeen && path.size() > 1)
    {
      return; // Still walking; the arrival turn has not come yet.
    }

    if (m_node == m_lastSeen)
    {
      TK_LOG("Seeker: arrived at the last seen tile (%d, %d); turning to memorized heading %s and looking.",
             m_node->ix,
             m_node->iz,
             GridDirName(m_lastHeading));
    }
    else
    {
      TK_LOG("Seeker: last seen tile (%d, %d) unreachable; turning to memorized heading %s and looking.",
             m_lastSeen->ix,
             m_lastSeen->iz,
             GridDirName(m_lastHeading));
    }

    TurnTo(m_lastHeading);

    // The turn above already points the stare, so the very same turn can see
    // along it. A fresh sighting keeps the chase going from here -- the walk
    // resumes on the next turn, one step per turn as always -- while an empty
    // line gives the patrol up and sends it back along its trail.
    if (CanSee(playerNode))
    {
      TK_LOG("Seeker: player caught along %s at (%d, %d) on arrival; chase continues.",
             GridDirName(m_lastHeading),
             playerNode->ix,
             playerNode->iz);
      SpotPlayer(playerNode, playerFacing);
      m_sighted = true;
      m_state   = State::Chasing;
    }
    else
    {
      // One empty look is not the end of it. The patrol keeps the angle it was
      // shown and stands on it for kWatchTurns turns: the player gets that many
      // extra steps to walk back into the line before the chase is buried. No
      // homeward step is allowed to share a turn with the wait -- a patrol that
      // looks and then walks has not visibly waited at all.
      m_watchLeft = kWatchTurns;
      TK_LOG("Seeker: nobody along %s; holding this angle for %d turn(s) before turning back.",
             GridDirName(m_lastHeading),
             m_watchLeft);
      m_state = State::Watching;
    }
  }

  void SeekerPatrol::StepReturn()
  {
    if (m_trail.size() > 1)
    {
      GridNode* back = m_trail[m_trail.size() - 2];
      m_trail.pop_back();
      PlaceOnNode(back); // Arrival and turning toward the step happen together.
      TK_LOG("Seeker: return step to (%d, %d).", back->ix, back->iz);

      if (m_trail.size() == 1 && m_node == m_trail[0])
      {
        // Back at the start: resume the idle stare.
        TK_LOG("Seeker: back at the start; resuming the idle stare.");
        m_state = State::Idle;
        TurnToIdle();
        m_lastSeen = nullptr;
      }
    }
    else
    {
      // No path to retrace: already back at the start.
      m_state = State::Idle;
      TurnToIdle();
      m_lastSeen = nullptr;
    }
  }

  void SeekerPatrol::TurnTo(GridDir dir)
  {
    if (m_root == nullptr)
    {
      return;
    }

    Vec3 forward;
    switch (dir)
    {
      case GridDir::Xm: forward = Vec3(-1.0f, 0.0f, 0.0f); break;
      case GridDir::Xp: forward = Vec3(1.0f, 0.0f, 0.0f); break;
      case GridDir::Zm: forward = Vec3(0.0f, 0.0f, -1.0f); break;
      default: forward = Vec3(0.0f, 0.0f, 1.0f); break;
    }

    Quaternion rot = RotationTo(Vec3(0.0f, 0.0f, -1.0f), forward);
    m_root->m_node->SetOrientation(rot, TransformationSpace::TS_WORLD);
  }

  void SeekerPatrol::TurnToIdle()
  {
    if (m_root == nullptr)
    {
      return;
    }
    m_root->m_node->SetOrientation(m_idleOrientation, TransformationSpace::TS_WORLD);
  }

  void SeekerPatrol::Reset()
  {
    Unit::Reset();
    m_startNode   = nullptr;
    m_lastSeen    = nullptr;
    m_lastHeading = GridDir::Zm;
    m_sighted     = false;
    m_watchLeft   = 0;
    m_state       = State::Idle;
    m_trail.clear();
  }

} // namespace ToolKit
