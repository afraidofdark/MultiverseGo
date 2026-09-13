/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "ClipMotion.h"

#include <MathUtil.h>

namespace ToolKit
{
  float ClipMotion::TimeToTravel(float distance) const
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

  ClipMotion MeasureClipMotion(const AnimationPtr& anim)
  {
    ClipMotion t;
    if (anim == nullptr)
    {
      return t;
    }

    t.duration          = anim->m_duration;
    const KeyArray* keys = anim->m_keys.Find(anim->m_rootKey);
    if (keys == nullptr || keys->size() < 2)
    {
      return t;
    }

    const float fps   = (anim->m_fps > 0.0f) ? anim->m_fps : 30.0f;
    const Vec3 first  = keys->front().m_position;
    const Vec3 last   = keys->back().m_position;

    // Travel axis: the net horizontal displacement of the root curve. The
    // actor's progress toward a straight-ahead target is the running max of the
    // signed projection of its position onto this axis.
    Vec3 axis(last.x - first.x, 0.0f, last.z - first.z);
    float axisLen = glm::length(axis);
    if (axisLen < 0.0001f)
    {
      return t; // No net horizontal travel (turn / idle style clip).
    }
    axis /= axisLen;

    float runMax = 0.0f;
    size_t n     = keys->size();
    size_t i     = 0;
    for (; i < n; i++)
    {
      const Key& k = (*keys)[i];
      float time   = k.m_frame / fps;
      if (time > t.duration + 0.0001f)
      {
        break; // Past the playable end; the engine clamps at m_duration.
      }
      Vec3 d  = k.m_position - first;
      runMax  = glm::max(runMax, d.x * axis.x + d.z * axis.z);
      t.keyTimes.push_back(time);
      t.keyTravel.push_back(runMax);
    }

    // A key that crosses the playable end: interpolate the travel reached at
    // exactly m_duration and append it as the final sample.
    if (i < n && !t.keyTimes.empty() && i > 0)
    {
      const Key& kNext = (*keys)[i];
      const Key& kPrev = (*keys)[i - 1];
      float tNext      = kNext.m_frame / fps;
      float tPrev      = kPrev.m_frame / fps;
      if (tNext > tPrev)
      {
        float r  = (t.duration - tPrev) / (tNext - tPrev);
        Vec3 pos = kPrev.m_position + (kNext.m_position - kPrev.m_position) * r;
        Vec3 d   = pos - first;
        runMax   = glm::max(runMax, d.x * axis.x + d.z * axis.z);
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

  float MeasureClipRootTravel(const AnimRecordPtr& record)
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

} // namespace ToolKit
