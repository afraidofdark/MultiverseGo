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

    // Follows a POINT instead of the target entity's world position, and captures the
    // framing against it. The game feeds the TILE the followed unit stands on, and that is
    // the whole point: the animated actor moves with root motion, so a camera riding it
    // inherits every wobble the animation has -- a turn in flight, the landing dip, the
    // blend back into the stride. A tile is a fixed point a unit only ever jumps BETWEEN,
    // so the follow glides from tile to tile and a fast transit reads as the camera
    // hopping the grid with the character. The entity target stays as the fallback until
    // this is called, and the point may be re-set every frame (the game does).
    void SetFollowPoint(const Vec3& point, bool captureFraming = false);
    Vec3 GetFollowPoint() const { return m_followPoint; }
    bool HasFollowPoint() const { return m_hasFollowPoint; }

    // Replaces the captured offset: the world position the camera trails the
    // target by. The camera converges on the new framing smoothly.
    void SetOffset(const Vec3& offset);
    Vec3 GetOffset() const { return m_offset; }

    // DAMPING: roughly how long (seconds) the camera takes to close the gap to where the
    // framing wants it. It is a CRITICALLY DAMPED SPRING, not a per frame mix: the camera
    // starts from its own velocity, so it accelerates into a move (it stays BEHIND while
    // the move starts) and decays into the stop when the move ends (it slows down onto the
    // framing instead of arriving at full speed), and it never overshoots. A larger value
    // means a heavier, lazier camera. The follow is frame rate independent: the spring is
    // solved per call, so a 30 fps and a 240 fps session behave the same.
    void SetSmoothTime(float seconds);
    float GetSmoothTime() const { return m_smoothTime; }

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
    EntityPtr m_target; // The entity being followed (fallback for the follow point).

    // The TILE (or any fixed point) the camera actually rides while HasFollowPoint:
    // set by the game from the followed unit's current grid node.
    Vec3 m_followPoint    = Vec3(0.0f);
    bool m_hasFollowPoint = false;

    // Camera -> target offset captured from the authored placement.
    Vec3 m_offset = Vec3(0.0f);
    // Smoothed camera position: the value actually written into the camera node.
    // Init seeds it with the authored position.
    Vec3 m_position = Vec3(0.0f);
    // Velocity of the damping spring (world units / second). Carried between frames so the
    // camera has its own momentum: it eases in when a move starts and eases out when it
    // ends, which is what a camera on rails does and a per frame mix never does.
    Vec3 m_velocity = Vec3(0.0f);

    float m_smoothTime   = 0.5f;  // Gap closing time of the damping (seconds).
    bool m_followHeight  = false; // Ride the target's Y instead of the authored one.
  };

} // namespace ToolKit
