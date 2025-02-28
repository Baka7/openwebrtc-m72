// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/paint/cull_rect.h"

#include "third_party/blink/renderer/platform/geometry/float_rect.h"
#include "third_party/blink/renderer/platform/geometry/layout_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/geometry_mapper.h"
#include "third_party/blink/renderer/platform/graphics/paint/scroll_paint_property_node.h"
#include "third_party/blink/renderer/platform/graphics/paint/transform_paint_property_node.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/transforms/affine_transform.h"

namespace blink {

bool CullRect::Intersects(const IntRect& rect) const {
  return IsInfinite() || rect.Intersects(rect_);
}

bool CullRect::Intersects(const LayoutRect& rect) const {
  return IsInfinite() || rect_.Intersects(EnclosingIntRect(rect));
}

bool CullRect::IntersectsTransformed(const AffineTransform& transform,
                                     const FloatRect& rect) const {
  return IsInfinite() || transform.MapRect(rect).Intersects(rect_);
}

bool CullRect::IntersectsHorizontalRange(LayoutUnit lo, LayoutUnit hi) const {
  return !(lo >= rect_.MaxX() || hi <= rect_.X());
}

bool CullRect::IntersectsVerticalRange(LayoutUnit lo, LayoutUnit hi) const {
  return !(lo >= rect_.MaxY() || hi <= rect_.Y());
}

void CullRect::MoveBy(const IntPoint& offset) {
  if (!IsInfinite())
    rect_.MoveBy(offset);
}

void CullRect::Move(const IntSize& offset) {
  if (!IsInfinite())
    rect_.Move(offset);
}

CullRect::ApplyTransformResult CullRect::ApplyTransformInternal(
    const TransformPaintPropertyNode* transform) {
  if (IsInfinite())
    return kNotExpanded;

  if (RuntimeEnabledFeatures::SlimmingPaintV2Enabled()) {
    if (const auto* scroll = transform->ScrollNode()) {
      rect_.Intersect(scroll->ContainerRect());
      if (rect_.IsEmpty())
        return kNotExpanded;
      rect_ = transform->Matrix().Inverse().MapRect(rect_);

      // Expand the cull rect for scrolling contents in case of composited
      // scrolling.
      // TODO(wangxianzhu): the expansion distance needs to be scaled to
      // screen pixels.
      // TODO(wangxianzhu): options for non-composited-scrolling contents:
      // 1. to use non-composted-scrolling heuristics to avoid expansion;
      // 2. to reduce the 4000px distance, no matter if the contents with be
      //    composited scrolling.
      // 3. mixed method of 1 and 2, e.g. the distance could be a function of
      //    confidence that the contents will be composited scrolling.
      static const int kPixelDistanceToExpand = 4000;
      rect_.Inflate(kPixelDistanceToExpand);
      // Don't clip the cull rect by contents size to let ChangedEnough() work
      // even if the new cull rect exceeds the bounds of contents rect.
      return rect_.Contains(IntRect(IntPoint(), scroll->ContentsSize()))
                 ? kExpandedForWholeScrollingContents
                 : kExpandedForPartialScrollingContents;
    }
  }

  rect_ = transform->Matrix().Inverse().MapRect(rect_);
  return kNotExpanded;
}

void CullRect::ApplyTransforms(const TransformPaintPropertyNode* from,
                               const TransformPaintPropertyNode* to,
                               const base::Optional<CullRect>& old_cull_rect) {
  DCHECK(RuntimeEnabledFeatures::SlimmingPaintV2Enabled());

  Vector<const TransformPaintPropertyNode*> scroll_translations;
  for (const auto* t = to; t != from; t = t->Parent()) {
    if (!t) {
      // |from| is not an ancestor of |to|. Simply map.
      GeometryMapper::SourceToDestinationRect(from, to, rect_);
      return;
    }
    if (t->ScrollNode())
      scroll_translations.push_back(t);
  }

  const auto* last_transform = from;
  ApplyTransformResult last_scroll_translation_result = kNotExpanded;
  for (auto it = scroll_translations.rbegin(); it != scroll_translations.rend();
       ++it) {
    const auto* scroll_translation = *it;
    GeometryMapper::SourceToDestinationRect(
        last_transform, scroll_translation->Parent(), rect_);
    last_scroll_translation_result = ApplyTransformInternal(scroll_translation);
    last_transform = scroll_translation;
  }
  GeometryMapper::SourceToDestinationRect(last_transform, to, rect_);

  if (last_scroll_translation_result == kExpandedForPartialScrollingContents &&
      old_cull_rect && !ChangedEnough(*old_cull_rect))
    rect_ = old_cull_rect->Rect();
}

bool CullRect::ChangedEnough(const CullRect& old_cull_rect) const {
  DCHECK(RuntimeEnabledFeatures::SlimmingPaintV2Enabled());

  const auto& new_rect = Rect();
  const auto& old_rect = old_cull_rect.Rect();
  if (old_rect == new_rect || (old_rect.IsEmpty() && new_rect.IsEmpty()))
    return false;

  if (old_rect.IsEmpty())
    return true;

  auto expanded_old_rect = old_rect;
  static const int kChangedEnoughMinimumDistance = 512;
  expanded_old_rect.Inflate(kChangedEnoughMinimumDistance);
  return !expanded_old_rect.Contains(new_rect);
}

}  // namespace blink
