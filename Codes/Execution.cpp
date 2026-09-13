/*
 * Copyright (c) 2019-2026 OtSoftware
 * This code is licensed under the GNU Lesser General Public License v3.0 (LGPL-3.0).
 * For more information, including options for a more permissive commercial license,
 * please visit [otsoftware.tr] or contact us at [info@otsoftare.tr].
 */

#include "Execution.h"

#include <AnimationControllerComponent.h>
#include <Logger.h>
#include <MathUtil.h>

#include <map>

namespace ToolKit
{
  namespace
  {
    // One authored variant: the pair of clips that make a single execution
    // scene, the attacker's strike and the victim's reaction.
    struct ExecutionVariant
    {
      const char* attacker;
      const char* victim;
    };

    // Behind: the ambush series. The attacker walks up the victim's back and
    // the clip carries it forward into the victim.
    const ExecutionVariant kAmbush[] = {
      {"ambush_1", "ambushed_1"},
      {"ambush_2", "ambushed_2"},
      {"ambush_3", "ambushed_3"},
    };

    // Front: the execution series, authored as thirteen paired scenes -- the
    // attacker plays execution_N while its victim plays executed_N. These are
    // the head on kills: the attacker walks into its prey and the clip covers
    // the last stretch, however far the two are authored to travel.
    const ExecutionVariant kExecution[] = {
      {"execution_1", "executed_1"},
      {"execution_2", "executed_2"},
      {"execution_3", "executed_3"},
      {"execution_4", "executed_4"},
      {"execution_5", "executed_5"},
      {"execution_6", "executed_6"},
      {"execution_7", "executed_7"},
      {"execution_8", "executed_8"},
      {"execution_9", "executed_9"},
      {"execution_10", "executed_10"},
      {"execution_11", "executed_11"},
      {"execution_12", "executed_12"},
      {"execution_13", "executed_13"},
    };

    // The catalogue. A relation with no variants authored (the two shoulders,
    // so far) resolves to null and its strike keeps the plain step; authoring
    // clips for one is a row here.
    struct RelationEntry
    {
      ExecRelation rel;
      const ExecutionVariant* variants;
      int count;
    };

    const RelationEntry kCatalogue[] = {
      {ExecRelation::Behind, kAmbush, static_cast<int>(sizeof(kAmbush) / sizeof(kAmbush[0]))},
      {ExecRelation::Front, kExecution, static_cast<int>(sizeof(kExecution) / sizeof(kExecution[0]))},
      {ExecRelation::Left, nullptr, 0},
      {ExecRelation::Right, nullptr, 0},
    };

    int RelationIndex(ExecRelation rel)
    {
      switch (rel)
      {
        case ExecRelation::Behind: return 0;
        case ExecRelation::Front: return 1;
        case ExecRelation::Left: return 2;
        default: return 3;
      }
    }

    const RelationEntry* CatalogueEntry(ExecRelation rel)
    {
      return &kCatalogue[RelationIndex(rel)];
    }

    // Resolved clip of each relation, reused between calls (the caller only
    // reads it, and no two executions resolve at the same time).
    ExecutionClip& ResolvedSlot(ExecRelation rel)
    {
      static ExecutionClip slots[4];
      return slots[RelationIndex(rel)];
    }

    // Measured motion of every clip the catalogue has resolved, keyed by the
    // loaded animation resource: each clip is measured once and then reused,
    // whichever relation and whichever character asked for it first.
    std::map<const Animation*, ClipMotion>& MotionCache()
    {
      static std::map<const Animation*, ClipMotion> cache;
      return cache;
    }

    // Which variant of a relation is due next: the variants take turns, so a
    // player eaten three times sees all three scenes instead of the same one.
    int& VariantCycle(ExecRelation rel)
    {
      static int cycles[4] = {0, 0, 0, 0};
      return cycles[RelationIndex(rel)];
    }
  } // namespace

  const char* ExecRelationName(ExecRelation rel)
  {
    switch (rel)
    {
      case ExecRelation::Behind: return "behind";
      case ExecRelation::Front: return "front";
      case ExecRelation::Left: return "left";
      default: return "right";
    }
  }

  ExecRelation ExecutionLibrary::RelationOf(GridDir approach, GridDir victimFacing)
  {
    // Walking the way the victim faces means coming up its back; walking into
    // it means the two meet face to face.
    if (approach == victimFacing)
    {
      return ExecRelation::Behind;
    }
    if (approach == OppositeDir(victimFacing))
    {
      return ExecRelation::Front;
    }

    // The two remaining directions are the victim's shoulders. Its right is its
    // facing turned toward (+X x +Y), the character's own right in this
    // right-handed, Y-up frame.
    Vec3 right = glm::normalize(glm::cross(FacingVector(victimFacing), Vec3(0.0f, 1.0f, 0.0f)));
    float side = glm::dot(FacingVector(approach), right);
    return (side > 0.0f) ? ExecRelation::Right : ExecRelation::Left;
  }

  const ExecutionClip* ExecutionLibrary::Resolve(ExecRelation rel,
                                                 AnimControllerComponent* attackerAnim)
  {
    const RelationEntry* entry = CatalogueEntry(rel);
    if (entry == nullptr || entry->count <= 0 || entry->variants == nullptr ||
        attackerAnim == nullptr)
    {
      return nullptr;
    }

    // The variants take turns, so repeated kills do not replay the same scene.
    // A variant that cannot be played on this character -- its record is not
    // authored on the prefab, or the clip carries no root travel -- must not
    // cost the whole strike: step on through the series instead, so a partly
    // authored set still performs the scenes it has.
    int& cycle = VariantCycle(rel);
    for (int attempt = 0; attempt < entry->count; attempt++)
    {
      const ExecutionVariant& variant = entry->variants[cycle % entry->count];
      cycle                           = (cycle + 1) % entry->count;

      ExecutionClip& clip = ResolvedSlot(rel);
      clip.attackerSignal = variant.attacker;
      clip.victimSignal   = variant.victim;
      clip.valid          = false;

      AnimRecordPtr rec  = attackerAnim->GetAnimRecord(clip.attackerSignal);
      AnimationPtr anim  = (rec != nullptr) ? rec->m_animation : nullptr;
      if (anim == nullptr)
      {
        TK_LOG("Exec: '%s' is not on this character; trying the next %s variant.",
               clip.attackerSignal.c_str(),
               ExecRelationName(rel));
        continue;
      }

      // Measure the clip once per loaded resource: the profile is pure
      // animation data, so it is read on its first use and never guessed.
      ClipMotion& motion = MotionCache()[anim.get()];
      if (motion.duration <= 0.0f)
      {
        motion = MeasureClipMotion(anim);
        TK_LOG("Exec: measured '%s' for a strike from %s: %.2f s, %.3f u of forward travel.",
               clip.attackerSignal.c_str(),
               ExecRelationName(rel),
               motion.duration,
               motion.totalTravel);
      }

      if (!motion.HasTravel())
      {
        TK_LOG("Exec: '%s' carries no root travel; trying the next %s variant.",
               clip.attackerSignal.c_str(),
               ExecRelationName(rel));
        continue;
      }

      clip.attackerMotion = motion;
      clip.valid          = true;
      return &clip;
    }

    TK_LOG("Exec: none of the %d %s variants is playable here; plain step.",
           entry->count,
           ExecRelationName(rel));
    return nullptr;
  }

  int ExecutionLibrary::VariantCount(ExecRelation rel)
  {
    const RelationEntry* entry = CatalogueEntry(rel);
    return (entry != nullptr) ? entry->count : 0;
  }

  void ExecutionLibrary::Reset()
  {
    for (const RelationEntry& entry : kCatalogue)
    {
      VariantCycle(entry.rel) = 0;
      ResolvedSlot(entry.rel) = ExecutionClip();
    }
    MotionCache().clear();
  }

} // namespace ToolKit
