# kREATE Test Assets

Everything in this folder exists for one reason: to give the kREATE tests real
geometry and real textures to load, so that mesh import, material binding, and
rendering can be exercised against content that behaves like content an actual
user would bring to the engine.

**These assets are for testing purposes only. They are not part of the shipped
engine, and they are not cleared for commercial use.** Do not copy them into a
product, a demo that ships, marketing material, or any other artifact that
leaves this repository. If you need art for something that ships, source it
separately with a license that covers that use.

That rule is a policy for this folder, not a restatement of every asset's
license — every license below is in fact more permissive than the policy, and all
of them do permit commercial use. The policy is the stricter of the two, and the
policy is what applies here. It keeps the whole folder on one simple footing so
nobody has to re-read a license file before deciding whether a test asset may be
reused elsewhere.

Every third-party asset here requires **attribution to its author**. That
obligation survives the policy above: if an asset is used or shown anywhere, the
credit line from its license file goes with it.

## How the art gets here

**The asset payloads are not committed.** Around 650MB of meshes and textures
would otherwise sit in git history forever, in every clone and every CI checkout,
so `.gitignore` in this folder keeps them out. What is committed is the part that
cannot be re-downloaded:

- **Each asset's `LICENSE.txt` / `license.txt`.** Sketchfab does not put the
  license inside the zip it hands you, so these files exist nowhere but here. If
  one is lost, the attribution obligation goes with it and the asset's provenance
  cannot be reconstructed. Do not "clean up" these files.
- **`orange_tennis_racket/` in full.** It was generated with Meshy AI rather than
  downloaded, so there is no upstream to pull it from. It lives in the tree
  through Git LFS (see `.gitattributes`).

The plan is for `autom-deps` to fetch the rest from stable archive URLs, the way
it already fetches the Vulkan SDK and Strawberry Perl. **That is not wired up
yet** — it is waiting on a home for the archives, because a Sketchfab download
link is signed and expiring and cannot be put in a manifest. Until it lands, a
fresh clone gets the licenses and the racket but none of the downloaded art, and
the models below have to be fetched by hand from the source URLs in the credits.
An empty asset folder on a new checkout is that gap, not a bug.

Two things whoever wires this up will hit. `autom-deps` extracts **zip and tar
only** — it has no RAR support, and the palm tree and dirt road ship a `.rar`
nested inside their Sketchfab zip. And `--clean` does an `rmtree` on a
dependency's `dest`, so the extraction target must be a subdirectory rather than
the asset folder itself, or cleaning an asset will delete the one license file we
can never get back. Re-packing each asset once into a normalized tarball solves
both.

## Model entry points

The downloads unpack to whatever folder structure their author happened to use,
so the path to the actual model is not consistent between them. These are the
files a test should load:

| Asset | Model file |
| --- | --- |
| `1965-ford-mustang-convertible` | `source/Mesh.obj` — **but see the note below; its `.mtl` is missing** |
| `boulder` | `source/Low Poly.fbx` |
| `orange_tennis_racket` | `orange_tennis_racket.fbx` |
| `realistic-palm-tree-free` | `source/palm_hp_3/Palm HP 3/HP_Palm3_1.fbx` |
| `screen_space_reflection_demo_follmann_2.og` | `scene.gltf` (with `scene.bin`) |
| `sofa` | `source/ready.obj` |
| `the-ancient-gnarled-tree-3d-model-free` | `source/model.glb` |
| `update-dirt-road-through-forest` | `source/dirt_road_forest_scene/Dirt Road Forest Scene/SOURCE/Dirt road scene.fbx` (an `.obj` + `.mtl` of the same scene sits beside it) |

Several of these paths contain spaces. Quote them.

The dirt road scene also ships its own `TEXTURES/` tree underneath its `SOURCE/`
folder, separate from the sibling `textures/` directory that came with the
Sketchfab download — its `.mtl` references the former, so do not assume the
top-level `textures/` folder is the one a given asset's material actually points
at.

**The Mustang has no material file.** Its `source/Mesh.obj` declares
`mtllib Mesh.mtl` on the first line, but no `Mesh.mtl` was included in the
download — the folder holds the `.obj` and the ten textures in `textures/` and
nothing else. A loader will therefore import the geometry with no material
bindings, and the ten maps (`Mustang_BaseColor.jpg`, `Mustang_Normal.jpg`, and
so on) will not be attached to anything. Either bind those maps to the mesh in
the test that loads it, or author the missing `.mtl`. Until then, treat the
Mustang as a geometry-only fixture and do not read an untextured render of it as
a material-system bug.

## What is here, and where it came from

### `1965-ford-mustang-convertible/`

"1965 Ford Mustang Convertible" (https://skfb.ly/6TM9O) by **Kristian LaGrange**,
licensed under **CC Attribution 4.0**
(http://creativecommons.org/licenses/by/4.0/). Author must be credited.

This replaced an earlier McLaren 765LT model that was CC BY-NC-SA — the swap was
made deliberately to get the folder onto a uniformly permissive footing, so
please keep any future car model on an attribution-only license rather than
reintroducing a non-commercial or share-alike term.

### `boulder/`

"Boulder" (https://skfb.ly/6XrBp) by **lukica**, licensed under **CC Attribution
4.0** (http://creativecommons.org/licenses/by/4.0/). Author must be credited.

### `realistic-palm-tree-free/`

"Realistic Palm Tree Free" (https://skfb.ly/prWT9) by **Next Spring**, licensed
under **CC Attribution 4.0** (http://creativecommons.org/licenses/by/4.0/).
Author must be credited.

### `screen_space_reflection_demo_follmann_2.og/`

"Screen Space Reflection Demo: Follmann 2.OG"
(https://sketchfab.com/3d-models/screen-space-reflection-demo-follmann-2og-6164eed28c464c94be8f5268240dc864)
by **Sketchfab**, licensed under **CC-BY 4.0**
(http://creativecommons.org/licenses/by/4.0/). Author must be credited. The
required credit line is in the asset's `license.txt`.

### `sofa/`

"Sofa" (https://skfb.ly/68QAz) by **katjagricishina**, licensed under **CC
Attribution 4.0** (http://creativecommons.org/licenses/by/4.0/). Author must be
credited.

### `the-ancient-gnarled-tree-3d-model-free/`

"The Ancient Gnarled Tree 3d model free" (https://skfb.ly/pJPxQ) by
**iGauravRajput**, licensed under **CC Attribution 4.0**
(http://creativecommons.org/licenses/by/4.0/). Author must be credited.

### `update-dirt-road-through-forest/`

"[UPDATE] Dirt Road Through Forest" (https://skfb.ly/pLGyB) by **99.Miles**,
licensed under **CC Attribution 4.0**
(http://creativecommons.org/licenses/by/4.0/). Author must be credited.

### `orange_tennis_racket/`

Generated with **Meshy AI** by the project author. No third-party license
attaches to it, and it carries no attribution requirement. It is kept here as a
test asset like the others.

## Adding a new asset

Keep the original license or attribution file that came with the download,
unmodified, inside the asset's own folder — that file is the authoritative
record, and this README only summarizes it. Then add a section above naming the
work, the author, the source URL, and the license. An asset with no traceable
provenance does not belong in the repository.
