/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include <Camera.h>
#include <Entity.h>
#include <MathUtil.h>

namespace ToolKit
{
  // Smooth follow camera: trails a target entity while KEEPING the framing the
  // camera was authored with.
  //
  // A scene camera placed by hand (tagged "master", see Game::SetupMasterCamera)
  // is aimed at the action once and then has to survive the player walking across
  // the grid. This controller does that: it captures the camera -> target offset
  // it starts with -- which IS the authored framing -- and holds that offset while
  // the target moves, converging exponentially instead of snapping. Because the
  // offset comes from the authored placement, the desired position on the first
  // update is exactly where the camera already is, so a session never starts with
  // a jump, and the camera's ROTATION is never touched: the view direction it was
  // placed with is preserved (this is a follow, not an aim).
  //
  // Per frame cost is one vector mix; nothing is allocated and no engine state is
  // held besides the camera node's translation.
  class FollowUpCameraController
  {
   public:
    FollowUpCameraController() = default;

    // Binds the camera to drive and the entity to follow, and captures the
    // current camera -> target offset as the framing to keep. The controller
    // stays inert (IsValid() false) when either side is missing or the camera has
    // no node, so a scene without a usable camera is simply left alone.
    void Init(CameraPtr camera, EntityPtr target);

    // Follows another entity from now on (the offset, i.e. the framing, is kept;
    // the camera glides over to it at the smoothing rate).
    void SetTarget(EntityPtr target);

    // Replaces the captured offset: the world position the camera trails the
    // target by. The camera converges on the new framing smoothly.
    void SetOffset(const Vec3& offset);
    Vec3 GetOffset() const { return m_offset; }

    // Convergence rate (1 / second): how fast the camera closes the remaining gap
    // to the desired position. 0 freezes it where it is, larger is snappier; the
    // default trails a walking character closely without ever snapping. The
    // convergence is frame rate independent (exponential, not a per frame lerp),
    // so the follow looks the same at 30 and at 240 fps.
    void SetSmoothing(float rate);
    float GetSmoothing() const { return m_smoothing; }

    // True when the target's own VERTICAL motion is followed too. False (the
    // default) keeps the height the camera was authored at: the walk cycle's bob
    // and a corpse sinking into the ground must not tilt or drag the view.
    void SetFollowHeight(bool follow);
    bool GetFollowHeight() const { return m_followHeight; }

    // Advances the follow and writes the result into the camera node. deltaTime is
    // the engine frame delta in MILLISECONDS, like every other per frame call in
    // the game code (Game::Frame); a non positive delta is ignored.
    void Update(float deltaTime);

    // True when the controller has a camera with a node and a target to follow.
    bool IsValid() const;

    CameraPtr GetCamera() const { return m_camera; }
    EntityPtr GetTarget() const { return m_target; }

   private:
    // World position of the followed entity (the origin when it has no node).
    Vec3 TargetPosition() const;

    CameraPtr m_camera; // The camera whose node is driven.
    EntityPtr m_target; // The entity being followed.

    // Camera -> target offset captured from the authored placement.
    Vec3 m_offset = Vec3(0.0f);
    // Smoothed camera position: the value actually written into the camera node.
    // Init seeds it with the authored position.
    Vec3 m_position = Vec3(0.0f);

    float m_smoothing    = 4.0f;  // Convergence rate (1 / second).
    bool m_followHeight  = false; // Ride the target's Y instead of the authored one.
  };

} // namespace ToolKit
