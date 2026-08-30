/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Unit.h"

#include <MathUtil.h>

#include <cmath>

namespace ToolKit
{
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

  void Player::OnTurn()
  {
    m_hasMoved = false;
  }

  bool Player::TryMove(GridNode* node, const std::function<bool(GridNode*)>& isOccupied)
  {
    if (m_root == nullptr || m_grid == nullptr || m_hasMoved)
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
    const GridDir dirs[4] = {GridDir::Xm, GridDir::Xp, GridDir::Zm, GridDir::Zp};
    for (GridDir dir : dirs)
    {
      if (GridNode* nb = m_grid->Neighbor(*current, dir))
      {
        if (nb == node && m_grid->Connected(*current, *nb))
        {
          PlaceOnNode(nb);
          m_hasMoved = true;
          return true;
        }
      }
    }

    return false;
  }

  void Player::Reset()
  {
    Unit::Reset();
    m_hasMoved = false;
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

  void LinearPatrol::OnTurn()
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

} // namespace ToolKit
