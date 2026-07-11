# Starlight Linear Layout Module Level 1

## Abstract

This specification defines Starlight linear layout, a single-axis layout model
for arranging in-flow children along a main axis and aligning them along a
perpendicular cross axis.

Linear layout is similar in shape to a single-line flex formatting context:
it has main and cross axes, supports main-axis packing, cross-axis alignment,
positive weighted main-size distribution, and absolute-position static
position alignment. It does not define flex wrapping, flex shrink/grow, or line
packing.

## Status of this Document

This document is a Starlight implementation specification. It is not a W3C
Recommendation and does not define web-facing CSS syntax by itself.

This document only describes the current non-deprecated behavior. It excludes:

- full-quirks linear sizing behavior;
- target-SDK compatibility paths that predate flex-style mappings in linear
  layout;
- the old cross-axis auto-margin behavior that used the border area instead of
  the content area;
- legacy crash/error behavior for baseline alignment;
- parser-level syntax handling and shorthand expansion.

The layout engine consumes style resolver output. Invalid syntax is not a
layout repair case. If a syntactically valid style value is ignored, mapped, or
falls back in linear layout, that is part of this layout algorithm.

## References

This module follows the document organization and terminology style of:

- CSS Flexible Box Layout Module Level 1:
  https://www.w3.org/TR/css-flexbox-1/
- CSS Box Alignment Module Level 3:
  https://www.w3.org/TR/css-align-3/
- CSS Display Module Level 3:
  https://www.w3.org/TR/css-display-3/

## Module Interactions

Linear layout participates in the Starlight layout tree after style resolution.
It uses common Starlight sizing, edges, min/max, aspect-ratio, measurement,
rounding, positioning, and out-of-flow layout utilities.

`display: block` may be converted to the same effective layout behavior as
linear layout by the engine. This document specifies the linear formatting
context itself; block-as-linear dispatch is a caller decision.

## Terminology

linear container

: A box whose effective display type is `linear`.

linear item

: An in-flow direct child of a linear container. Children with
  `display: none` and out-of-flow positioned children are not linear items.
  Children with `visibility: hidden` or `visibility: collapse` remain linear
  items unless they also have `display: none`.

main axis

: The primary axis along which linear items are laid out. The main axis is
  horizontal for `row`, `row-reverse`, `horizontal`, and `horizontal-reverse`;
  vertical for `column`, `column-reverse`, `vertical`, and
  `vertical-reverse`.

cross axis

: The axis perpendicular to the main axis.

main-start and main-end

: The logical sides of the main axis. A reverse orientation swaps the logical
  main-start and main-end sides. For horizontal main axes, RTL direction also
  reverses the main axis.

cross-start and cross-end

: The logical sides of the cross axis. For vertical linear containers, RTL
  direction reverses the horizontal cross axis.

outer main size

: The used border-box main size plus used margins in the main axis.

outer cross size

: The used border-box cross size plus used margins in the cross axis.

weighted item

: A linear item whose `linear_weight` is positive.

## Value Definitions

The following value names describe computed style data, not raw CSS parser
tokens.

Initial values in this section document the current Rust standalone style
surface. If another Starlight bridge materializes different compatibility
defaults before layout, the bridge owns that defaulting decision; this
document defines layout behavior for the computed values that reach the
algorithm.

### `display: linear`

Name: `display`

Value: `linear`

Applies to: layout containers

Effect: establishes a linear formatting context for the container's in-flow
children.

### `linear-orientation`

Name: `linear-orientation`

Value: `row | row-reverse | column | column-reverse | horizontal |
horizontal-reverse | vertical | vertical-reverse`

Initial: `vertical`

Applies to: linear containers

The `row` and `horizontal` values establish a horizontal main axis. The
`column` and `vertical` values establish a vertical main axis. The `*-reverse`
values reverse the main axis before final physical offset export. The
`horizontal*` and `vertical*` values are compatibility aliases for the
corresponding row/column directions; they are still accepted by the current
public API and are therefore part of this non-deprecated layout surface.

### `linear-gravity`

Name: `linear-gravity`

Value: `none | start | end | center | space-between | left | right | top |
bottom | center-horizontal | center-vertical`

Initial: `none`

Applies to: linear containers

This property packs linear items along the main axis. If it is `none`,
`justify-content` is mapped into linear gravity as defined in
"Main-Axis Alignment".

### `linear-cross-gravity`

Name: `linear-cross-gravity`

Value: `none | start | end | center | stretch`

Initial: `none`

Applies to: linear containers

This property provides the container-level fallback for cross-axis alignment
when an item does not specify `linear-layout-gravity` and no applicable
`align-self` value supplies one.

### `linear-layout-gravity`

Name: `linear-layout-gravity`

Value: `none | start | end | center | stretch | left | right | top | bottom |
center-horizontal | center-vertical | fill-horizontal | fill-vertical`

Initial: `none`

Applies to: linear items

This property aligns an individual item in the cross axis. The `fill-*` and
`stretch` values force a definite cross-axis constraint when the container's
cross-axis content size is definite.

### `linear-weight`

Name: `linear-weight`

Value: non-negative number

Initial: `0`

Applies to: linear items

Positive values request a share of the definite remaining main-axis content
space. Non-positive values do not participate in weighted distribution.

### `linear-weight-sum`

Name: `linear-weight-sum`

Value: non-negative number

Initial: `0`

Applies to: linear containers

If positive, this value overrides the effective total weight used to scale
free-space distribution. It does not make non-positive `linear-weight` items
participate.

## Linear Items

The children of a linear container are processed as follows:

1. Direct children with `display: none` are laid out as zero-sized hidden
   subtrees and are not linear items.
2. Direct children with `position: absolute` are not linear items. They are
   laid out after the in-flow container size is known.
3. Fixed descendants are handled by the common fixed-position pass and are not
   linear items.
4. Remaining direct children are linear items.
5. If any linear item has a non-zero `order`, items are sorted by `order`
   before layout. Otherwise source order is preserved. Sorting is stable with
   respect to equal order values.

## Layout Algorithm

This section defines the linear layout algorithm.

### 1. Initial Setup

1. Resolve the container's padding, border, and margin against the available
   parent constraints.
2. Resolve the container's definite width and height, including
   `box-sizing`, min/max constraints, and aspect ratio.
3. Establish the main and cross axes from `linear-orientation`.
4. Determine whether the main or cross axis has a definite content size from
   resolved container size or definite parent constraints.
5. Lay out `display: none` direct children as hidden zero-sized subtrees.
6. Collect and order linear items as defined in "Linear Items".

### 2. Item Measurement

For each linear item:

1. Resolve item edges against the container width when that width is available.
2. Determine the item main-axis size from its main-size property if that value
   resolves to a definite border-box size.
3. Determine the item cross-axis constraint:
   1. Start from the container cross-axis content constraint.
   2. If the item cross-size is `fit-content()`, use the fit-content owner
      constraint.
4. If the item has a definite main size, use that as its main-axis constraint.
   Otherwise the main-axis constraint is indefinite unless the child sizing
   rules provide a definite value.
5. If the cross-axis constraint is definite and either:
   - the computed linear layout gravity is `stretch`, `fill-horizontal`, or
     `fill-vertical`; or
   - the computed linear layout gravity is `none`, the item cross-size is
     `auto`, and the cross-size is not intrinsic;
   then use the available cross-axis content size minus cross-axis margins as a
   definite cross-axis constraint.
6. Lay out non-weighted items with the resulting constraints. A weighted item
   may be initially represented with zero main size until weighted
   distribution is resolved.

### 3. Weighted Main-Size Resolution

If the container has no definite content main size, weighted distribution does
not run.

If it has a definite content main size:

1. Collect items whose `linear_weight` is positive.
2. Treat each collected item's main size as zero for the purpose of initial
   free-space calculation.
3. Compute the initial free space as the content main size minus all
   non-frozen item outer main sizes.
4. Let the active weight be the sum of positive weights among unfrozen
   weighted items.
5. If `linear-weight-sum` is positive, scale the distributable space by
   `sum(item weights) / linear-weight-sum`; otherwise distribute according to
   active weight.
6. Assign each unfrozen weighted item a tentative main size proportional to
   its weight.
7. Clamp each tentative size by the item's min/max main-size rules.
8. If clamping caused min or max violations, freeze the violating items and
   repeat from step 4.
9. Re-layout each weighted item with its resolved main size.

### 4. Container Size Determination

1. The natural content main size is the definite content main size if one
   exists; otherwise it is the sum of all item outer main sizes.
2. The natural content cross size is the definite content cross size if one
   exists; otherwise it is the maximum item outer cross size.
3. Add padding and border to produce the border-box size.
4. Apply min/max constraints.
5. If a final in-flow child layout changes the summed main size while the
   container main size is content-sized, recompute the container main size from
   the final total.

### 5. Main-Axis Alignment

Let free space be the final content main size minus the sum of item outer main
sizes.

1. Compute the logical main gravity:
   1. If `linear-gravity` is not `none`, use it.
   2. Otherwise map `justify-content` as follows:
      - `flex-end` and `end` -> `end`;
      - `center` -> `center`;
      - `space-between` -> `space-between`;
      - `stretch`, `flex-start`, `start`, `space-around`, and
        `space-evenly` -> `start`.
   3. Convert physical `left`, `right`, `top`, and `bottom` values to
      logical `start` or `end` using the main axis and reversal state.
2. If gravity is `end`, the first item starts at the free space.
3. If gravity is `center`, the first item starts at half the free space.
4. If gravity is `space-between` and there is more than one item, the first
   item starts at zero and the item gap is `max(free space, 0) / (item count -
   1)`.
5. Otherwise the first item starts at zero and the item gap is zero.
6. Each item is placed at the current main cursor, adjusted for main-axis
   margins and physical reversal. The cursor advances by the item's final outer
   main size plus the item gap.

Negative free space is allowed for `end` and `center` alignment. Negative free
space does not create negative `space-between` gaps.

### 6. Cross-Axis Alignment

For each item:

1. Compute item linear layout gravity:
   1. Start with the item's `linear-layout-gravity`.
   2. If it is `none` and `align-self` is present, map `align-self`:
      - `stretch` -> `stretch`;
      - `flex-start` and `start` -> `start`;
      - `center` -> `center`;
      - `flex-end` and `end` -> `end`;
      - `baseline` -> `none`.
   3. If it is still `none`, map container `linear-cross-gravity` to the same
      `start`/`end`/`center`/`stretch` values.
   4. If it is still `none` and container `align-items` is not `stretch`, map
      `align-items` in the same way as `align-self`.
   5. For vertical linear containers in RTL direction, swap physical `left`
      and `right` layout gravity.
2. If either cross-axis margin is `auto`, auto margins take precedence over
   item alignment:
   - if both logical cross margins are auto and the item is smaller than the
     cross-axis line, split the free space equally;
   - if one logical cross margin is auto and the item is smaller than the
     cross-axis line, assign all free space to that margin;
   - if the item is not smaller than the line, resolve auto margins so the
     item remains anchored at the logical start side.
3. Otherwise, if gravity is `end`, place the item's margin box at the
   cross-end side.
4. Otherwise, if gravity is `center`, center the item's margin box in the
   cross-axis content size.
5. Otherwise place the item's margin box at cross-start.
6. Convert the logical cross offset into the physical x/y offset using the
   cross-axis reversal state.

### 7. Baseline

If the container's main axis is horizontal, its baseline is the maximum
distance from each item's cross-start margin edge to that item's baseline,
after applying cross-axis alignment.

If the container's main axis is vertical, its baseline is based on the first
linear item's baseline plus the main-axis alignment start offset. If there are
no in-flow items or the first item has no baseline, the container has no
exported baseline.

### 8. Out-of-Flow Children

Absolutely positioned children are not linear items. After the in-flow linear
container size is known:

1. Use the container padding box as the containing block.
2. Resolve the child size and insets with the common out-of-flow algorithm.
3. If an inset pair is auto and a static-position fallback is required, use
   linear main-axis gravity for the main axis and computed linear layout
   gravity for the cross axis.
4. Lay out the child at the resolved out-of-flow offset.

Fixed descendants are collected by the root fixed-position pass.

## Non-Goals

This module does not define:

- multi-line wrapping;
- `flex-grow`, `flex-shrink`, or `flex-basis` behavior for linear items;
- baseline alignment as an item alignment mode;
- fragmentation;
- legacy quirks mode behavior;
- syntax parsing for raw CSS values.

## Implementation Anchors

- C++: `core/renderer/starlight/layout/linear_layout_algorithm.cc`
- C++ style data: `core/renderer/starlight/style/linear_data.h`
- Rust: `core/renderer/starlight/rust/crates/starlight_layout/src/engine.rs`
- Rust style data: `core/renderer/starlight/rust/crates/starlight_layout/src/style.rs`
- Rust tests: `core/renderer/starlight/rust/crates/starlight_parity/tests/linear_layout_tests.rs`

## Conformance

A Starlight linear layout implementation conforms to this document if, for all
style-resolved trees using the non-deprecated surface above, it produces the
same border-box sizes, offsets, margins, baselines, hidden-subtree behavior,
and out-of-flow static positions as this algorithm.
