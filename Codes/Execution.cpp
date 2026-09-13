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

    // The catalogue. A relation with no variants authored (front and the two
    // shoulders, so far) resolves to null and its strike keeps the plain bite;
    // authoring front_*/fronted_* clips (for instance) is one row here.
    struct RelationEntry
    {
      ExecRelation rel;
      const ExecutionVariant* variants;
      int count;
    };

    const RelationEntry kCatalogue[] = {
      {ExecRelation::Behind, kAmbush, static_cast<int>(sizeof(kAmbush) / sizeof(kAmbush[0]))},
      {ExecRelation::Front, nullptr, 0},
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

    // Animation resource each relation's clip was measured from, so a clip is
    // measured once per loaded resource instead of once per execution.
    const Animation*& MeasuredSource(ExecRelation rel)
    {
      static const Animation* sources[4] = {nullptr, nullptr, nullptr, nullptr};
      return sources[RelationIndex(rel)];
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

    int& cycle              = VariantCycle(rel);
    const ExecutionVariant& variant = entry->variants[cycle % entry->count];
    cycle                   = (cycle + 1) % entry->count;

    ExecutionClip& clip = ResolvedSlot(rel);
    clip.attackerSignal = variant.attacker;
    clip.victimSignal   = variant.victim;

    AnimRecordPtr rec = attackerAnim->GetAnimRecord(clip.attackerSignal);
    AnimationPtr anim = (rec != nullptr) ? rec->m_animation : nullptr;
    if (anim == nullptr)
    {
      clip.valid = false;
      return nullptr;
    }

    // Measure the clip once per loaded resource: the profile is pure animation
    // data, so it is read at init / on reload and never guessed at runtime.
    if (MeasuredSource(rel) != anim.get())
    {
      clip.attackerMotion = MeasureClipMotion(anim);
      MeasuredSource(rel) = anim.get();
      TK_LOG("Exec: measured '%s' for a strike from %s: %.2f s, %.3f u of forward travel.",
             clip.attackerSignal.c_str(),
             ExecRelationName(rel),
             clip.attackerMotion.duration,
             clip.attackerMotion.totalTravel);
    }

    clip.valid = clip.attackerMotion.HasTravel();
    return clip.valid ? &clip : nullptr;
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
      VariantCycle(entry.rel)   = 0;
      MeasuredSource(entry.rel) = nullptr;
      ResolvedSlot(entry.rel)   = ExecutionClip();
    }
  }

} // namespace ToolKit
