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

    // World-space direction vector a unit faces when heading toward dir.
    Vec3 FacingVector(GridDir d)
    {
      switch (d)
      {
        case GridDir::Xm: return Vec3(-1.0f, 0.0f, 0.0f);
        case GridDir::Xp: return Vec3(1.0f, 0.0f, 0.0f);
        case GridDir::Zm: return Vec3(0.0f, 0.0f, -1.0f);
        default: return Vec3(0.0f, 0.0f, 1.0f);
      }
    }

    // The grid direction opposite to d (180-degree turn on the grid).
    GridDir OppositeDir(GridDir d)
    {
      switch (d)
      {
        case GridDir::Xm: return GridDir::Xp;
        case GridDir::Xp: return GridDir::Xm;
        case GridDir::Zm: return GridDir::Zp;
        default: return GridDir::Zm;
      }
    }
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
  };

  // Crossfade length (seconds) used whenever the walk state machine switches
  // clips (idle -> walk_f_start -> walk_f -> walk_f_end -> idle). A global so
  // it can be tuned at runtime (e.g. bound to a settings value) instead of
  // being an inline constant; declared in Unit.h.
  float gWalkBlendDuration = 0.2f;

  // Enemy glide duration (seconds). Temporary stand-in until patrols get their
  // own walk state machines / animation.
  const float gPatrolGlideTime = 2.0f;

  // Every turn's action window (seconds). The player's move is time-scaled to
  // fit it and enemy tile steps glide for the same length, so all units of a
  // turn start and stop together. Tunable at runtime like gWalkBlendDuration.
  float gTurnDuration = 3.0f;

  float WalkClipTiming::TimeToTravel(float distance) const
  {
    if (distance <= 0.0f || keyTimes.empty() || keyTravel.empty())
    {
      return 0.0f;
    }
    if (distance >= totalTravel || totalTravel <= 0.0001f)
    {
      return duration;
    }

    for (size_t i = 1; i < keyTravel.size(); i++)
    {
      if (keyTravel[i] >= distance)
      {
        float segTrav = keyTravel[i] - keyTravel[i - 1];
        if (segTrav <= 0.0001f)
        {
          return keyTimes[i];
        }
        float ratio = (distance - keyTravel[i - 1]) / segTrav;
        return keyTimes[i - 1] + (keyTimes[i] - keyTimes[i - 1]) * ratio;
      }
    }
    return duration;
  }

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

    // Builds the measured timing model of one clip from its root key track:
    // the time of every reachable root key (frame / fps, clipped to the playable
    // duration) and how far the actor has travelled towards its destination at
    // that key. Maps any required remaining-distance drop to the clip time that
    // covers it, exactly the way the engine interpolates the root curve. All
    // timing is derived from the animation data -- no hardcoded durations.
    WalkClipTiming BuildClipTiming(AnimationPtr anim)
    {
      WalkClipTiming t;
      if (anim == nullptr)
      {
        return t;
      }

      t.duration = anim->m_duration;
      const KeyArray* keys = anim->m_keys.Find(anim->m_rootKey);
      if (keys == nullptr || keys->size() < 2)
      {
        return t;
      }

      const float fps = (anim->m_fps > 0.0f) ? anim->m_fps : 30.0f;
      const Vec3 first = keys->front().m_position;
      const Vec3 last  = keys->back().m_position;

      // Travel axis: the net horizontal displacement of the root curve. The
      // actor's progress toward a straight-ahead destination is the running max
      // of the signed projection of its position onto this axis -- a curve that
      // settles back slightly at its very end never reduces how far it has been.
      Vec3 axis(last.x - first.x, 0.0f, last.z - first.z);
      float axisLen = glm::length(axis);
      if (axisLen < 0.0001f)
      {
        return t; // No net horizontal travel (turn / idle style clip).
      }
      axis /= axisLen;

      float runMax = 0.0f;
      size_t n = keys->size();
      size_t i = 0;
      for (; i < n; i++)
      {
        const Key& k = (*keys)[i];
        float time = k.m_frame / fps;
        if (time > t.duration + 0.0001f)
        {
          break; // Past the playable end; the engine clamps at m_duration.
        }
        Vec3 d = k.m_position - first;
        runMax = glm::max(runMax, d.x * axis.x + d.z * axis.z);
        t.keyTimes.push_back(time);
        t.keyTravel.push_back(runMax);
      }

      // A key that crosses the playable end: interpolate the travel reached at
      // exactly m_duration and append it as the final sample.
      if (i < n && !t.keyTimes.empty() && i > 0)
      {
        const Key& kNext = (*keys)[i];
        const Key& kPrev = (*keys)[i - 1];
        float tNext = kNext.m_frame / fps;
        float tPrev = kPrev.m_frame / fps;
        if (tNext > tPrev)
        {
          float r = (t.duration - tPrev) / (tNext - tPrev);
          Vec3 pos = kPrev.m_position + (kNext.m_position - kPrev.m_position) * r;
          Vec3 d   = pos - first;
          runMax = glm::max(runMax, d.x * axis.x + d.z * axis.z);
          t.keyTimes.push_back(t.duration);
          t.keyTravel.push_back(runMax);
        }
      }

      if (!t.keyTravel.empty())
      {
        t.totalTravel = t.keyTravel.back();
      }
      return t;
    }

    // Machine seconds the stride clip needs to cover distance. Its curve
    // repeats every cycle: each full cycle adds totalTravel over duration
    // seconds (the engine re-measures from the cycle start at the wrap), and
    // the leftover is covered by a partial cycle.
    float LoopTravelTime(const WalkClipTiming& loop, float distance)
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
                               const WalkClipTiming& start,
                               const WalkClipTiming& loop,
                               const WalkClipTiming& end)
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

    // Middle phase: keeps the stride clip (walk_f) looping while root motion
    // covers the gap. Leaves for the end clip the moment the remaining distance
    // fits the end clip's own root travel, so the walk can stop exactly on the
    // destination node no matter how long the gap is.
    class WalkLoopState : public State
    {
     public:
      explicit WalkLoopState(AnimatedUnit::WalkContext* ctx) : m_ctx(ctx) {}

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

    auto rebuild = [](WalkClipTiming& timing,
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
      timing = BuildClipTiming(rec->m_animation);
    };

    rebuild(m_timingStart, m_walkAnim->GetAnimRecord("walk_f_start"), m_timedStartRec);
    rebuild(m_timingLoop, m_walkAnim->GetAnimRecord("walk_f"), m_timedLoopRec);
    rebuild(m_timingEnd, m_walkAnim->GetAnimRecord("walk_f_end"), m_timedEndRec);
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
    // phase legitimately makes no distance progress, so it is exempt.
    if (!ctx->turning && dt < 0.5f)
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

  void AnimatedUnit::StartInPlaceTurn(float targetYaw)
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
    m_timeScale        = 1.0f;

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
      TK_LOG("Move: in-place turn %.0f deg (%s).", glm::degrees(dYaw), ctx->turnSignal.c_str());
    }
    else
    {
      ctx->turnSignal.clear();
      ctx->turnDur = kWalkTurnDuration;
      TK_LOG("Move: in-place turn %.0f deg (node-only).", glm::degrees(dYaw));
    }

    m_walkSM = new StateMachine();
    m_walkSM->PushState(new WalkTurnState(ctx));
    m_walkSM->PushState(new InPlaceTurnDoneState(ctx));
    m_walkSM->m_currentState = m_walkSM->QueryState("WalkTurn");
    m_walkSM->m_currentState->TransitionIn(nullptr);
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

    // An animated player walks to the tile; without animation support it lands
    // instantly. Either way the move is accepted and counts as this turn's
    // single step.
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
    ctx->endReach     = ClipRootTravel(endRec);
    ctx->totalDist    = HorizontalDistance(ctx->startPos, ctx->targetPos);
    ctx->bestRemaining = ctx->totalDist;
    ctx->sinceProgress = 0.0f;
    ctx->arrived       = false;
    ctx->rootNode     = m_root->m_node;

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

    // Fit this move to its target length (default gTurnDuration). The natural
    // duration of the whole move (in-place turn + walk phases) is predicted
    // from the measured clip timings; the FSM timers and the clip playback both
    // run at scale = natural / target so the move finishes in exactly the
    // target seconds (see AnimatedUnit::Frame and the record multipliers).
    float target = (targetDuration > 0.0f) ? targetDuration : gTurnDuration;
    EnsureWalkTimings();
    float naturalDur = EstimateWalkDuration(ctx->turning ? ctx->turnDur : 0.0f,
                                            ctx->totalDist,
                                            m_timingStart,
                                            m_timingLoop,
                                            m_timingEnd);
    float scale = 1.0f;
    if (target > 0.01f && naturalDur > 0.01f && m_timingStart.duration > 0.0f)
    {
      scale = glm::clamp(naturalDur / target, 0.05f, 20.0f);
    }
    m_timeScale = scale;

    // Apply the scale to every clip that can play during the walk (idle may
    // still be fading out at the click; the turn/walk clips follow). The engine
    // multiplies each record's playback -- and its blend countdown -- by the
    // record's own m_timeMultiplier, so transitions stay in sync with the FSM.
    if (m_walkAnim != nullptr)
    {
      static const char* kScaledClips[] = {"idle",
                                           "walk_f_start",
                                           "walk_f",
                                           "walk_f_end",
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
    }

    TK_LOG("Move: natural %.2f s -> %.2f s (x%.2f), turn %s.",
           naturalDur,
           target,
           scale,
           ctx->turning ? "yes" : "no");

    m_walkSM = new StateMachine();
    m_walkSM->PushState(new WalkTurnState(ctx));
    m_walkSM->PushState(new WalkStartState(ctx));
    m_walkSM->PushState(new WalkLoopState(ctx));
    m_walkSM->PushState(new WalkEndState(ctx));
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
    // blend, so the loop and its future fades run at 1x again.
    if (m_walkAnim != nullptr)
    {
      static const char* kScaledClips[] = {"idle",
                                           "walk_f_start",
                                           "walk_f",
                                           "walk_f_end",
                                           "turn_l_90",
                                           "turn_l_180",
                                           "turn_r_90",
                                           "turn_r_180"};
      for (const char* name : kScaledClips)
      {
        if (AnimRecordPtr rec = m_walkAnim->GetAnimRecord(name))
        {
          rec->m_timeMultiplier = 1.0f;
        }
      }
    }
    m_timeScale = 1.0f;

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

    // A patrol that must turn to a heading / idle stare the moment its move
    // lands (a seeker arriving at the last seen tile) plays that turn
    // ANIMATED now, the way the player would, instead of snapping. Falls back
    // to an instant orientation without turn clips.
    if (m_hasDeferredTurn)
    {
      Quaternion target = m_deferredTurn;
      m_hasDeferredTurn = false;
      if (HasAnimatedTurn())
      {
        StartInPlaceTurn(YawOf(target));
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
    m_timingStart = WalkClipTiming();
    m_timingLoop  = WalkClipTiming();
    m_timingEnd   = WalkClipTiming();
    m_timedStartRec = nullptr;
    m_timedLoopRec  = nullptr;
    m_timedEndRec   = nullptr;
    m_hasDeferredTurn = false;

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
    // wall. The guard already faces this way, so the step reads purely as a
    // strike. The shared animated move (or glide fallback) makes the bite
    // visible; the game eats the player when the move lands.
    StartMove(watched, gPatrolGlideTime);
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
    // patrol moving; a missing or blocked one means the line ends, so the
    // patrol turns 180 degrees in place and walks back next turn. Enemies do
    // not block each other, so the tile ahead is only checked for a connection.
    // The step itself is recorded (m_intendedMove) and started by the game, so
    // this patrol moves at the same time as everyone else this turn. The line-
    // end about-face runs the shared ANIMATED in-place turn (turn clips) when
    // the patrol has them, exactly like the player turns.
    GridNode* next = m_grid->Neighbor(*m_node, GetFacingDir());
    if (next != nullptr && m_grid->Connected(*m_node, *next))
    {
      m_intendedMove = next;
      TK_LOG("Linear: line step to (%d, %d).", next->ix, next->iz);
    }
    else
    {
      StartTurn(OppositeDir(GetFacingDir()));
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
