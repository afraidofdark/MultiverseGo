/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Unit.h"

#include "Execution.h"

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

    // Grid direction a horizontal offset points along. Moves are axis aligned,
    // so the dominant axis decides.
    GridDir DirectionOf(const Vec3& delta)
    {
      if (std::fabs(delta.x) >= std::fabs(delta.z))
      {
        return (delta.x < 0.0f) ? GridDir::Xm : GridDir::Xp;
      }
      return (delta.z < 0.0f) ? GridDir::Zm : GridDir::Zp;
    }

    // The stride clips a move loops on. A PLAIN step travels with walk_f; an
    // EXECUTION closes in with the FIGHT walk (fight_walk_f), because the
    // attacker is walking into a kill instead of travelling -- see
    // AnimatedUnit::StartAction. Both are looping cycles that carry root motion,
    // so whichever one is chosen is the clip that covers the gap and the one the
    // move's timing is measured on.
    constexpr const char* kStrideSignal      = "walk_f";
    constexpr const char* kFightStrideSignal = "fight_walk_f";
  } // namespace

  // Data the unit's walk state machine operates on. One instance lives per
  // walk (created by AnimatedUnit::StartWalk, owned by the unit). The FSM
  // states only read/write this context; they never reach into the unit.
  struct AnimatedUnit::WalkContext
  {
    Node* actorNode = nullptr;              // Node root motion moves.
    AnimControllerComponent* anim = nullptr; // Controller playing the clips.
    GridNode* to = nullptr;                 // Destination tile.
    Vec3 startPos;                          // Actor position at walk start.
    Vec3 targetPos;                         // Destination node center.
    float startDur = 0.0f;                  // walk_f_start duration (seconds).
    float endDur = 0.0f;                    // walk_f_end duration (seconds).
    float endReach = 0.0f;                  // Root travel of the full end clip.
    String loopSignal = kStrideSignal;      // Stride clip the loop phase plays.
    float totalDist = 0.0f;                 // Horizontal gap start -> target.
    float bestRemaining = 0.0f;             // Smallest gap seen (stall watchdog).
    float sinceProgress = 0.0f;             // Seconds since the gap last shrank.
    bool arrived = false;                   // True once the walk reached the tile.

    // Turn-in-place phase data. Primary: the re-authored turn clips carry their
    // rotation as ROOT MOTION, so WalkTurnState plays the clip with root
    // motion on and the engine yaws the actor node itself. When the clip ends,
    // the persistent facing (prefab top root) is folded to the target yaw and
    // the actor's local orientation is restored to its pre-turn base.
    // Fallback (no usable clip): the top root is yawed directly.
    Node* rootNode = nullptr;               // Prefab top root holding the facing.
    String turnSignal;                      // Turn clip signal ("" = node-only fallback).
    float turnDur = 0.0f;                   // Turn duration (seconds).
    float turnYawFrom = 0.0f;               // Top root yaw at turn start (rad).
    float turnYawTo = 0.0f;                 // Top root yaw after the turn (rad).
    Quaternion actorBaseOrient;             // Actor local orientation before the turn.
    bool turning = false;                   // True while the turn plays.

    // Execution phase data (see AnimatedUnit::StartExecution). The strike is
    // the landing phase of the walk: it replaces walk_f_end, covers the last
    // `execReach` of the gap with its own root motion, and ends the action.
    String execSignal;             // Authored strike clip ("" = a plain walk).
    std::function<float()> onStrike; // Plays the victim's side, returns its length.
    float execSceneDur = 0.0f;     // Whole scene length, known once the strike starts.
    bool strikeStarted = false;    // Set when the strike clip begins to play.

    // Pre-strike turn of a SIDE strike (a strike the victim never faces, see
    // ExecutionPlan::victimTurnsToAttacker): the victim turns to face the
    // attacker while the approach runs, and this reports whether that turn is
    // still going. Null when the victim had nothing to turn (every other strike),
    // so the strike starts immediately. `waitingForVictim` is set while the
    // strike phase is holding for that turn, so the walk's stall watchdog knows
    // the attacker is standing still ON PURPOSE.
    std::function<bool()> victimTurning;
    bool waitingForVictim = false;
  };

  // Crossfade length (seconds) used whenever the walk state machine switches
  // clips (idle -> walk_f_start -> walk_f -> walk_f_end -> idle). A global so
  // it can be tuned at runtime (e.g. bound to a settings value) instead of
  // being an inline constant; declared in Unit.h.
  float gWalkBlendDuration = 0.2f;

  // Every turn's action window (seconds). The player's move is time-scaled to
  // fit it and every enemy move (walk, glide fallback or bite) runs for the
  // same length, so all units of a turn start and stop together. Tunable at
  // runtime like gWalkBlendDuration.
  float gTurnDuration = 2.5f;

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

    // Turn duration used by the node-only fallback (seconds).
    constexpr float kWalkTurnDuration = 0.4f;

    // Signals the walk states use to move the machine through its phases.
    enum WalkSignal : SignalId
    {
      WalkLoop = 1,     // Start clip finished; stride in the loop clip.
      WalkEnd  = 2,     // Close enough; play the end clip to the stop.
      WalkToStart = 3   // Turn finished; start the wind-up.
    };

    // Horizontal distance between two world points.
    float HorizontalDistance(const Vec3& a, const Vec3& b)
    {
      float dx = b.x - a.x;
      float dz = b.z - a.z;
      return glm::sqrt(dx * dx + dz * dz);
    }

    // Remaining horizontal distance from the moving actor to the target.
    float WalkRemaining(const AnimatedUnit::WalkContext& ctx)
    {
      if (ctx.actorNode == nullptr)
      {
        return 0.0f;
      }

      Vec3 pos = ctx.actorNode->GetTranslation(TransformationSpace::TS_WORLD);
      return HorizontalDistance(pos, ctx.targetPos);
    }

    // Net horizontal root travel a clip makes when played from its first to its
    // last key (see ClipMotion.h: MeasureClipRootTravel) is the exact distance
    // the clip walks its actor; the walk uses it as the landing clip's reach.

    // Machine seconds the stride clip needs to cover distance. Its curve
    // repeats every cycle: each full cycle adds totalTravel over duration
    // seconds (the engine re-measures from the cycle start at the wrap), and
    // the leftover is covered by a partial cycle.
    float LoopTravelTime(const ClipMotion& loop, float distance)
    {
      if (distance <= 0.0f || loop.totalTravel <= 0.0001f || loop.duration <= 0.0f)
      {
        return 0.0f;
      }

      float full = glm::floor(distance / loop.totalTravel);
      float rem  = distance - full * loop.totalTravel;
      return full * loop.duration + loop.TimeToTravel(rem);
    }

    // The walk's NATURAL duration (machine seconds at playback speed 1): how
    // long the FSM phases (optional turn + wind-up + stride + landing) run
    // until the actor reaches the destination node. Predicts the arrival
    // thresholds exactly the way the states gate on them, from the measured
    // clip timings.
    float EstimateWalkDuration(float turnDur,
                               float gap,
                               const ClipMotion& start,
                               const ClipMotion& loop,
                               const ClipMotion& end)
    {
      float d = turnDur; // 0 unless an in-place turn precedes the walk.

      float S = start.totalTravel;
      if (gap <= S)
      {
        // The wind-up alone covers the gap: arrive inside the wind-up as soon
        // as the remaining distance drops to the arrival epsilon.
        d += start.TimeToTravel(glm::max(0.0f, gap - kWalkArriveEps));
        return d;
      }

      // The wind-up plays through, then the landing clip covers the last
      // endReach; whatever remains above that needs stride cycles.
      d += start.duration;
      float remainingAfterStart = gap - S;
      float E = end.totalTravel;

      if (remainingAfterStart <= E)
      {
        d += end.TimeToTravel(glm::max(0.0f, remainingAfterStart - kWalkArriveEps));
        return d;
      }

      d += LoopTravelTime(loop, remainingAfterStart - E);
      d += end.TimeToTravel(glm::max(0.0f, E - kWalkArriveEps));
      return d;
    }

    // Switches the clip the animation controller plays using a short pose
    // crossfade (AnimControllerComponent::SmoothTransition fills the record
    // blending data; the engine fades the skeleton pose between the outgoing
    // and the incoming clip). The outgoing record must stop contributing root
    // motion for the blend: while it still sits in the animation player it
    // would otherwise drive the actor together with the incoming clip and
    // double the travelled distance.
    // Yaw (radians) about the world +Y axis carried by a (pure-yaw) rotation.
    float YawOf(const Quaternion& q)
    {
      return glm::atan(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    }

    // Rotation about the world +Y axis by the given yaw (radians). Note:
    // glm::rotate expects radians; the callers already pass radians, so no
    // degrees conversion here (a stray glm::degrees used to scale the yaw by
    // 180/pi, leaving the fold facing a near-random direction).
    Quaternion YawRotation(float yaw)
    {
      glm::mat4 m = glm::rotate(glm::mat4(1.0f), yaw, Vec3(0.0f, 1.0f, 0.0f));
      return glm::quat_cast(m);
    }

    // Shortest signed yaw (radians) that rotates the horizontal direction fwd
    // onto dir around +Y. Result is in [-pi, pi].
    float YawDeltaTo(const Vec3& fwd, const Vec3& dir)
    {
      const Vec3 up(0.0f, 1.0f, 0.0f);
      return glm::atan(glm::dot(glm::cross(fwd, dir), up), glm::dot(fwd, dir));
    }

    // Turn clip that matches a signed yaw delta in degrees, or "" when no turn
    // is needed. Grid moves are axis aligned, so the delta is a multiple of
    // 90 degrees.
    String TurnClipFor(float yawDeg)
    {
      int steps = static_cast<int>(glm::round(yawDeg / 90.0f));
      steps     = glm::clamp(steps, -2, 2);
      switch (steps)
      {
        case 1: return "turn_l_90";
        case 2: return "turn_l_180";
        case -1: return "turn_r_90";
        case -2: return "turn_r_180";
        default: return "";
      }
    }

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

    // Leading phase: turns the player toward the destination.
    //
    // Primary path: the turn clips were re-authored so their rotation is ROOT
    // MOTION (bone space only does in-place stepping). The clip is played with
    // m_applyRootMotion = true and the engine yaws the actor node itself. When
    // the clip ends, the turn is folded into the persistent facing: the prefab
    // top root is set to the target yaw and the actor's local orientation is
    // restored to its pre-turn base, so the following front-authored walk clip
    // starts clean.
    //
    // Fallback path (no usable clip): the top root is yawed directly over
    // kWalkTurnDuration.
    class WalkTurnState : public State
    {
     public:
      explicit WalkTurnState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        if (m_ctx != nullptr && m_ctx->anim != nullptr && !m_ctx->turnSignal.empty())
        {
          BlendTo(m_ctx->anim, m_ctx->turnSignal);
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

        // Node-only fallback: yaw the top root across the turn duration.
        if (m_ctx->turnSignal.empty())
        {
          if (m_ctx->rootNode == nullptr)
          {
            return State::NullSignal;
          }

          float t = (m_ctx->turnDur > 0.0f) ? glm::min(m_elapsed / m_ctx->turnDur, 1.0f) : 1.0f;
          float yaw = m_ctx->turnYawFrom + (m_ctx->turnYawTo - m_ctx->turnYawFrom) * t;
          m_ctx->rootNode->SetOrientation(YawRotation(yaw), TransformationSpace::TS_WORLD);

          if (m_elapsed < m_ctx->turnDur)
          {
            return State::NullSignal;
          }
        }
        else if (m_elapsed < m_ctx->turnDur)
        {
          // Clip path: the engine is rotating the actor node via root motion;
          // nothing to do here until the clip plays through.
          return State::NullSignal;
        }

        // Turn finished: fold the rotation into the persistent facing. The
        // actor node has been yawed by the root motion during the clip (or the
        // top root directly in the fallback); orient the top root at the exact
        // target yaw and put the actor back to its pre-turn local pose so the
        // front-authored walk clips start clean.
        if (m_ctx->rootNode != nullptr)
        {
          m_ctx->rootNode->SetOrientation(YawRotation(m_ctx->turnYawTo),
                                          TransformationSpace::TS_WORLD);
        }
        if (m_ctx->actorNode != nullptr && !m_ctx->turnSignal.empty())
        {
          m_ctx->actorNode->SetOrientation(m_ctx->actorBaseOrient, TransformationSpace::TS_LOCAL);
        }

        m_ctx->turning = false;
        TK_LOG("WalkState: turn done (elapsed %.2f/%.2f%s).",
               m_elapsed,
               m_ctx->turnDur,
               m_ctx->turnSignal.empty() ? " node" : " clip");
        return WalkToStart;
      }

      String Signaled(SignalId signal) override
      {
        switch (signal)
        {
          case WalkToStart: return "WalkStart";
          default: return "";
        }
      }

      String GetType() override { return "WalkTurn"; }

     private:
      AnimatedUnit::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // First walk phase: plays the wind-up clip (walk_f_start) once with root
    // motion. Hands over to the stride loop (or straight to the end clip when
    // the remaining gap already fits it) once the clip has played through.
    class WalkStartState : public State
    {
     public:
      explicit WalkStartState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

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

        // Wind-up played through. walk_f_start is a one-shot clip: when it
        // reaches its end the engine holds its final frame instead of wrapping
        // or dropping it, so starting the crossfade to the stride loop right at
        // the clip end is seamless.
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
      AnimatedUnit::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // Middle phase: keeps the STRIDE clip looping while root motion covers the
    // gap -- walk_f for a plain step, fight_walk_f when the move is an execution
    // (the attacker walks into a kill; see AnimatedUnit::StartAction). Leaves for
    // the landing phase the moment the remaining distance fits its root travel,
    // so the walk can stop exactly on the destination node no matter how long
    // the gap or which stride it is made of.
    class WalkLoopState : public State
    {
     public:
      explicit WalkLoopState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        if (m_ctx != nullptr && m_ctx->anim != nullptr)
        {
          BlendTo(m_ctx->anim, m_ctx->loopSignal);
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
      AnimatedUnit::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // Final phase: plays the landing clip (walk_f_end) with root motion. The
    // state ends the walk (ctx->arrived) when the actor reaches the node or the
    // clip plays through; AnimatedUnit::FinishWalk then snaps the actor onto
    // the exact node center and returns it to the idle loop.
    class WalkEndState : public State
    {
     public:
      explicit WalkEndState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

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

        // Arrive when the actor reaches the node or the end clip has fully
        // played. walk_f_end is a one-shot clip: at its end the engine holds
        // its final (stopped) pose, so the crossfade into idle starts from the
        // correct stopping pose -- no wrap, no extra clip restart.
        bool reached  = remaining <= kWalkArriveEps;
        bool clipDone = m_elapsed >= m_ctx->endDur;
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
      AnimatedUnit::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
    };

    // Terminal phase of an EXECUTION: plays the authored strike clip
    // (ambush_N) with root motion, which carries the attacker the last stretch
    // onto its victim and ends the action. Registered under the type name of
    // the landing phase ("WalkEnd"), because that is exactly the role it takes
    // over: the walk hands over to it where it would have played walk_f_end,
    // and its measured reach is what the approach is gated on. The strike is
    // also the moment the victim reacts: onStrike starts the paired reaction
    // clip, and the length it reports is how long the scene still runs after
    // the attacker's own clip is over.
    //
    // A SIDE strike hands over to this state while the victim is still turning
    // to face the attacker: the strike then HOLDS -- the attacker stands where
    // the approach left it, holding the pose it arrived on -- until that turn is
    // over, because the whole point of the turn is the front this strike needs.
    class ExecStrikeState : public State
    {
     public:
      explicit ExecStrikeState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override
      {
        m_elapsed = 0.0f;
        m_wait    = 0.0f;
        m_started = false;
        StartStrikeIfVictimReady();
      }

      void TransitionOut(State* nextState) override {}

      SignalId Update(float deltaTime) override
      {
        if (m_ctx == nullptr)
        {
          return State::NullSignal;
        }

        // Holding for the victim's front. The wait is bounded by the strike
        // clip's own length: a turn that somehow never ends must not lock the
        // turn flow, so after that the scene plays anyway (the clips are still
        // the pair the relation resolved, they just start on a turning victim).
        if (!m_started)
        {
          m_wait += deltaTime;
          const bool ready = VictimReady();
          if (!ready && m_wait < m_ctx->endDur)
          {
            // Stand still where the approach left off: the walk clip that is
            // still active would otherwise keep driving the actor forward --
            // straight through the very victim it is waiting to strike.
            if (m_ctx->anim != nullptr)
            {
              if (AnimRecordPtr active = m_ctx->anim->GetActiveRecord())
              {
                active->m_applyRootMotion = false;
              }
            }
            return State::NullSignal;
          }

          if (!ready)
          {
            TK_LOG("Exec: the victim is still turning after %.2f s; striking anyway.",
                   m_wait);
          }
          StartStrike();
        }

        m_elapsed += deltaTime;
        float remaining = WalkRemaining(*m_ctx);

        // The strike ends the action when the clip has played through (by then
        // its root motion has carried the attacker onto the victim) or when the
        // actor is already standing on top of it.
        bool reached  = remaining <= kWalkArriveEps;
        bool clipDone = m_elapsed >= m_ctx->endDur;
        if (reached || clipDone)
        {
          TK_LOG("Exec: strike finished (elapsed %.2f/%.2f, remaining %.3f).",
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
      // True when the strike may start: the victim's pre-strike turn is over.
      // Always true for a scene whose victim never had to turn.
      bool VictimReady() const
      {
        return (m_ctx->victimTurning == nullptr) || !m_ctx->victimTurning();
      }

      // First frame of the strike: the attacker's clip starts, the victim's
      // paired reaction starts with it, and the scene clock begins.
      void StartStrikeIfVictimReady()
      {
        if (m_ctx == nullptr)
        {
          return;
        }

        if (VictimReady())
        {
          StartStrike();
        }
        else
        {
          m_ctx->waitingForVictim = true;
        }
      }

      void StartStrike()
      {
        m_started               = true;
        m_ctx->waitingForVictim = false;
        m_ctx->sinceProgress    = 0.0f; // The strike's travel gets a fresh watchdog.

        if (m_ctx->anim != nullptr && !m_ctx->execSignal.empty())
        {
          if (AnimRecordPtr rec = m_ctx->anim->GetAnimRecord(m_ctx->execSignal))
          {
            rec->m_loop            = false; // One-shot; holds its final frame.
            rec->m_applyRootMotion = true;  // It carries the strike's movement.
          }
          BlendTo(m_ctx->anim, m_ctx->execSignal);
        }

        float victimDur      = (m_ctx->onStrike != nullptr) ? m_ctx->onStrike() : 0.0f;
        m_ctx->execSceneDur  = glm::max(m_ctx->endDur, victimDur);
        m_ctx->strikeStarted = true;

        TK_LOG("Exec: strike '%s' started %.2f u from the victim (%.2f s clip, %.2f s scene).",
               m_ctx->execSignal.c_str(),
               m_ctx->endReach,
               m_ctx->endDur,
               m_ctx->execSceneDur);
      }

      AnimatedUnit::WalkContext* m_ctx;
      float m_elapsed = 0.0f;
      float m_wait    = 0.0f; // Machine seconds spent holding for the victim.
      bool m_started  = false;
    };

    // Terminal phase of an in-place turn: ends the unit's action as soon as
    // the turn phase hands over. Registered under the type name the turn phase
    // signals ("WalkStart"), so the existing WalkTurn -> WalkToStart transition
    // lands here instead of on a walking state.
    class InPlaceTurnDoneState : public State
    {
     public:
      explicit InPlaceTurnDoneState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

      void TransitionIn(State* prevState) override {}

      void TransitionOut(State* nextState) override {}

      SignalId Update(float deltaTime) override
      {
        if (m_ctx != nullptr)
        {
          m_ctx->arrived = true;
        }
        return State::NullSignal;
      }

      String Signaled(SignalId signal) override { return ""; }

      String GetType() override { return "WalkStart"; }

     private:
      AnimatedUnit::WalkContext* m_ctx;
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

    m_intendedMove     = nullptr;
    m_gliding          = false;
    m_glideDur         = 1.0f;
    m_glideT           = 0.0f;
    m_glideNode        = nullptr;
    m_hasArriveOrient  = false;
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

  void Unit::StartGlide(GridNode* node, float duration)
  {
    if (m_root == nullptr || node == nullptr || node == m_node)
    {
      return;
    }

    // Face the step right away so the root does not spin mid-glide; the exact
    // node/step bookkeeping happens when the glide lands (PlaceOnNode).
    Vec3 fromPos = m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    Vec3 dir     = node->center - fromPos;
    if (glm::length(dir) < 0.0001f)
    {
      PlaceOnNode(node);
      return;
    }
    FaceTowards(dir);

    m_glideFrom = fromPos;
    m_glideTo   = node->center;
    m_glideNode = node;
    m_glideDur  = glm::max(duration, 0.001f);
    m_glideT    = 0.0f;
    m_gliding   = true;
  }

  void Unit::StartMove(GridNode* node, float targetDuration)
  {
    // Plain glide fallback: used by units without an animation controller.
    float duration = (targetDuration > 0.0f) ? targetDuration : gTurnDuration;
    StartGlide(node, duration);
  }

  bool Unit::StartExecution(Unit* victim, float targetDuration)
  {
    // A unit with no animation cannot perform a strike scene: the caller keeps
    // its plain bite (walk / glide onto the victim's tile and eat).
    TK_LOG("Exec: this unit has no animation support; plain bite.");
    return false;
  }

  void Unit::StartTurn(GridDir dir)
  {
    if (m_root == nullptr)
    {
      return;
    }

    // Instant in-place rotation fallback (no turn clips on this unit).
    m_root->m_node->SetOrientation(RotationTo(Vec3(0.0f, 0.0f, -1.0f), FacingVector(dir)),
                                   TransformationSpace::TS_WORLD);
  }

  void Unit::TurnToFaceAttacker(GridDir towardAttacker, float scale)
  {
    // No animation support: the front is snapped onto the attacker. That is all
    // the head on strike needs, and because it is instant nothing ever waits for
    // it (IsTurning() stays false).
    Unit::StartTurn(towardAttacker);
  }

  void Unit::SetArrivalOrientation(const Quaternion& worldOrient)
  {
    m_arriveOrient    = worldOrient;
    m_hasArriveOrient = true;
  }

  void Unit::Frame(float deltaTime)
  {
    if (!m_gliding || m_root == nullptr || m_root->m_node == nullptr)
    {
      return;
    }

    // Engine frame deltas arrive in milliseconds; glide timers work in
    // seconds, matching every other animation/timer in this codebase.
    float dt = deltaTime * 0.001f;
    m_glideT = glm::min(1.0f, m_glideT + dt / m_glideDur);
    m_root->m_node->SetTranslation(glm::mix(m_glideFrom, m_glideTo, m_glideT),
                                   TransformationSpace::TS_WORLD);

    if (m_glideT >= 1.0f)
    {
      LandMove();
    }
  }

  void Unit::LandMove()
  {
    if (!m_gliding || m_glideNode == nullptr || m_root == nullptr)
    {
      m_hasArriveOrient = false;
      return;
    }

    GridNode* target = m_glideNode;
    m_gliding        = false;
    m_glideNode      = nullptr;

    // Snap onto the exact node center (this also updates m_node and faces the
    // step direction).
    PlaceOnNode(target);

    // A unit that must arrive already facing somewhere else (a seeker turning
    // toward its held heading on the arrival tile) overrides the step-facing
    // now.
    if (m_hasArriveOrient)
    {
      m_root->m_node->SetOrientation(m_arriveOrient, TransformationSpace::TS_WORLD);
      m_hasArriveOrient = false;
    }
  }

  bool AnimatedUnit::Init(EntityPtr root, GridGraph* grid)
  {
    // The unit stays bound to the prefab's top root (the tagged "root" node):
    // the game rotates this node to aim the character and anchors it on the
    // grid tiles. The skinned character (mesh + skeleton + animation
    // controller) hangs under it as a child and is the actor root motion
    // plays on. Actors without an animation controller (legacy simple actors)
    // simply have no walk animation and fall back to gliding.
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

    m_timeScale = 1.0f;

    // Settle the character into the idle loop between turns.
    if (m_walkAnim != nullptr)
    {
      if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
      {
        idle->m_loop            = true; // idle is a looping clip.
        idle->m_applyRootMotion = false;
      }
      BlendTo(m_walkAnim, "idle");
    }

    // Measure the walk clip timings once, so the natural move duration can be
    // predicted for the per-turn time scaling.
    EnsureWalkTimings();

    return true;
  }

  void AnimatedUnit::EnsureWalkTimings()
  {
    if (m_walkAnim == nullptr)
    {
      return;
    }

    auto rebuild = [](ClipMotion& timing,
                      AnimRecordPtr rec,
                      const AnimRecord*& cached) -> void
    {
      if (rec == nullptr || rec->m_animation == nullptr)
      {
        return; // Not loaded yet; keep the last good profile.
      }
      if (cached == rec.get())
      {
        return; // Same clip as measured before.
      }
      cached = rec.get();
      timing = MeasureClipMotion(rec->m_animation);
    };

    rebuild(m_timingStart, m_walkAnim->GetAnimRecord("walk_f_start"), m_timedStartRec);
    rebuild(m_timingLoop, m_walkAnim->GetAnimRecord(kStrideSignal), m_timedLoopRec);
    rebuild(m_timingEnd, m_walkAnim->GetAnimRecord("walk_f_end"), m_timedEndRec);

    // The stride an execution closes in with. Measured like the others, never
    // assumed: a fight walk that carries no root travel cannot cover a gap and
    // is reported so by HasTravel (StartAction then keeps walk_f).
    rebuild(m_timingFightLoop, m_walkAnim->GetAnimRecord(kFightStrideSignal), m_timedFightLoopRec);
  }

  AnimatedUnit::~AnimatedUnit()
  {
    delete m_walkSM;
    m_walkSM = nullptr;
    delete m_walkCtx;
    m_walkCtx = nullptr;
  }

  void Player::OnTurn(GridNode* playerNode, GridDir playerFacing)
  {
    m_hasMoved = false;
  }

  void AnimatedUnit::Frame(float deltaTime)
  {
    // An execution outlives its walk state machine: once the strike has landed
    // the attacker holds its final pose while the victim's (usually longer)
    // reaction plays out, and the kill only lands when that whole scene is
    // over. The scene clock runs at the action's tempo, like the clips.
    if (m_execActive)
    {
      m_execT += deltaTime * 0.001f * m_timeScale;
      if (m_execT >= m_execDur)
      {
        m_execActive = false;
        m_execAction = false;
        TK_LOG("Exec: scene finished after %.2f s; the attacker settles back.", m_execDur);
        ApplyMoveTimeScale(1.0f);
        m_execSignal.clear();

        if (m_walkAnim != nullptr)
        {
          if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
          {
            idle->m_loop            = true;
            idle->m_applyRootMotion = false;
          }
          BlendTo(m_walkAnim, "idle");
        }
      }
    }

    if (m_walkSM == nullptr || m_walkCtx == nullptr)
    {
      // No animated walk running: advance a glide fallback, if any.
      Unit::Frame(deltaTime);
      return;
    }

    // Engine frame deltas arrive in milliseconds; clip durations and the state
    // machine timers work in seconds, and the AnimationPlayer advances records
    // with the same millisecond-to-second conversion. The whole walk (machine
    // timers AND clip playback) runs at m_timeScale so it finishes in exactly
    // the requested target length.
    float dt = deltaTime * 0.001f * m_timeScale;

    WalkContext* ctx = m_walkCtx;
    if (ctx->actorNode == nullptr)
    {
      FinishWalk(true);
      return;
    }

    // Stall watchdog: root motion that never converges (misaligned direction,
    // missing walk data) must not lock the turn forever. The gap shrinking at
    // least a little every frame keeps the timer at zero. A turn-in-place
    // phase -- and a strike holding for the victim's pre-strike turn -- makes no
    // distance progress by design, so both are exempt.
    if (!ctx->turning && !ctx->waitingForVictim && dt < 0.5f)
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
        TK_LOG("Move: walk stalled (gap %.3f not shrinking); snapping to the tile.",
               remaining);
        FinishWalk(true);
        return;
      }
    }

    m_walkSM->Update(dt);

    // The strike phase just began: the execution scene is now running, and it
    // is what IsExecuting() reports until the victim's reaction is over too.
    if (ctx->strikeStarted && !m_execActive)
    {
      m_execActive = true;
      m_execT      = 0.0f;
      m_execDur    = ctx->execSceneDur;
    }

    if (ctx->arrived)
    {
      FinishWalk(false);
    }
  }

  void AnimatedUnit::StartMove(GridNode* node, float targetDuration)
  {
    if (node == nullptr || node == m_node)
    {
      return;
    }

    // Preferred: the shared animated walk. Without an animation controller (or
    // usable clips) the unit glides for the same target length, so every unit
    // of a turn still moves for the same time.
    if (!StartWalk(node, targetDuration))
    {
      float duration = (targetDuration > 0.0f) ? targetDuration : gTurnDuration;
      StartGlide(node, duration);
    }
  }

  void AnimatedUnit::LandMove()
  {
    if (m_walkSM != nullptr)
    {
      FinishWalk(true);
      return;
    }
    Unit::LandMove();
  }

  void AnimatedUnit::CancelArrivalTurn() { m_hasDeferredTurn = false; }

  void AnimatedUnit::StartTurn(GridDir dir)
  {
    Quaternion target = RotationTo(Vec3(0.0f, 0.0f, -1.0f), FacingVector(dir));
    if (HasAnimatedTurn())
    {
      StartInPlaceTurn(YawOf(target));
      return;
    }
    Unit::StartTurn(dir);
  }

  void AnimatedUnit::TurnToFaceAttacker(GridDir towardAttacker, float scale)
  {
    // The same in-place turn every other unit plays, at the ATTACKER's tempo:
    // the turn is part of the attacker's action window, so a stand-alone turn
    // (which fills a whole window of its own) would leave the victim still
    // turning when the strike lands.
    if (HasAnimatedTurn() && m_walkSM == nullptr)
    {
      Quaternion target = RotationTo(Vec3(0.0f, 0.0f, -1.0f), FacingVector(towardAttacker));
      StartInPlaceTurn(YawOf(target), scale);
      TK_LOG("Exec: the victim turns to face the attacker at x%.2f.", scale);
      return;
    }

    if (m_walkSM != nullptr)
    {
      // Already acting (a move of its own, or a turn toward another attacker):
      // leave that action alone. The strike then finds whichever front the
      // victim happens to be showing instead of being hijacked by a snap.
      TK_LOG("Exec: the victim cannot turn now; the strike takes it as it stands.");
      return;
    }

    // No turn clips on this unit: show the front at once rather than making the
    // strike wait for a turn that cannot play.
    TK_LOG("Exec: the victim faces the attacker instantly (no turn animation).");
    Unit::TurnToFaceAttacker(towardAttacker, scale);
  }

  bool AnimatedUnit::HasAnimatedTurn() const
  {
    if (m_walkAnim == nullptr)
    {
      return false;
    }

    static const char* kTurnClips[] = {"turn_l_90", "turn_l_180", "turn_r_90", "turn_r_180"};
    for (const char* name : kTurnClips)
    {
      if (AnimRecordPtr rec = m_walkAnim->GetAnimRecord(name))
      {
        if (rec->m_animation != nullptr)
        {
          return true;
        }
      }
    }
    return false;
  }

  void AnimatedUnit::TurnOnArrival(const Quaternion& worldOrient)
  {
    if (HasAnimatedTurn())
    {
      // Animate the turn once the current move lands.
      m_deferredTurn      = worldOrient;
      m_hasDeferredTurn   = true;
    }
    else
    {
      // No turn clips: apply the orientation instantly when the move lands.
      SetArrivalOrientation(worldOrient);
    }
  }

  void AnimatedUnit::StartInPlaceTurn(float targetYaw, float explicitScale)
  {
    if (m_walkAnim == nullptr || m_actor == nullptr || m_root == nullptr || m_walkSM != nullptr)
    {
      return;
    }

    // The turn lands exactly where the unit stands: no destination, no travel.
    WalkContext* ctx = new WalkContext();
    m_walkCtx         = ctx;
    ctx->actorNode    = m_actor->m_node;
    ctx->anim         = m_walkAnim;
    ctx->to           = m_node;
    ctx->startPos     = m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD);
    ctx->targetPos    = ctx->startPos;
    ctx->startDur     = 0.0f;
    ctx->endDur       = 0.0f;
    ctx->endReach     = 0.0f;
    ctx->totalDist    = 0.0f;
    ctx->bestRemaining = 0.0f;
    ctx->sinceProgress = 0.0f;
    ctx->arrived       = false;
    ctx->rootNode      = m_root->m_node;

    // Shortest signed yaw from the current facing to the target.
    Vec3 fwd = glm::normalize(glm::vec3(m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD) * Vec3(0.0f, 0.0f, -1.0f)));
    Vec3 tgt = glm::normalize(glm::vec3(YawRotation(targetYaw) * Vec3(0.0f, 0.0f, -1.0f)));
    float dYaw = YawDeltaTo(fwd, tgt);

    if (std::fabs(dYaw) <= 0.02f)
    {
      // Already facing the target: snap it exact and finish at once.
      delete ctx;
      m_walkCtx = nullptr;
      m_root->m_node->SetOrientation(YawRotation(targetYaw), TransformationSpace::TS_WORLD);
      return;
    }

    // Same turn decision as the walk machine's leading phase: the turn clip
    // rotates the actor through root motion; the fold lands the top root on
    // the exact target yaw. Node-only fallback when the clip is unusable.
    ctx->turnYawFrom = YawOf(m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD));
    ctx->turnYawTo   = targetYaw;
    ctx->actorBaseOrient = m_actor->m_node->GetOrientation(TransformationSpace::TS_LOCAL);
    ctx->turning     = true;

    AnimRecordPtr turnRec = m_walkAnim->GetAnimRecord(TurnClipFor(glm::degrees(dYaw)));
    if (turnRec != nullptr && turnRec->m_animation != nullptr && turnRec->m_animation->m_duration > 0.0f)
    {
      turnRec->m_loop            = false;
      turnRec->m_applyRootMotion = true;
      ctx->turnSignal = TurnClipFor(glm::degrees(dYaw));
      ctx->turnDur    = turnRec->m_animation->m_duration;
    }
    else
    {
      ctx->turnSignal.clear();
      ctx->turnDur = kWalkTurnDuration;
    }

    // Same rule as a walk: the turn closes in exactly gTurnDuration, so its
    // clip is slowed down or sped up to fill the window by a single scale. A
    // turn that belongs to an action already running (explicitScale > 0, i.e.
    // an arrival turn that shares the walk's budget) reuses that same scale so
    // the whole action keeps one tempo and closes in its own T.
    float scale = 1.0f;
    if (explicitScale > 0.0f)
    {
      scale = glm::clamp(explicitScale, 0.05f, 20.0f);
    }
    else if (gTurnDuration > 0.01f && ctx->turnDur > 0.01f)
    {
      scale = glm::clamp(ctx->turnDur / gTurnDuration, 0.05f, 20.0f);
    }
    ApplyMoveTimeScale(scale);

    TK_LOG("Move: in-place turn %.0f deg (%s), plays %.2f s (x%.2f).",
           glm::degrees(dYaw),
           ctx->turnSignal.empty() ? "node-only" : ctx->turnSignal.c_str(),
           ctx->turnDur / scale,
           scale);

    // Reported by IsTurning() for as long as this action runs: an attacker whose
    // strike needs this unit's front (a side strike) holds its strike until here.
    m_turningInPlace = true;

    m_walkSM = new StateMachine();
    m_walkSM->PushState(new WalkTurnState(ctx));
    m_walkSM->PushState(new InPlaceTurnDoneState(ctx));
    m_walkSM->m_currentState = m_walkSM->QueryState("WalkTurn");
    m_walkSM->m_currentState->TransitionIn(nullptr);
  }

  void AnimatedUnit::ApplyMoveTimeScale(float scale)
  {
    m_timeScale = scale;

    // Every clip that can play during a move: idle may still be fading out when
    // the move starts, the walk clips follow, the stride an execution closes in
    // with (fight_walk_f) is the same loop phase under another clip, the turn
    // clips cover the leading turn phase / in-place turns, and an execution's
    // strike clip is the landing phase of the same action.
    if (m_walkAnim != nullptr)
    {
      const String strike = m_execSignal;
      static const char* kScaledClips[] = {"idle",
                                           "walk_f_start",
                                           "walk_f",
                                           "walk_f_end",
                                           "fight_walk_f",
                                           "turn_l_90",
                                           "turn_l_180",
                                           "turn_r_90",
                                           "turn_r_180"};
      for (const char* name : kScaledClips)
      {
        if (AnimRecordPtr rec = m_walkAnim->GetAnimRecord(name))
        {
          rec->m_timeMultiplier = scale;
        }
      }

      if (!strike.empty())
      {
        if (AnimRecordPtr rec = m_walkAnim->GetAnimRecord(strike))
        {
          rec->m_timeMultiplier = scale;
        }
      }
    }
  }

  bool Player::TryMove(GridNode* node,
                       const std::function<bool(GridNode*)>& isOccupied,
                       Unit* victim)
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

    // An animated player walks to the tile; without animation support it lands
    // instantly. Either way the move is accepted and counts as this turn's
    // single step.
    //
    // A unit standing on the destination is struck first: the step becomes an
    // execution whose approach is this very walk and whose strike clip covers
    // the last stretch onto its prey (a patrol taken from behind never sees it
    // coming). Only when that does not fit -- the relation has no clip authored,
    // the clips are not loaded -- does the step fall back to the plain walk.
    if (victim != nullptr && StartExecution(victim))
    {
      m_hasMoved = true;
      return true;
    }

    if (!StartWalk(node, gTurnDuration))
    {
      PlaceOnNode(node);
      TK_LOG("Player: move to (%d, %d) stepped instantly (no walk animation).", node->ix, node->iz);
    }

    m_hasMoved = true;
    return true;
  }

  bool AnimatedUnit::StartWalk(GridNode* node, float targetDuration)
  {
    return StartAction(node, targetDuration, ExecutionPlan(), nullptr);
  }

  bool AnimatedUnit::StartExecution(Unit* victim, float targetDuration)
  {
    if (victim == nullptr || victim->GetNode() == nullptr || m_node == nullptr ||
        victim->GetNode() == m_node)
    {
      return false;
    }

    GridNode* dest = victim->GetNode();

    // Which side of the victim this strike comes from decides the scene: the
    // attacker walks in along `approach`, and the victim faces `their facing`
    // on the tile it is being attacked on. Nothing about the attacker's own
    // type enters the choice -- a patrol walking up the player's back and a
    // player sneaking up a patrol's back resolve to the same relation.
    GridDir approach = DirectionOf(dest->center - m_node->center);
    ExecRelation rel = ExecutionLibrary::RelationOf(approach, victim->GetFacingDir());

    ExecutionPlan plan = ExecutionLibrary::Resolve(rel, m_walkAnim);
    if (!plan.HasClip())
    {
      // Nothing authored for this relation (and no relation it can be turned
      // into), or the clips are not loaded: the caller keeps its plain step (a
      // bite for a patrol, a capture for the player).
      TK_LOG("Exec: no execution authored for a strike from the %s; plain step.",
             ExecRelationName(rel));
      return false;
    }

    if (plan.victimTurnsToAttacker)
    {
      // A strike over the victim's shoulder: it has no scene of its own, so the
      // victim is turned to face the attacker FIRST and the head on scene then
      // plays (StartAction starts that turn; the strike waits for it).
      TK_LOG("Exec: strike from the %s -> the victim shows its front, then '%s' + "
             "'%s' (starts %.2f u out).",
             ExecRelationName(rel),
             plan.clip->attackerSignal.c_str(),
             plan.clip->victimSignal.c_str(),
             plan.clip->StartDistance());
    }
    else
    {
      TK_LOG("Exec: strike from the %s -> '%s' + '%s' (starts %.2f u out).",
             ExecRelationName(rel),
             plan.clip->attackerSignal.c_str(),
             plan.clip->victimSignal.c_str(),
             plan.clip->StartDistance());
    }

    return StartAction(dest, targetDuration, plan, victim);
  }

  float AnimatedUnit::PlayExecutionReaction(const String& signal, float scale)
  {
    if (m_walkAnim == nullptr || m_actor == nullptr || signal.empty())
    {
      return 0.0f;
    }

    AnimRecordPtr rec = m_walkAnim->GetAnimRecord(signal);
    if (rec == nullptr || rec->m_animation == nullptr)
    {
      return 0.0f;
    }

    // The victim's half of the scene: a one-shot reaction whose own root motion
    // carries the victim where the strike throws it, played at the attacker's
    // tempo so both halves stay in step. It is not a walk, so it never touches
    // the victim's tile: the reaction is the last thing this unit does.
    rec->m_loop            = false;
    rec->m_applyRootMotion = true;
    rec->m_timeMultiplier  = scale;
    BlendTo(m_walkAnim, signal);

    TK_LOG("Exec: victim reaction '%s' playing (%.2f s at x%.2f).",
           signal.c_str(),
           rec->m_animation->m_duration,
           scale);

    return rec->m_animation->m_duration;
  }

  float AnimatedUnit::ActiveAnimRemaining() const
  {
    if (m_walkAnim == nullptr)
    {
      return 0.0f;
    }

    AnimRecordPtr rec = m_walkAnim->GetActiveRecord();
    if (rec == nullptr || rec->m_animation == nullptr || rec->m_loop)
    {
      // Nothing playing, or a LOOPING clip: an idle loop never settles, so there
      // is no death animation to wait for.
      return 0.0f;
    }

    float left = rec->m_animation->m_duration - rec->m_currentTime;
    if (left <= 0.0f)
    {
      return 0.0f; // A one-shot that reached its end holds its final frame.
    }

    // Clip time advances at the record's own multiplier -- the action tempo the
    // scene was played at -- so the left over CLIP time is divided by it to get
    // the wall clock seconds the caller has to wait.
    const float rate = rec->m_timeMultiplier;
    return (rate > 0.0001f) ? left / rate : left;
  }

  bool AnimatedUnit::StartAction(GridNode* node,
                                 float targetDuration,
                                 const ExecutionPlan& plan,
                                 Unit* victim)
  {
    const ExecutionClip* exec = plan.clip;
    if (m_walkAnim == nullptr || m_actor == nullptr || m_root == nullptr)
    {
      return false; // No animation support; the caller picks the fallback.
    }

    AnimRecordPtr startRec = m_walkAnim->GetAnimRecord("walk_f_start");
    AnimRecordPtr loopRec  = m_walkAnim->GetAnimRecord("walk_f");
    AnimRecordPtr endRec   = m_walkAnim->GetAnimRecord("walk_f_end");
    if (startRec == nullptr || loopRec == nullptr || endRec == nullptr)
    {
      TK_LOG("Move: walk clips are missing on the animation controller; falling back.");
      return false;
    }

    if (startRec->m_animation == nullptr || endRec->m_animation == nullptr)
    {
      TK_LOG("Move: walk clip resources are not loaded; falling back.");
      return false;
    }

    Vec3 stepDir = (m_node != nullptr) ? (node->center - m_node->center)
                                        : (node->center - m_root->m_node->GetTranslation(TransformationSpace::TS_WORLD));

    // Clip semantics: the wind-up and stop clips play once and hold their
    // final frame (m_loop = false); the stride clip and idle loop. Root motion
    // is enabled on the three walk clips.
    startRec->m_loop           = false;
    loopRec->m_loop            = true;
    endRec->m_loop             = false;
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
    ctx->endReach     = MeasureClipRootTravel(endRec);
    ctx->totalDist    = HorizontalDistance(ctx->startPos, ctx->targetPos);
    ctx->bestRemaining = ctx->totalDist;
    ctx->sinceProgress = 0.0f;
    ctx->arrived       = false;
    ctx->rootNode     = m_root->m_node;

    // Execution: the authored strike clip takes the landing phase's place, so
    // its measured reach is the distance the approach stops at and its length
    // is how long the final phase runs. The victim's side of the scene starts
    // the moment the strike does, at the very same tempo.
    if (exec != nullptr)
    {
      m_execSignal      = exec->attackerSignal;
      m_execAction      = true;
      ctx->execSignal   = exec->attackerSignal;
      ctx->endDur       = exec->AttackDuration();
      ctx->endReach     = exec->StartDistance();
      ctx->onStrike     = [this, victim, signal = exec->victimSignal]() -> float
      {
        return (victim != nullptr) ? victim->PlayExecutionReaction(signal, m_timeScale)
                                   : 0.0f;
      };
    }

    // Decide whether the unit must turn in place before walking. Preferred
    // path: the re-authored turn clips rotate the actor through ROOT MOTION
    // (bone space only steps in place). Fallback: node-only yaw when no usable
    // clip exists.
    Vec3 fwd = glm::normalize(glm::vec3(m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD) * Vec3(0.0f, 0.0f, -1.0f)));
    float dYaw = YawDeltaTo(fwd, stepDir);
    if (std::fabs(dYaw) > 0.02f)
    {
      ctx->turnYawFrom = YawOf(m_root->m_node->GetOrientation(TransformationSpace::TS_WORLD));
      ctx->turnYawTo   = ctx->turnYawFrom + dYaw;
      ctx->actorBaseOrient = m_actor->m_node->GetOrientation(TransformationSpace::TS_LOCAL);
      ctx->turning     = true;

      AnimRecordPtr turnRec = m_walkAnim->GetAnimRecord(TurnClipFor(glm::degrees(dYaw)));
      if (turnRec != nullptr && turnRec->m_animation != nullptr && turnRec->m_animation->m_duration > 0.0f)
      {
        // Clip path: one-shot clip, its rotation is root motion.
        turnRec->m_loop           = false;
        turnRec->m_applyRootMotion = true;
        ctx->turnSignal = TurnClipFor(glm::degrees(dYaw));
        ctx->turnDur    = turnRec->m_animation->m_duration;
        TK_LOG("Move: turning %.0f deg (%s) before walking to (%d, %d).",
               glm::degrees(dYaw),
               ctx->turnSignal.c_str(),
               node->ix,
               node->iz);
      }
      else
      {
        // Fallback: no clip, rotate the top root directly.
        ctx->turnSignal.clear();
        ctx->turnDur = kWalkTurnDuration;
        TK_LOG("Move: turning %.0f deg (node-only) before walking to (%d, %d).",
               glm::degrees(dYaw),
               node->ix,
               node->iz);
      }
    }
    else
    {
      // Already facing the step; snap it exact and walk straight.
      FaceTowards(stepDir);
    }

    // Rule: EVERY action of a turn closes in exactly the target length
    // (default gTurnDuration), whatever phases it is made of. The natural
    // duration of the whole action is predicted from the measured clip timings
    // and a SINGLE time scale drives it, so the clips keep their proportions
    // relative to each other (the longer phase simply loses more seconds).
    float target = (targetDuration > 0.0f) ? targetDuration : gTurnDuration;

    // A move that ends with a queued in-place turn (a line patrol's about-face
    // on reaching its line end, a seeker's arrival look) has that turn's
    // natural length counted INTO the same budget, so the walk and the turn
    // share one scale and the action still closes in `target` seconds.
    float arrivalTurnDur = 0.0f;
    if (m_hasDeferredTurn && glm::length(stepDir) > 0.0001f)
    {
      Vec3 endFwd = glm::normalize(stepDir); // facing the walk ends on
      Vec3 tgtDir = glm::normalize(glm::vec3(m_deferredTurn * Vec3(0.0f, 0.0f, -1.0f)));
      float dTurn = std::fabs(YawDeltaTo(endFwd, tgtDir));
      if (dTurn > 0.02f)
      {
        arrivalTurnDur = kWalkTurnDuration; // node-only fallback length
        if (AnimRecordPtr turnRec = m_walkAnim->GetAnimRecord(TurnClipFor(glm::degrees(dTurn))))
        {
          if (turnRec->m_animation != nullptr && turnRec->m_animation->m_duration > 0.0f)
          {
            arrivalTurnDur = turnRec->m_animation->m_duration;
          }
        }
      }
    }

    EnsureWalkTimings();

    // WHICH STRIDE CLOSES IN. A plain step travels with walk_f; an EXECUTION
    // closes in with the FIGHT walk (fight_walk_f), so a kill reads as one: the
    // attacker is walking into its victim, not travelling. Only the LOOP phase
    // changes -- the wind-up it starts with and the phase that ends the action
    // (the strike clip) are the ones the action already chose -- and the chosen
    // clip's own measured motion is what the move is timed on below, so a
    // faster or slower combat cycle sizes its own approach.
    const ClipMotion* strideMotion = &m_timingLoop;
    if (exec != nullptr)
    {
      AnimRecordPtr fightRec   = m_walkAnim->GetAnimRecord(kFightStrideSignal);
      const bool fightPlayable = fightRec != nullptr && fightRec->m_animation != nullptr;
      if (fightPlayable && m_timingFightLoop.HasTravel())
      {
        fightRec->m_loop            = true; // A stride cycle: it loops.
        fightRec->m_applyRootMotion = true; // ... and it is what covers the gap.
        ctx->loopSignal             = kFightStrideSignal;
        strideMotion                = &m_timingFightLoop;
        TK_LOG("Exec: closing in on '%s' (%.2f s per %.3f u stride cycle).",
               kFightStrideSignal,
               m_timingFightLoop.duration,
               m_timingFightLoop.totalTravel);
      }
      else
      {
        TK_LOG("Exec: '%s' is %s; closing in with '%s'.",
               kFightStrideSignal,
               fightPlayable ? "carrying no root travel" : "not on this character",
               kStrideSignal);
      }
    }

    // Natural length of the action. For a plain step that is the walk's own
    // phases (turn + wind-up + strides + landing clip); for an execution the
    // strike clip IS the landing phase -- it covers the last `startDistance` of
    // the gap exactly the way walk_f_end covers its own reach -- so it is
    // measured in its place. A gap already inside the strike's reach skips the
    // approach entirely: the wind-up plays and the strike follows.
    float naturalDur = 0.0f;
    if (exec != nullptr && ctx->totalDist <= exec->StartDistance())
    {
      naturalDur = m_timingStart.duration + exec->AttackDuration();
    }
    else
    {
      naturalDur = EstimateWalkDuration(ctx->turning ? ctx->turnDur : 0.0f,
                                        ctx->totalDist,
                                        m_timingStart,
                                        *strideMotion,
                                        (exec != nullptr) ? exec->attackerMotion : m_timingEnd);
    }

    float actionNatural = naturalDur + arrivalTurnDur;
    float scale = 1.0f;
    if (target > 0.01f && actionNatural > 0.01f && m_timingStart.duration > 0.0f)
    {
      scale = glm::clamp(actionNatural / target, 0.05f, 20.0f);
    }

    // The scale drives the FSM timers and the clip playback (including blend
    // countdowns) together, and the queued turn reuses the same value when it
    // plays after the landing, so the whole action closes in `target` seconds.
    ApplyMoveTimeScale(scale);

    TK_LOG("Move: natural %.2f s -> %.2f s (x%.2f, T %.2f), turn %s%s%s.",
           actionNatural,
           actionNatural / scale,
           scale,
           target,
           ctx->turning ? "yes" : "no",
           arrivalTurnDur > 0.0f ? ", arrival turn included" : "",
           exec != nullptr ? ", execution strike" : "");

    // A side strike starts with the VICTIM's turn: the victim shows its front to
    // the attacker while the approach runs, so the head on scene that follows
    // finds the two facing each other. It plays at this action's tempo -- the
    // turn is part of the same window -- and its length is NOT counted into the
    // budget above: the strike phase holds for it instead (see ExecStrikeState),
    // which keeps the two in step whatever the clips measure.
    if (plan.victimTurnsToAttacker && victim != nullptr && glm::length(stepDir) > 0.0001f)
    {
      GridDir towardAttacker = OppositeDir(DirectionOf(stepDir));
      ctx->victimTurning     = [victim]() -> bool { return victim->IsTurning(); };
      victim->TurnToFaceAttacker(towardAttacker, scale);
    }

    m_walkSM = new StateMachine();
    m_walkSM->PushState(new WalkTurnState(ctx));
    m_walkSM->PushState(new WalkStartState(ctx));
    m_walkSM->PushState(new WalkLoopState(ctx));
    // The landing phase: the walk's own stop clip, or the authored strike that
    // takes its place in an execution (same state machine type name, so the
    // walk hands over to it exactly where it would have played walk_f_end).
    m_walkSM->PushState(exec != nullptr ? static_cast<State*>(new ExecStrikeState(ctx))
                                        : static_cast<State*>(new WalkEndState(ctx)));
    m_walkSM->m_currentState = m_walkSM->QueryState(ctx->turning ? "WalkTurn" : "WalkStart");
    m_walkSM->m_currentState->TransitionIn(nullptr);

    TK_LOG("Move: walk started (%d, %d) -> (%d, %d), gap %.2f, end clip reach %.2f.",
           m_node != nullptr ? m_node->ix : -1,
           m_node != nullptr ? m_node->iz : -1,
           node->ix,
           node->iz,
           ctx->totalDist,
           ctx->endReach);
    return true;
  }

  void AnimatedUnit::FinishWalk(bool forceSnap)
  {
    WalkContext* ctx = m_walkCtx;
    GridNode* dest   = (ctx != nullptr) ? ctx->to : nullptr;
    Vec3 targetPos   = (ctx != nullptr) ? ctx->targetPos : Vec3(0.0f);

    // An execution is NOT over when its walk machine is: the attacker holds the
    // strike's final pose (the one-shot clip holds it by itself) until the
    // victim's reaction, which is usually much longer, has played out. So the
    // idle settle and the tempo reset are skipped here and happen when the
    // scene ends (see Frame). An execution whose walk never made it to its
    // strike -- the stall watchdog landed it early, or the run ended mid
    // approach -- is treated like any other landing and its action ends here,
    // so nothing keeps waiting for a scene that will never play.
    const bool execution = (ctx != nullptr) && !ctx->execSignal.empty();
    const bool execScene = execution && m_execActive;

    // The scale the finished action ran at: a queued arrival turn must reuse it
    // so it stays part of the same, already-budgeted action.
    const float actionScale = m_timeScale;

    delete m_walkSM;
    m_walkSM  = nullptr;
    m_walkCtx = nullptr;

    // Whatever action this was -- an in-place turn included -- it is over, so a
    // strike waiting for this unit's front may go ahead.
    m_turningInPlace = false;

    if (ctx != nullptr)
    {
      // How far the actor was from the node center when the walk ended; a
      // normal arrival should be within a few centimeters, anything bigger
      // means the end clip overshot or the walk was interrupted.
      if (m_actor != nullptr && m_actor->m_node != nullptr)
      {
        Vec3 pos = m_actor->m_node->GetTranslation(TransformationSpace::TS_WORLD);
        TK_LOG("Move: arrival snap residual %.3f u (%s).",
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

    // A unit that must arrive already facing somewhere else (a seeker turning
    // toward its held heading on the arrival tile) overrides the step-facing
    // now that the move has landed.
    if (m_hasArriveOrient)
    {
      if (m_root != nullptr)
      {
        m_root->m_node->SetOrientation(m_arriveOrient, TransformationSpace::TS_WORLD);
      }
      m_hasArriveOrient = false;
    }

    // The walk is over: restore normal playback speed before the idle settle
    // blend, so the loop and its future fades run at 1x again. An execution
    // keeps its tempo and its pose until its whole scene is over -- and a walk
    // that never reached its strike (the stall watchdog landed it early) is
    // treated like any other landing, so the unit cannot get stuck holding a
    // pose with no scene left to end it.
    if (!execScene)
    {
      if (execution)
      {
        m_execAction = false;
        m_execSignal.clear();
      }

      ApplyMoveTimeScale(1.0f);

      // Settle the character back into the idle loop with a crossfade. BlendTo
      // also turns off the root motion of the outgoing walk clip, so the
      // snapped-to-node actor does not drift while the end clip fades out.
      if (m_walkAnim != nullptr)
      {
        if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
        {
          idle->m_loop            = true; // idle is a looping clip.
          idle->m_applyRootMotion = false;
        }
        BlendTo(m_walkAnim, "idle");
      }
    }

    // A patrol that must turn to a heading / idle stare the moment its move
    // lands (a seeker arriving at the last seen tile, a line patrol reaching
    // the end of its line) plays that turn ANIMATED now, the way the player
    // would, instead of snapping. Falls back to an instant orientation without
    // turn clips; a forced landing (stalled walk, end of the run) just takes
    // the orientation without starting a new action. An execution drops it: the
    // strike ends the action, so the attacker holds its pose instead of turning
    // on the body.
    if (m_hasDeferredTurn && !execScene)
    {
      Quaternion target = m_deferredTurn;
      m_hasDeferredTurn = false;
      if (!forceSnap && HasAnimatedTurn())
      {
        // Reuse the finished walk's scale: this turn was budgeted inside the
        // same action, so it must keep the same tempo and close the action.
        StartInPlaceTurn(YawOf(target), actionScale);
      }
      else if (m_root != nullptr)
      {
        m_root->m_node->SetOrientation(target, TransformationSpace::TS_WORLD);
      }
    }

    if (dest != nullptr)
    {
      TK_LOG("Move: walk finished on (%d, %d).", dest->ix, dest->iz);
    }
  }

  void AnimatedUnit::StopAnimation()
  {
    if (m_walkAnim != nullptr)
    {
      m_walkAnim->Stop();
    }
  }

  void AnimatedUnit::SettleExecutionScene()
  {
    if (!m_execActive && !m_execAction)
    {
      return;
    }

    // What the scene clock does when it runs out (see Frame), done NOW instead.
    // The attacker's ACTION is over, so nothing here may keep the turn waiting --
    // and the walk machine is already gone, so the idle settle it skipped has to
    // happen here. The victim's death animation is left alone: it plays on a body
    // that is already out of the game, and no action of this unit depends on it.
    m_execActive = false;
    m_execAction = false;
    m_execT      = 0.0f;
    m_execDur    = 0.0f;

    // Same order as the scene clock's own end (see Frame): the clips the action
    // drove go back to 1x before the signal that names the strike is dropped.
    ApplyMoveTimeScale(1.0f);
    m_execSignal.clear();

    if (m_walkAnim != nullptr)
    {
      if (AnimRecordPtr idle = m_walkAnim->GetAnimRecord("idle"))
      {
        idle->m_loop            = true;
        idle->m_applyRootMotion = false;
      }
      BlendTo(m_walkAnim, "idle");
    }

    TK_LOG("Exec: the attacker's action is over; it settles into idle while the body plays on.");
  }

  void AnimatedUnit::Reset()
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

    m_timeScale = 1.0f;
    m_timingStart = ClipMotion();
    m_timingLoop  = ClipMotion();
    m_timingEnd   = ClipMotion();
    m_timingFightLoop = ClipMotion();
    m_timedStartRec = nullptr;
    m_timedLoopRec  = nullptr;
    m_timedEndRec   = nullptr;
    m_timedFightLoopRec = nullptr;
    m_hasDeferredTurn = false;
    m_turningInPlace  = false;

    // A running execution scene never outlives the session either.
    m_execAction = false;
    m_execActive = false;
    m_execT      = 0.0f;
    m_execDur    = 0.0f;
    m_execSignal.clear();

    Unit::Reset();
  }

  void Player::Reset()
  {
    m_hasMoved = false;
    AnimatedUnit::Reset();
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

  void StationaryPatrol::Lunge(Unit* victim)
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
    // wall. The guard already faces this way, so the step reads purely as a
    // strike. When an execution is authored for the relation between the guard
    // and its prey (a prey walking away from it, showing its back) the strike
    // clip performs it; otherwise the shared animated move (or glide fallback)
    // makes the bite visible and the game eats the player when it lands.
    if (victim != nullptr && StartExecution(victim))
    {
      TK_LOG("Guard: ambushes the prey on (%d, %d).", watched->ix, watched->iz);
      return;
    }

    StartMove(watched);
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

    m_intendedMove = nullptr;

    // One tile per turn along the facing line. A connected neighbour keeps the
    // patrol moving; a missing or blocked one means the line ends here, so the
    // patrol turns 180 degrees in place and walks back next turn. Enemies do
    // not block each other, so the tile ahead is only checked for a connection.
    // The step itself is recorded (m_intendedMove) and started by the game, so
    // this patrol moves at the same time as everyone else this turn.
    //
    // The about-face does NOT cost a turn of its own: when the step being taken
    // already reaches the end of the line (the tile beyond the step target is
    // missing or blocked), the patrol turns as it lands -- the turn plays right
    // after that move, inside the same turn.
    GridDir facing = GetFacingDir();
    GridNode* next  = m_grid->Neighbor(*m_node, facing);
    if (next != nullptr && m_grid->Connected(*m_node, *next))
    {
      m_intendedMove = next;
      TK_LOG("Linear: line step to (%d, %d).", next->ix, next->iz);

      GridNode* beyond = m_grid->Neighbor(*next, facing);
      if (beyond == nullptr || !m_grid->Connected(*next, *beyond))
      {
        // This step lands on the last tile of the line: about-face on arrival.
        TurnOnArrival(RotationTo(Vec3(0.0f, 0.0f, -1.0f), FacingVector(OppositeDir(facing))));
        TK_LOG("Linear: step reaches the line end; turning around on arrival.");
      }
    }
    else
    {
      // Already at the line end (blocked straight away): turn in place now.
      StartTurn(OppositeDir(facing));
      TK_LOG("Linear: line ended; turning around in place.");
    }
  }

  bool SeekerPatrol::Init(EntityPtr root, GridGraph* grid)
  {
    if (!AnimatedUnit::Init(root, grid))
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
    // A standing look: from the patrol's current tile along its current facing.
    return SeesAlong(m_node, GetFacingDir(), playerNode);
  }

  bool SeekerPatrol::SeesAlong(GridNode* origin, GridDir dir, GridNode* playerNode) const
  {
    if (origin == nullptr || m_grid == nullptr || playerNode == nullptr)
    {
      return false;
    }

    // Line of sight runs along the facing direction through connected tiles,
    // until a blocked passage, the grid edge, or the player. The origin and
    // direction are explicit: the arrival look on the turn a seeker lands on
    // the last seen tile is taken from THAT tile along the held heading, which
    // is decided before the glide actually lands (and before the root has been
    // rotated to that heading).
    GridNode* cursor = origin;
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
    m_intendedMove = nullptr;

    std::vector<GridNode*> path = FindPath(m_lastSeen);
    GridNode* step = nullptr; // Tile stepped to this turn, when the chase walks.
    if (path.size() > 1)
    {
      step = path[1];
      m_intendedMove = step;
      m_trail.push_back(step);
      TK_LOG("Seeker: chase step to (%d, %d), %d tile(s) to go.", step->ix, step->iz, (int) path.size() - 2);
    }

    // Logical position after this turn's step. The physical glide lands over
    // the coming frames (StartGlide by the game), so all the arrival logic
    // below reads the tile the patrol WILL stand on, not its departure tile.
    GridNode* here = (step != nullptr) ? step : m_node;

    // Landing on the player's tile IS the bite, so the chase ends right here:
    // there is nobody left to look for. The patrol holds exactly where it
    // stopped, still facing the way it walked in -- it does not turn to the
    // memorized heading, does not take an arrival look down a line it is
    // standing in, and never enters the wait. It stays in Chasing; the game
    // resolves the loss when the step lands.
    if (here == playerNode)
    {
      TK_LOG("Seeker: caught the player at (%d, %d); holding position.", here->ix, here->iz);
      return;
    }

    // The chase leg ends when the patrol lands on the last sighting tile, or at
    // once when that tile turns out to be unreachable. Landing, turning to the
    // heading frozen at sight loss and looking down it are ONE turn: the look is
    // taken before the player gets another step in, so a player fleeing straight
    // ahead of that heading is caught the moment the patrol arrives. Handing the
    // look to a later turn let it stand having already turned, watch the player
    // walk out of the very line it was staring down, and give up.
    if (step != nullptr && here != m_lastSeen)
    {
      return; // Still walking; the arrival turn has not come yet.
    }

    if (here == m_lastSeen)
    {
      TK_LOG("Seeker: arrived at the last seen tile (%d, %d); turning to memorized heading %s and looking.",
             here->ix,
             here->iz,
             GridDirName(m_lastHeading));
    }
    else
    {
      TK_LOG("Seeker: last seen tile (%d, %d) unreachable; turning to memorized heading %s and looking.",
             m_lastSeen != nullptr ? m_lastSeen->ix : -1,
             m_lastSeen != nullptr ? m_lastSeen->iz : -1,
             GridDirName(m_lastHeading));
    }

    // The turn above already points the stare, so the very same turn can see
    // along it. When the arrival is a step, the turn to the held heading is
    // played ANIMATED the moment the move lands (like the player would); a
    // patrol standing on the tile already turns in place, animated when it has
    // turn clips.
    if (step != nullptr)
    {
      TurnOnArrival(HeadingRotation(m_lastHeading));
    }
    else
    {
      StartTurn(m_lastHeading);
    }

    // A fresh sighting keeps the chase going from here -- the walk resumes on
    // the next turn, one step per turn as always -- while an empty line gives
    // the patrol up and sends it back along its trail. The look is taken from
    // the tile the patrol lands on (here) along the held heading, even though
    // the glide has not physically landed yet.
    if (SeesAlong(here, m_lastHeading, playerNode))
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
    m_intendedMove = nullptr;

    if (m_trail.size() > 1)
    {
      GridNode* back = m_trail[m_trail.size() - 2];
      m_trail.pop_back();
      m_intendedMove = back;
      TK_LOG("Seeker: return step to (%d, %d).", back->ix, back->iz);

      if (m_trail.size() == 1 && back == m_trail[0])
      {
        // Back at the start: resume the idle stare. The step still runs; the
        // stare orientation is applied (animated, when possible) as it lands.
        TK_LOG("Seeker: back at the start; resuming the idle stare.");
        m_state = State::Idle;
        TurnOnArrival(m_idleOrientation);
        m_lastSeen = nullptr;
      }
    }
    else
    {
      // No path to retrace: already back at the start.
      m_state = State::Idle;
      if (HasAnimatedTurn())
      {
        StartInPlaceTurn(YawOf(m_idleOrientation));
      }
      else
      {
        TurnToIdle();
      }
      m_lastSeen = nullptr;
    }
  }

  Quaternion SeekerPatrol::HeadingRotation(GridDir dir) const
  {
    Vec3 forward;
    switch (dir)
    {
      case GridDir::Xm: forward = Vec3(-1.0f, 0.0f, 0.0f); break;
      case GridDir::Xp: forward = Vec3(1.0f, 0.0f, 0.0f); break;
      case GridDir::Zm: forward = Vec3(0.0f, 0.0f, -1.0f); break;
      default: forward = Vec3(0.0f, 0.0f, 1.0f); break;
    }

    return RotationTo(Vec3(0.0f, 0.0f, -1.0f), forward);
  }

  void SeekerPatrol::TurnTo(GridDir dir)
  {
    if (m_root == nullptr)
    {
      return;
    }

    m_root->m_node->SetOrientation(HeadingRotation(dir), TransformationSpace::TS_WORLD);
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
    AnimatedUnit::Reset();
    m_startNode   = nullptr;
    m_lastSeen    = nullptr;
    m_lastHeading = GridDir::Zm;
    m_sighted     = false;
    m_watchLeft   = 0;
    m_state       = State::Idle;
    m_trail.clear();
  }

} // namespace ToolKit
