### Tier 4 — WML compiler front-end

**Ship after Render Redesign Tier 4 / Lifecycle Tier D.**

- Implement a WML parser that produces a tree of `SceneNode`
  declarations + a `StyleSheet` per `<style>` block.
- The parser is a separate library (`omegaWTK::WML`) that depends on
  the engine but not vice versa. The engine has no awareness of
  WML — it only consumes the `StyleSheet` and `SceneNode` outputs.
- Layout properties in WML `<style>` blocks (`width: 320px`, `gap`,
  `flex-direction`) compile to `Layout` field assignments on the
  declaring node *at instantiation*, not to `StyleRule`s. Visual
  properties compile to `StyleRule`s. This is the place where the
  CSS-conflation-of-layout-and-visuals is resolved at the
  authoring/engine boundary.
- Tier 2 selector combinators (`>`, ` `, `+`, `~`) are added if and
  only if WML examples exercise them.
- Theme files (`.wtheme`) compile to `ThemeVars`.
- Components, slots, bindings, events from the WML spec are out of
  scope for this plan — they belong to the WML compiler proposal.

**Risk:** High in absolute terms (it's a parser + new compiler), low
relative to the engine refactor (it's purely additive — the engine
doesn't depend on WML).

**Files touched:** new `wtk/wml/` subtree. No changes to the engine
beyond what Tier 3 already shipped.