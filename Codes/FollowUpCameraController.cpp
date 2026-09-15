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

  void FollowUpCameraController::SetOffset(const Vec3& offset)
  {
    m_offset = offset;
  }

  void FollowUpCameraController::SetSmoothing(float rate)
  {
    m_smoothing = (rate > 0.0f) ? rate : 0.0f;
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

    Vec3 desired = TargetPosition() + m_offset;
    if (!m_followHeight)
    {
      // Keep the authored height: the target's own vertical motion (the walk
      // cycle's bob, a body sinking into the ground) must not move the view.
      desired.y = m_position.y;
    }

    // Frame rate independent exponential convergence: the same fraction of the
    // remaining gap is closed per second whatever the frame time is. A huge delta
    // (a hitch) converges almost fully instead of overshooting.
    const float k = 1.0f - std::exp(-m_smoothing * dt);
    m_position    = glm::mix(m_position, desired, k);

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
