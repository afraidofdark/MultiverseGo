/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "FollowUpCameraController.h"

#include <cmath>

namespace ToolKit
{
  void FollowUpCameraController::Init(CameraPtr camera, EntityPtr target)
  {
    m_camera = camera;
    m_target = target;

    if (!IsValid())
    {
      // Nothing to drive or nothing to follow: the controller stays inert and the
      // caller keeps whatever camera it had.
      m_offset   = Vec3(0.0f);
      m_position = Vec3(0.0f);
      return;
    }

    // The framing to keep is the one the camera was AUTHORED with, expressed as
    // the offset from the target it starts next to. The first Update therefore
    // wants the camera exactly where it already is: a session starts with no
    // movement at all, and the authored look is preserved.
    const Vec3 cameraPos =
        m_camera->m_node->GetTranslation(TransformationSpace::TS_WORLD);

    m_position = cameraPos;
    m_offset   = cameraPos - TargetPosition();
  }

  void FollowUpCameraController::SetTarget(EntityPtr target) { m_target = target; }

  void FollowUpCameraController::SetFollowPoint(const Vec3& point, bool captureFraming)
  {
    m_followPoint    = point;
    m_hasFollowPoint = true;

    if (captureFraming && IsValid())
    {
      // Re-capture the framing against the point the camera is going to ride, so the
      // authored view is preserved exactly (the first update then wants the camera where
      // it already is).
      const Vec3 cameraPos =
          m_camera->m_node->GetTranslation(TransformationSpace::TS_WORLD);
      m_position = cameraPos;
      m_offset   = cameraPos - point;
    }
  }

  void FollowUpCameraController::SetOffset(const Vec3& offset)
  {
    m_offset = offset;
  }

  void FollowUpCameraController::SetSmoothTime(float seconds)
  {
    m_smoothTime = (seconds > 0.001f) ? seconds : 0.001f;
  }

  void FollowUpCameraController::SetFollowHeight(bool follow)
  {
    m_followHeight = follow;
  }

  void FollowUpCameraController::Update(float deltaTime)
  {
    if (!IsValid())
    {
      return;
    }

    // Frame deltas arrive in milliseconds; the convergence rate is per second.
    const float dt = deltaTime * 0.001f;
    if (dt <= 0.0f)
    {
      return;
    }

    // What the camera rides: the follow POINT when the game gave one (the TILE the
    // followed unit stands on), otherwise the target entity's own world position.
    Vec3 desired = (m_hasFollowPoint ? m_followPoint : TargetPosition()) + m_offset;
    if (!m_followHeight)
    {
      // Keep the authored height: the target's own vertical motion (the walk
      // cycle's bob, a body sinking into the ground) must not move the view.
      desired.y = m_position.y;
    }

    // CRITICALLY DAMPED SPRING, solved for this frame (the classic SmoothDamp): the camera
    // has a VELOCITY of its own, so a move that starts does not yank it -- it accelerates
    // into the move and therefore trails BEHIND while the character gets going -- and a
    // move that ends does not stop it dead: the velocity decays and it eases onto the
    // framing. It is solved per call, so any frame rate behaves the same, and a target
    // that jumps past the camera is never overshot (result == desired).
    const float omega = 2.0f / m_smoothTime;
    const float x     = omega * dt;
    const float expo  = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    const Vec3 change = m_position - desired;
    const Vec3 temp   = (m_velocity + omega * change) * dt;
    m_velocity        = (m_velocity - omega * temp) * expo;

    Vec3 result = desired + (change + temp) * expo;
    if (glm::dot(desired - m_position, result - desired) > 0.0f)
    {
      result     = desired; // Never pass the target: a snap would read as a bounce.
      m_velocity = Vec3(0.0f);
    }

    m_position = result;
    m_camera->m_node->SetTranslation(m_position, TransformationSpace::TS_WORLD);
  }

  bool FollowUpCameraController::IsValid() const
  {
    return m_camera != nullptr && m_camera->m_node != nullptr && m_target != nullptr;
  }

  Vec3 FollowUpCameraController::TargetPosition() const
  {
    if (m_target == nullptr || m_target->m_node == nullptr)
    {
      return Vec3(0.0f);
    }

    return m_target->m_node->GetTranslation(TransformationSpace::TS_WORLD);
  }

} // namespace ToolKit
