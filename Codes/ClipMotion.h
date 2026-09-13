/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include "GridGraph.h"

#include <Animation.h>

#include <vector>

namespace ToolKit
{
  // Measured motion model of ONE animation clip: how far the clip's root key
  // (the track the .anim file marks with rootKey="...") carries its actor, and
  // when. It is built from the actual animation data -- never hardcoded -- so a
  // clip answers both questions the game asks about it:
  //   * "how long is the action made of you?"   -> duration, TimeToTravel
  //   * "how far do I have to stand from my target so your root motion lands me
  //      on it?"                                -> totalTravel
  // The walk uses it to predict a move's natural length, the execution system
  // uses it to derive the distance a strike clip has to start from.
  struct ClipMotion
  {
    float duration = 0.0f;    // Clip duration (seconds).
    float totalTravel = 0.0f; // Furthest forward root travel reached in the clip.
    std::vector<float> keyTimes;  // Time of each root key (seconds).
    std::vector<float> keyTravel; // Cumulative forward travel at each key.

    // Clip time (seconds) the clip needs to travel the given horizontal
    // distance from its first frame. 0 when the clip has no usable root track.
    float TimeToTravel(float distance) const;

    // True when the clip carries its actor forward at all; a turn-in-place or
    // idle clip has no usable travel and cannot close a gap.
    bool HasTravel() const { return totalTravel > 0.0001f; }
  };

  // Builds the motion model of a clip from its root key track: the time of
  // every reachable root key (frame / fps, clipped to the playable duration)
  // and how far the actor has travelled at that key. Travel is measured along
  // the clip's net horizontal displacement (the direction the clip is authored
  // to move in) and taken as a running maximum, so a curve that settles back
  // slightly at its very end never reduces how far the actor has been.
  ClipMotion MeasureClipMotion(const AnimationPtr& anim);

  // Net horizontal root travel a clip makes when played from its first to its
  // last key. The engine applies per-frame deltas whose sum telescopes to
  // (last - first), so this is the exact distance the clip moves its actor.
  // Zero when the animation has no usable root track.
  float MeasureClipRootTravel(const AnimRecordPtr& record);

  // World-space direction vector a unit faces when heading toward dir.
  Vec3 FacingVector(GridDir d);

  // The grid direction opposite to d (180-degree turn on the grid).
  GridDir OppositeDir(GridDir d);

} // namespace ToolKit
