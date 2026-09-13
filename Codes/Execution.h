/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#pragma once

#include "ClipMotion.h"

namespace ToolKit
{
  // Forward declaration: resolving an execution only needs the attacker's
  // animation controller, to find and measure the clip it plays.
  class AnimControllerComponent;

  // Which side of its victim an execution comes from: the direction the
  // attacker walks in measured against the way the victim faces at that
  // moment. This relation -- not the attacker's type -- selects the animation,
  // because the clips are authored per side (a strike from the back is a
  // different scene from one from the front).
  enum class ExecRelation
  {
    Behind, // The attacker walks the way the victim faces: it comes from the back.
    Front,  // Attacker and victim walk into each other, face to face.
    Left,   // The attacker comes in over the victim's left shoulder.
    Right   // ... over its right shoulder.
  };

  // Readable relation name (logs).
  const char* ExecRelationName(ExecRelation rel);

  // One authored execution: the clip the attacker plays (ambush_N), the clip
  // its victim plays (ambushed_N) and the MEASURED motion of the attacker's
  // clip. The measurement is the whole point: it says how far the strike clip
  // carries its actor, which is exactly how far from the victim the strike has
  // to start for the root motion to land the attacker on it. No distance is
  // ever hardcoded -- author a new clip and its start distance follows from the
  // clip itself.
  struct ExecutionClip
  {
    String attackerSignal;     // Signal on the attacker's animation controller.
    String victimSignal;       // Signal on the victim's animation controller.
    ClipMotion attackerMotion; // Measured length and forward reach of the strike.
    bool valid = false;        // Measured and usable (clip loaded, has root travel).

    // Distance (world units) the attacker must be from the victim when the
    // strike starts: the clip's own forward reach.
    float StartDistance() const { return attackerMotion.totalTravel; }

    // Natural length of the strike clip (seconds).
    float AttackDuration() const { return attackerMotion.duration; }
  };

  // The catalogue of executions, keyed by relation and driven by the animation
  // assets: which clips a relation uses and how far each of them starts from.
  // Adding a direction is a table entry, never code -- and a relation with no
  // clips authored simply reports none, so the caller keeps its plain bite.
  class ExecutionLibrary
  {
   public:
    // Relation of an execution whose attacker walks in along `approach` toward
    // a victim facing `victimFacing` (both snapped to grid axes).
    static ExecRelation RelationOf(GridDir approach, GridDir victimFacing);

    // Resolves the execution authored for the relation, cycling through the
    // variant clips (ambush_1/2/3) so repeated kills do not replay the same
    // scene, and measuring the chosen clip against the attacker's controller.
    // Returns null when the relation has no clip authored, the clip is missing
    // or it carries no root travel.
    static const ExecutionClip* Resolve(ExecRelation rel, AnimControllerComponent* attackerAnim);

    // Number of authored variants of a relation (0 when it has none).
    static int VariantCount(ExecRelation rel);

    // Drops the measurement cache and the variant cycle. Called when a play
    // session starts, so clips are measured again for the fresh resources.
    static void Reset();
  };

} // namespace ToolKit
