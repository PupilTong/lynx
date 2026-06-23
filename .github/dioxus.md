# Dioxus for Lynx frontend exploration

This document summarizes the local analysis of `PupilTong/dioxus` after adding it as a Lynx submodule at `rust/dioxus`.

Pinned submodule snapshot:

- Repository: `https://github.com/PupilTong/dioxus.git`
- Path: `rust/dioxus`
- Commit: `b00ff7525 Allow using generated assets (#5490)`

## Goal

We want to reuse Dioxus as a Rust frontend layer for Lynx:

- `rsx!` as the authoring syntax.
- Dioxus `VirtualDom` and diff engine as the UI update engine.
- A new Lynx renderer instead of the existing Web/Desktop/LiveView renderers.
- Rust/WASM execution through the existing Lynx WAMR host integration.

The key finding is that Dioxus is already renderer-agnostic at the core diff boundary. The integration point is the `WriteMutations` trait in `packages/core/src/mutations.rs`.

## Repository map

Important crates for Lynx:

- `packages/dioxus`
  - Public facade crate.
  - Re-exports `dioxus-core`, `dioxus-core-macro`, `dioxus-html`, hooks, signals, and renderer-specific launch APIs behind features.
  - Has a `third-party-renderer` feature that suppresses "no renderer enabled" warnings.

- `packages/core`
  - Core runtime, `VirtualDom`, scopes, hooks storage, events, templates, VNodes, diffing, scheduler, and mutation protocol.
  - This is the part we most likely want to reuse unchanged.

- `packages/core-macro`
  - Exposes the proc macros: `rsx!`, `#[component]`, `#[derive(Props)]`.
  - `rsx!` delegates parsing to `dioxus-rsx`.

- `packages/rsx`
  - Parses `rsx!` syntax.
  - Generates static `Template` data plus dynamic node/attribute arrays.
  - This is the main syntax layer we want.

- `packages/html`
  - Provides the default HTML/SVG element and attribute namespace used by `rsx!`.
  - For Lynx, this should probably be replaced or supplemented with a Lynx element crate instead of forcing Lynx through HTML names.

- `packages/hooks` and `packages/signals`
  - State management and invalidation.
  - `use_signal` uses automatic dependency tracking and schedules dirty scopes through Dioxus core.

- `packages/web` and `packages/interpreter`
  - Useful reference implementations of a renderer.
  - Web implements `WriteMutations` in `packages/web/src/mutations.rs`.
  - The interpreter packages show a compact command-channel model that is conceptually close to what Lynx host functions may need.

Less relevant initially:

- `desktop`, `native`, `liveview`, `ssr`, `fullstack`, `router`, `devtools`, `cli`.
- They are useful references, but not needed for the smallest Lynx renderer.

## How `rsx!` lowers

Macro entry:

- `packages/core-macro/src/lib.rs`
  - `pub fn rsx(tokens: TokenStream)` parses `dioxus_rsx::CallBody`.

Parser and template generation:

- `packages/rsx/src/rsx_block.rs`
  - Parses attributes, spreads, and children.
  - Allows attributes before spreads before children.

- `packages/rsx/src/element.rs`
  - Turns static element structure into `TemplateNode::Element`.
  - Static attributes become `TemplateAttribute::Static`.
  - Dynamic attributes become `TemplateAttribute::Dynamic { id }`.
  - Dynamic children, loops, conditionals, raw expressions, components, and dynamic text become `TemplateNode::Dynamic { id }`.

- `packages/rsx/src/assign_dyn_ids.rs`
  - Assigns stable dynamic node IDs and records paths into the template tree.
  - Records `node_paths` and `attr_paths`.

- `packages/rsx/src/template_body.rs`
  - Emits:
    - static `TemplateNode` roots,
    - `node_paths`,
    - `attr_paths`,
    - runtime `dynamic_nodes`,
    - runtime `dynamic_attributes`,
    - `VNode::new(...)`.

Important property:

- Static structure is compiled into a `Template`.
- Runtime diff mostly skips static structure and only revisits dynamic slots.
- This is a good fit for Lynx because static node creation can be batched/cached, while updates can be small mutation lists.

## Runtime flow

Creation:

- `VirtualDom::new(root)` creates the runtime, scheduler channel, root scope, and root element ID `0`.

Initial render:

- `VirtualDom::rebuild(&mut writer)`
  - Runs root scope.
  - Creates scopes/VNodes.
  - Writes mutations to a renderer implementing `WriteMutations`.
  - Appends created nodes under `ElementId(0)`.

Updates:

- Events call `runtime.handle_event(name, event, ElementId)`.
- Signals/hooks schedule `SchedulerMsg::Immediate(scope_id)`.
- `VirtualDom::render_immediate(&mut writer)`:
  - drains queued work,
  - reruns dirty scopes from parent to child,
  - diffs old/new rendered nodes,
  - writes mutations to the renderer.

Scheduler:

- Dirty scopes have highest priority.
- Tasks/futures are second priority.
- Effects run after DOM/UI mutations have been applied.

This ordering matters for Lynx: the renderer should apply mutation batches before running after-render effects.

## Diff model

Core files:

- `packages/core/src/diff/mod.rs`
- `packages/core/src/diff/node.rs`
- `packages/core/src/diff/iterator.rs`
- `packages/core/src/diff/component.rs`

VNode diff:

- If old/new templates differ, Dioxus replaces the whole template.
- If templates match, Dioxus:
  - copies mount metadata,
  - diffs dynamic attributes,
  - diffs dynamic nodes.

Dynamic nodes:

- Text to text: emit `set_node_text` only if content changed.
- Placeholder to placeholder: no-op.
- Fragment to fragment: diff children.
- Component to same component function:
  - memoize props when possible,
  - otherwise rerun and diff that component scope.
- Different dynamic node kinds: create the new node, remove/replace the old node.

Children:

- Non-keyed children are diffed by index, with append/remove for length changes.
- Keyed children:
  - match common prefix/suffix,
  - remove non-shared keys,
  - use longest increasing subsequence to minimize moves in the middle section.

Implication for Lynx:

- Lynx renderer must support stable element IDs and moves/inserts/removes efficiently.
- Dioxus assumes renderer-side node IDs remain valid across mutations.

## Mutation boundary

The core renderer API is `WriteMutations`:

- `append_children(id, m)`
- `assign_node_id(path, id)`
- `create_placeholder(id)`
- `create_text_node(value, id)`
- `load_template(template, index, id)`
- `replace_node_with(id, m)`
- `replace_placeholder_with_nodes(path, m)`
- `insert_nodes_after(id, m)`
- `insert_nodes_before(id, m)`
- `set_attribute(name, namespace, value, id)`
- `set_node_text(value, id)`
- `create_event_listener(name, id)`
- `remove_event_listener(name, id)`
- `remove_node(id)`
- `push_root(id)`

Dioxus renderers use a stack protocol:

- New nodes are pushed onto a renderer-side stack.
- Operations such as append/insert/replace consume the last `m` nodes from that stack.
- Template roots are cached and cloned via `load_template`.

Existing references:

- Web renderer: `packages/web/src/mutations.rs`
- Native/interpreter mutation writer: `packages/interpreter/src/write_native_mutations.rs`
- JS interpreter stack implementation: `packages/interpreter/src/ts/core.ts`

## Proposed Lynx renderer shape

Add a Lynx-specific renderer crate, either inside the Dioxus fork or as a Lynx-side adapter:

- `dioxus-lynx` or `lynx-dioxus`
  - Provides `launch_lynx(root)` or `run_lynx(vdom)`.
  - Owns `VirtualDom`.
  - Owns `LynxMutations`.
  - Bridges host events back into `Runtime::handle_event`.

Minimal feature set:

```toml
dioxus = { path = ".../packages/dioxus", default-features = false, features = [
  "macro",
  "hooks",
  "signals",
  "third-party-renderer",
] }
```

If we keep using `dioxus-html` initially, add `html`. For a proper Lynx frontend, prefer a dedicated Lynx element crate.

### `LynxMutations`

Implement `WriteMutations` with a renderer-side state:

- `nodes: Vec<LynxNodeHandle>`
- `stack: Vec<LynxNodeHandle>`
- `templates: HashMap<Template, TemplateId>`
- root `ElementId(0)` mapped to Lynx root/container.

Host calls likely needed:

- Create:
  - `create_element(tag) -> handle`
  - `create_text(text) -> handle`
  - `create_placeholder() -> handle`
  - `clone_template(template_id, root_index) -> handle`

- Tree mutation:
  - `append_children(parent, children)`
  - `insert_before(target, children)`
  - `insert_after(target, children)`
  - `replace_with(target, children)`
  - `remove(handle)`

- Attributes/text:
  - `set_attribute(handle, name, namespace, value)`
  - `remove_attribute(handle, name, namespace)`
  - `set_text(handle, text)`

- Events:
  - `add_event_listener(handle, event_name)`
  - `remove_event_listener(handle, event_name)`

Template handling options:

1. Rust-side template creation:
   - On first `load_template`, walk `TemplateNode` in Rust and call host `create_element/create_text`.
   - Cache resulting native template handles by `Template`.
   - This is simplest.

2. Serialized template blob:
   - Serialize `TemplateNode` into compact bytes and pass to host once.
   - Host constructs/caches the native template tree.
   - Better long-term for fewer host calls.

3. Build-time template extraction:
   - Extract static templates during Rust build and embed a compact table.
   - Most work, likely not needed for first prototype.

## Lynx element namespace

Dioxus `rsx!` currently expects element/attribute constants from `dioxus_elements`.
The default `dioxus_elements` is `dioxus-html`.

For Lynx, we should introduce a Lynx element namespace instead of mapping everything through HTML:

- Elements:
  - `view`
  - `text`
  - `image`
  - `list`
  - `scroll_view`
  - `component`
  - benchmark-specific tags as needed

- Attributes:
  - `class`
  - `style`
  - `id`
  - layout props if needed
  - Lynx-specific props/events.

Possible approach:

- Copy the shape of `packages/html/src/elements.rs` and `attribute_groups.rs`.
- Generate zero-sized modules/constants for Lynx tags and attributes.
- Re-export this as `dioxus_elements` in the Lynx app prelude.

This lets code remain:

```rust
rsx! {
    view {
        class: "card",
        text { "hello" }
    }
}
```

without pretending that `view` is an HTML element.

## Events

Dioxus stores listeners as dynamic attributes:

- Listener attributes become `AttributeValue::Listener`.
- Diff only emits add/remove listener by event name and `ElementId`.
- The actual callback is held in Dioxus runtime state.

Web maps events by walking from the real event target to `data-dioxus-id`.

For Lynx:

- Native side should deliver `(event_name, element_id, event_payload)`.
- Rust side should construct `dioxus_core::Event<Rc<dyn Any>>`.
- Then call `runtime.handle_event(event_name, event, ElementId(element_id))`.
- After handling, call `vdom.render_immediate(&mut lynx_mutations)` and flush.

We need a Lynx-specific event data type analogous to Web's platform event data.

## Async and tasks

Dioxus supports futures/tasks and suspense.

For first Lynx/WAMR integration:

- Avoid depending on Tokio.
- Start with synchronous event-driven rendering.
- Support `use_signal`, `use_effect`, and plain callbacks first.
- Add a small local executor only if async hooks become necessary.

Risk:

- Dioxus examples may assume web/desktop async APIs.
- WAMR integration may need explicit host wakeups for timers/network/futures.

## CSS/style handling

Dioxus itself treats attributes generically. It does not parse CSS for the renderer.

For Lynx:

- `style: "..."` can initially be passed through as a string if Lynx accepts it.
- For performance, split style into structured Lynx props at compile time or first render.
- `class` can map to Lynx style/class handling if the bundle has CSS.

Open question:

- Whether the Dioxus Lynx path should emit Lynx template binary directly or use runtime host calls to create nodes.

For first prototype, runtime host mutations are lower risk.

## Risks and integration constraints

- `ElementId` stability is mandatory. Lynx renderer must maintain a stable `ElementId -> native handle` table.
- Template paths use `u8` segments. Individual template nodes cannot have more than 255 children at one level.
- `AttributeValue::Any` is not supported by the serializable/interpreter renderers. Lynx should initially reject or ignore it.
- Dioxus core uses `Rc`, `RefCell`, slabs, and boxed trait objects. This is fine in single-threaded WAMR, but memory footprint should be measured.
- Static template caching may make first render faster after warm-up, but first render still needs template registration.
- Dioxus `rsx!` is proc-macro based, so build tooling must compile proc macros for the host while targeting WASM for app code.
- The default facade feature set pulls in web/devtools/logger by default. Lynx examples should use `default-features = false`.

## Suggested phased plan

1. Keep Dioxus as `rust/dioxus` submodule.
2. Create a minimal Lynx Dioxus example crate outside the Dioxus submodule.
3. Depend on:
   - `dioxus-core`
   - `dioxus-core-macro`
   - `dioxus-hooks`
   - `dioxus-signals`
   - initially `dioxus-html` or a small local Lynx elements crate.
4. Implement `LynxMutations: WriteMutations`.
5. Implement a synchronous `launch_lynx(root)`:
   - create `VirtualDom`,
   - `rebuild`,
   - flush mutations,
   - handle events,
   - `render_immediate`,
   - flush mutations.
6. Map mutation host functions to existing Lynx/WAMR host integration.
7. Port the Rust/WAMR React-style demo and ColorfulView benchmark from Yew to Dioxus RSX.
8. Add CPB case comparing:
   - ReactLynx3,
   - Yew/WAMR,
   - Dioxus/WAMR,
   - Relax2VM.

## Bottom line

Dioxus is a good architectural match for this experiment because the expensive parts we want, `rsx!` and the diff engine, are isolated from the renderer. The smallest viable Lynx integration should not fork Dioxus core. It should add a Lynx element namespace and a Lynx `WriteMutations` implementation, then drive `VirtualDom` from Lynx/WAMR host events.
