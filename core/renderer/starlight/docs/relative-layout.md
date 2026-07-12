# Starlight Relative Layout Module Level 1

## Abstract

This specification defines Starlight relative layout, an id-based constraint
layout model for positioning in-flow children relative to parent edges or
sibling edges.

Starlight relative layout is not CSS `position: relative`. CSS relative
positioning shifts a box from its normal-flow position using inset properties;
Starlight relative layout establishes a container formatting context whose
children can align to parent or sibling sides, sit before or after sibling
sides, and optionally center themselves when unconstrained.

## Status of this Document

This document is a Starlight implementation specification. It is not a W3C
Recommendation and does not define web-facing CSS syntax by itself.

This document only describes the current non-deprecated behavior. It excludes:

- CSS `position: relative` visual-offset behavior;
- table-row relative positioning behavior from CSS Positioned Layout;
- legacy quirks mode behavior;
- parser-level syntax handling and shorthand expansion.

The layout engine consumes style resolver output. Invalid syntax is not a
layout repair case. If a syntactically valid style value is ignored, mapped, or
falls back in relative layout, that is part of this layout algorithm.

## References

This module follows the document organization and terminology style of:

- CSS Positioned Layout Module Level 3:
  https://www.w3.org/TR/css-position-3/
- CSS Display Module Level 3:
  https://www.w3.org/TR/css-display-3/

The CSS Positioned Layout module defines CSS `position: relative`; this
document references it only to distinguish that model from Starlight
`display: relative`.

## Module Interactions

Relative layout participates in the Starlight layout tree after style
resolution. It uses common Starlight sizing, edges, min/max, aspect-ratio,
measurement, rounding, positioning, and out-of-flow layout utilities.

Relative dependency values reference other relative items by `relative-id`.
These references are layout-time constraints, not selector matching and not
document-tree ids.

## Terminology

relative container

: A box whose effective display type is `relative`.

relative item

: An in-flow direct child of a relative container. Children with
  `display: none` and out-of-flow positioned children are not relative items.
  Children with `visibility: hidden` or `visibility: collapse` remain relative
  items unless they also have `display: none`.

relative id

: The integer stored in a relative item's `relative-id` property. `-1` is the
  none value and `0` is reserved for the parent reference. Other integers may
  identify relative items.

parent reference

: The reserved reference value `0`. On a definite axis, the parent start edge
  resolves to `0` and the parent end edge resolves to the content-box extent in
  that axis.

start side

: The physical left side in the horizontal axis, or the physical top side in
  the vertical axis.

end side

: The physical right side in the horizontal axis, or the physical bottom side
  in the vertical axis.

outer size

: The used border-box size plus used margins in one axis.

one-pass relative layout

: The algorithm selected by `relative-layout-once: true`. It uses one combined
  horizontal-and-vertical dependency order and measures each item as it is
  encountered.

two-pass relative layout

: The algorithm selected by `relative-layout-once: false`. It first resolves
  horizontal and vertical dependency orders separately, then remeasures children
  whose resolved constraints changed after proposed positions or wrap-content
  container sizes become known.

## Value Definitions

The following value names describe computed style data, not raw CSS parser
tokens.

Initial values in this section document the current Rust standalone style
surface. If another Starlight bridge materializes different compatibility
defaults before layout, the bridge owns that defaulting decision; this
document defines layout behavior for the computed values that reach the
algorithm.

### `display: relative`

Name: `display`

Value: `relative`

Applies to: layout containers

Effect: establishes a relative formatting context for the container's in-flow
children.

### `relative-id`

Name: `relative-id`

Value: integer

Initial: none (`-1`)

Applies to: relative items

The none value does not identify the item. The parent reference value `0` is
reserved and must not identify an item. If multiple relative items have the
same non-reserved id, references resolve to the last matching item in the
ordered relative item list.

### `relative-align-left`, `relative-align-right`,
`relative-align-top`, `relative-align-bottom`

Name: side alignment properties

Value: `none | parent | <relative-id>`

Initial: none

Applies to: relative items

These properties align one side of the item to the same side of the referenced
box:

- `relative-align-left` constrains the item's left margin edge to the
  referenced left side.
- `relative-align-right` constrains the item's right margin edge to the
  referenced right side.
- `relative-align-top` constrains the item's top margin edge to the referenced
  top side.
- `relative-align-bottom` constrains the item's bottom margin edge to the
  referenced bottom side.

### `relative-left-of`, `relative-right-of`, `relative-top-of`,
`relative-bottom-of`

Name: side adjacency properties

Value: `none | parent | <relative-id>`

Initial: none

Applies to: relative items

These properties place one side of the item before or after the opposite side
of the referenced box:

- `relative-right-of` constrains the item's left margin edge to the referenced
  right side.
- `relative-left-of` constrains the item's right margin edge to the referenced
  left side.
- `relative-bottom-of` constrains the item's top margin edge to the referenced
  bottom side.
- `relative-top-of` constrains the item's bottom margin edge to the referenced
  top side.

If both an alignment property and its adjacency fallback could constrain the
same side, the alignment property takes precedence.

### `relative-center`

Name: `relative-center`

Value: `none | horizontal | vertical | both`

Initial: `none`

Applies to: relative items

When neither side in an axis is constrained, this property centers the item in
that axis within the current relative bounds. `horizontal` centers only the
horizontal axis, `vertical` centers only the vertical axis, and `both` centers
both axes.

Parent-start and parent-end fallback rules take precedence over centering.

### `relative-layout-once`

Name: `relative-layout-once`

Value: `false | true`

Initial: `false`

Applies to: relative containers

When true, relative layout uses the one-pass combined dependency algorithm.
When false, it uses the two-pass algorithm.

## Relative Items

The children of a relative container are processed as follows:

1. Direct children with `display: none` are laid out as zero-sized hidden
   subtrees and are not relative items.
2. Direct children with `position: absolute` are not relative items. They are
   laid out after the in-flow container size is known.
3. Fixed descendants are handled by the common fixed-position pass and are not
   relative items.
4. Remaining direct children are relative items.
5. If any relative item has a non-zero `order`, items are sorted by `order`
   before relative-id lookup and dependency ordering. Otherwise source order is
   preserved. Sorting is stable with respect to equal order values.

## Reference Resolution

For a relative item and one axis:

1. A reference value of none (`-1`) resolves to no constraint.
2. A reference value of parent (`0`) resolves only when the parent content
   extent in that axis is definite for the current step:
   - parent start resolves to `0`;
   - parent end resolves to the content extent.
3. A non-reserved id resolves to the last matching item in the ordered relative
   item list. If no matching item exists, it resolves to no constraint.
4. A horizontal reference reads left or right from the referenced item.
5. A vertical reference reads top or bottom from the referenced item.

## Dependency Ordering

Relative layout builds a directed graph for the referenced ids in the relevant
scope:

- horizontal scope: `relative-right-of`, `relative-left-of`,
  `relative-align-left`, and `relative-align-right`;
- vertical scope: `relative-top-of`, `relative-bottom-of`,
  `relative-align-top`, and `relative-align-bottom`;
- combined scope: all horizontal and vertical dependency properties.

Items with no dependencies are processed first. When an item is processed,
dependents whose dependency sets become empty are appended to the ready queue.

If a cycle prevents a ready item from existing, the algorithm chooses the
lowest-index remaining item in the ordered relative item list and continues.
This makes circular dependencies deterministic without rejecting the tree.

## Layout Algorithm

This section defines the relative layout algorithm.

### 1. Initial Setup

1. Resolve the container's padding, border, and margin against the available
   parent constraints.
2. Resolve the container's definite width and height, including
   `box-sizing`, min/max constraints, and aspect ratio.
3. Establish available content width and height from fixed container sizes or
   bounded parent constraints.
4. Establish definite content width and height from fixed container sizes or
   definite parent constraints.
5. Lay out `display: none` direct children as hidden zero-sized subtrees.
6. Collect and order relative items as defined in "Relative Items".

### 2. Initial Child Constraints

For each relative item:

1. Resolve child edges against the available parent width when available.
2. Resolve child width and height against the definite parent content size in
   the corresponding axis when available.
3. If a child size is `fit-content()`, resolve its owner constraint from the
   definite content size, then the available content size, then an indefinite
   owner constraint.
4. If both item sides in an axis align to the parent and the parent content
   extent in that axis is definite, use a definite constraint equal to that
   extent minus the item's margins in the axis.
5. Otherwise, if the parent provides an available content size, use an
   at-most constraint reduced by margins.
6. Otherwise use an indefinite constraint.

In two-pass layout, initial child measurement does not treat the parent height
as definite unless the height is already definite before relative placement.
This allows vertical constraints to be resolved after horizontal sizing and
wrap-content effects are known.

### 3. Position Equation

For each item and axis, resolve a start constraint and an end constraint.

The start constraint is:

1. the side alignment start property if it is not none;
2. otherwise the adjacency property that places the item after the referenced
   end side.

The end constraint is:

1. the side alignment end property if it is not none;
2. otherwise the adjacency property that places the item before the referenced
   start side.

Given start, end, and the item's outer size:

1. If both start and end are definite, set the item start to start and item end
   to `max(end, start)`.
2. If only start is definite, set item end to `start + outer size`.
3. If only end is definite, set item start to `end - outer size`.
4. If neither is definite:
   1. if the item's end alignment targets parent, place the item ending at the
      current maximum relative bound;
   2. otherwise, if the item's start alignment targets parent or the item is
      not centered in this axis, place the item starting at the current minimum
      relative bound;
   3. otherwise center the item between the current minimum and maximum
      relative bounds.

When the parent extent is content-sized, the current relative bounds start at
`0..0` and grow to include each item as it is positioned.

### 4. One-Pass Relative Layout

If `relative-layout-once` is true:

1. Build the combined dependency order.
2. Initialize horizontal and vertical relative bounds from definite parent
   extents if present, or `0..0` otherwise.
3. For each item in combined order:
   1. refine its width and height constraints from resolved side positions
      when both sides in an axis are definite;
   2. lay out the item with the refined constraints;
   3. compute horizontal position and update horizontal bounds if the parent
      width is content-sized;
   4. compute vertical position and update vertical bounds if the parent
      height is content-sized.

### 5. Two-Pass Relative Layout

If `relative-layout-once` is false:

1. Compute horizontal dependency order and vertical dependency order
   separately.
2. Position items in the horizontal axis using the horizontal order.
3. Position items in the vertical axis using the vertical order.
4. Remeasure any item whose constraints become definite or tighter because
   both sides in an axis are resolved.
5. Reposition both axes.
6. Determine content width:
   - fixed or definite container width uses that content width;
   - otherwise use the horizontal relative content extent.
7. If the content width was not definite before this step:
   1. recompute horizontal positions against the resolved content width;
   2. remeasure items whose horizontal proposed sizes changed;
   3. reposition the vertical axis.
8. Determine content height:
   - fixed or definite container height uses that content height;
   - otherwise recompute vertical content extent from the current item set.
9. Recompute final horizontal and vertical positions against the final content
   width and height.

### 6. Container Size Determination

The relative container's border-box size is determined as follows:

1. If width or height is fixed, use the resolved fixed border-box size.
2. Otherwise, if the parent constraint in that axis is definite, use that
   border-box size.
3. Otherwise, use the final relative content extent in that axis plus padding
   and border.
4. Apply min/max constraints.
5. Clamp negative final sizes to zero.

### 7. Final Item Placement

After final content width and height are known:

1. Use the content box origin as the coordinate origin.
2. For each relative item, set the physical offset to:
   - `content-left + item.left + margin-left`;
   - `content-top + item.top + margin-top`.
3. Lay out the item at that final offset using the item constraints and any
   style override needed for resolved percentages.

Relative layout does not export a container baseline.

### 8. Out-of-Flow Children

Absolutely positioned children are not relative items. After the in-flow
relative container size is known:

1. Use the container padding box as the containing block.
2. Resolve the child size and insets with the common out-of-flow algorithm.
3. If an inset pair is auto and a static-position fallback is required, use the
   common block/relative static-position behavior.
4. Lay out the child at the resolved out-of-flow offset.

Fixed descendants are collected by the root fixed-position pass.

## Non-Goals

This module does not define:

- CSS `position: relative` visual offset rules;
- table-specific relative positioning;
- named anchors or selector-based references;
- fragmentation;
- legacy quirks mode behavior;
- syntax parsing for raw CSS values.

## Implementation Anchors

- C++: `core/renderer/starlight/layout/relative_layout_algorithm.cc`
- C++ style data: `core/renderer/starlight/style/relative_data.h`
- Rust: `core/renderer/starlight/rust/crates/starlight_layout/src/engine.rs`
- Rust style data: `core/renderer/starlight/rust/crates/starlight_layout/src/style.rs`
- Rust tests:
  `core/renderer/starlight/rust/crates/starlight_parity/tests/relative_display_layout_tests.rs`

## Conformance

A Starlight relative layout implementation conforms to this document if, for
all style-resolved trees using the non-deprecated surface above, it produces
the same border-box sizes, offsets, hidden-subtree behavior, dependency-order
fallbacks, duplicate-id resolution, and out-of-flow static positions as this
algorithm.
