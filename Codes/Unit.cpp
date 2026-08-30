/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Unit.h"

#include <Logger.h>
#include <MathUtil.h>

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

  void Player::OnTurn(GridNode* playerNode, GridDir playerFacing)
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
          StepChase();
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
        StepChase();
        break;

      case State::Investigating:
        // One full turn is spent turning in place to face the heading frozen at
        // the moment the player left the view. Turning and seeing never share a
        // turn -- seeing comes on the following turn (Deciding), exactly like a
        // move takes one turn and a turn takes one turn.
        TK_LOG("Seeker: investigating at (%d, %d); turning to memorized heading %s.",
               m_node->ix,
               m_node->iz,
               GridDirName(m_lastHeading));
        TurnTo(m_lastHeading);
        m_state = State::Deciding;
        break;

      case State::Deciding:
        // Now facing the memorized heading: spotting the player resumes the
        // chase on the spot; an empty view sends the patrol back along its
        // trail.
        if (CanSee(playerNode))
        {
          TK_LOG("Seeker: player seen again at (%d, %d); chase continues.", playerNode->ix, playerNode->iz);
          SpotPlayer(playerNode, playerFacing);
          m_sighted = true;
          m_state = State::Chasing;
          StepChase();
        }
        else
        {
          TK_LOG("Seeker: nobody along %s; returning home.", GridDirName(GetFacingDir()));
          m_state = State::Returning;
          StepReturn();
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
          StepChase();
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

  void SeekerPatrol::StepChase()
  {
    std::vector<GridNode*> path = FindPath(m_lastSeen);
    if (path.size() > 1)
    {
      GridNode* next = path[1];
      PlaceOnNode(next);
      m_trail.push_back(next);
      TK_LOG("Seeker: chase step to (%d, %d), %d tile(s) to go.", next->ix, next->iz, (int) path.size() - 2);
    }

    // Whether it arrived or the target is unreachable, the chase leg ends here:
    // the next turn is spent investigating.
    if (m_node == m_lastSeen)
    {
      TK_LOG("Seeker: arrived at the last seen tile (%d, %d); investigating next turn.", m_node->ix, m_node->iz);
      m_state = State::Investigating;
    }
    else if (path.size() <= 1)
    {
      TK_LOG("Seeker: last seen tile (%d, %d) unreachable; investigating next turn.", m_lastSeen->ix, m_lastSeen->iz);
      m_state = State::Investigating;
    }
  }

  void SeekerPatrol::StepReturn()
  {
    if (m_trail.size() > 1)
    {
      GridNode* back = m_trail[m_trail.size() - 2];
      m_trail.pop_back();
      PlaceOnNode(back);
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
    m_state       = State::Idle;
    m_trail.clear();
  }

} // namespace ToolKit
