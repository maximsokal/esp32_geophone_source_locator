# WebXR depth / RGB / ToF strategy

This note records the reconstruction policy used by the diploma WebUI. The goal is to keep the browser implementation scientifically defensible while taking advantage of extra phone depth hardware and CPU parallelism whenever the browser/XR runtime can expose them safely.

## Browser data path

The immersive WebXR session requests:

- `depth-sensing` with CPU access;
- raw depth first, with smoothed depth as a fallback;
- a 16-bit CPU format when supported, with other specification formats as fallbacks;
- depth matched to the current `XRView`;
- `camera-access` for pose-synchronised RGB guidance;
- optional `plane-detection` as advisory native structural metadata.

`XRCPUDepthInformation` is treated as frame-local data. Its raw `data` buffer is consumed synchronously inside the active XR animation-frame callback and is never retained as a live browser depth object after that callback. Values are transformed from normalized view coordinates through `normDepthBufferFromNormView`, indexed in the row-major depth buffer and multiplied by `rawValueToMeters`. A raw value of zero remains invalid/no-depth evidence.

The raw camera texture returned by `XRWebGLBinding.getCameraImage()` is opaque user-agent-owned state. The application samples it into a small private 256x144 framebuffer but never modifies, redefines, deletes or otherwise takes ownership of the XR camera texture.

On WebGL2 the small framebuffer is read back through a bounded `PIXEL_PACK_BUFFER` ring with GPU fences. The XR callback never waits for the GPU: an unsignalled capture remains pending and, if all slots are occupied, advisory RGB is dropped. Every queued readback stores the exact RGB capture timestamp, projection and camera pose so delayed GPU delivery cannot be mistaken for a newer camera observation. WebGL1 retains a synchronous compatibility fallback.

## What `luminance-alpha` means

The WebXR depth data format named `luminance-alpha` is **not** a grayscale ToF/IR image. In CPU depth sensing it represents a 16-bit unsigned depth value. The current specification also defines `unsigned-short` for the same 16-bit class of depth data and `float32` for 32-bit floating-point depth.

The WebUI therefore must not label the depth buffer as a monochrome camera feed.

## Phones with ToF / hardware depth sensors

ARCore Depth can combine depth-from-motion with supported hardware depth sensors such as ToF automatically. When such hardware is available, the WebXR page can benefit from it indirectly through the depth values exposed by the browser/XR runtime. No separate WebUI code should pretend to select a ToF camera by media-device index.

ARCore documents two automatic depth sources: depth from motion through the main RGB camera and a supported hardware depth sensor such as ToF. Raw and full depth both use supported hardware sensors when they exist, but neither requires a ToF sensor. Hardware contribution can be especially valuable on low-texture surfaces and near the phone.

## Confidence / grayscale depth-quality image

Native ARCore Raw Depth exposes a matching 8-bit confidence image (Y8, 0..255) in addition to raw depth. That is the genuine grayscale per-pixel confidence source that would be useful for weighting reconstruction measurements. Raw depth is sparse and more accurate at valid pixels; the full depth path is denser because it is smoothed/interpolated.

The WebXR Depth Sensing API does not currently expose that native ARCore confidence image as a page-facing image/buffer. The specification instead allows the user agent to treat low-confidence native depth as invalid before exposing depth to the page. Consequently the browser pipeline must not fabricate a confidence map from `luminance-alpha` or claim access to the phone's IR/ToF intensity image.

If a later native Android companion is added, the correct integration boundary is an explicitly calibrated Raw Depth + Y8 confidence path with camera/depth intrinsics, extrinsics and timestamps. It must remain separate from generic `getUserMedia()` video devices unless their calibration and sensor semantics are actually known.

## Why not open a second camera with getUserMedia()

A generic `MediaStreamTrack` does not guarantee that the selected camera is a ToF/IR sensor, does not provide the WebXR depth coordinate transform, does not guarantee synchronized XR pose, and may compete with the active AR camera. Treating such a stream as metric depth would therefore create an uncalibrated data fusion path.

The browser implementation intentionally uses only:

1. physical WebXR/ARCore depth for metric geometry and occupancy;
2. pose-synchronised XR RGB for boundary/line/junction guidance;
3. optional calibrated monocular AI only as post-scan surface prior/veto;
4. verified physical depth rays only for FREE-space evidence;
5. optional native XR planes only as bounded normal-orientation guidance for already measured surfels.

Unknown/occluded interior volume is never converted to FREE or OCCUPIED merely because a native plane tracker extrapolates a surface behind an occluder.

## Mobile multicore policy

Ordinary page JavaScript remains on the main UI thread, but Dedicated Web Workers can execute CPU-heavy scanner branches in separate worker threads. The browser/OS decides which physical ARM cores execute them; page JavaScript cannot pin work to performance/efficiency cores.

`navigator.hardwareConcurrency` is treated only as a browser-provided scheduling hint. It can be lower than the physical/logical CPU topology, and heterogeneous mobile cores are not equivalent. The scanner therefore never starts one heavy worker per reported processor.

Before the user requests the immersive XR session, a non-benchmark capability negotiation resolves the fixed live maximum:

- browser-reported logical processors;
- Dedicated Worker availability;
- cross-origin-isolation / SharedArrayBuffer availability;
- Wasm shared-memory support;
- WebGPU adapter availability.

The live CPU scheduler deliberately reserves browser/UI/compositor headroom and currently caps simultaneous **heavy** XR workers at W2. The negotiated maximum is shown as `Wmax`; the current pressure-limited budget is `Weff`. Runtime frame-pressure hysteresis may immediately reduce W2 to W1 under high pressure, requires sustained medium pressure before degrading, and requires several healthy windows before recovering. Restarting a scan does not pretend a hot phone has cooled.

Structural geometry owns the critical heavy slot. Optional visual/Hough corroboration can use only spare capacity and is dropped instead of queued when no spare slot exists. Large one-shot arrays are transferred to workers with transferable `ArrayBuffer`s rather than duplicated by structured clone where ownership semantics allow it.

SharedArrayBuffer/Wasm threading remain optional optimizations because they require a secure cross-origin-isolated document (COOP/COEP). The existing Dedicated Worker architecture remains valid without them.

## Native WebXR plane guidance

When Plane Detection is available, `XRFrame.detectedPlanes` is copied only while the XR animation frame is active. The application retains no `XRPlane` or `XRSpace` objects; it stores only bounded numeric snapshots:

- project-space center and normal;
- project-space tangents corresponding to plane-local X/Z;
- an owned copy of the detected polygon in plane-local X/Z coordinates;
- polygon area, extent and tracker metadata for diagnostics/ranking.

The copied polygon, not its circumscribed radius, is the authoritative support region. The worker first applies radius and UV-AABB rejects, then exact point-in-polygon support with a small physical jitter margin. A native plane can only softly rotate the normal of an already measured, sufficiently confident physical surfel that lies close to that plane and inside the polygon. It cannot:

- create or delete surfels;
- change a surfel position;
- increase physical surfel confidence;
- create FREE or OCCUPIED voxels;
- certify a corner/edge by itself.

This makes system planes useful for stabilizing measured wall/table normals without allowing tracker extrapolation behind furniture to manufacture geometry.

## Feature-aware structural density and physical re-probing

Uniform downsampling is undesirable for rubble geometry because a flat wall and a corner do not carry the same structural information. The structural layer therefore estimates local geometric complexity from physically mature surfel normals:

- normal sign is ignored, because opposite normal directions can represent the same physical surface;
- normal evidence is canonicalized and processed deterministically so arrival order/view order does not change the family result;
- when live surfel maturity metadata is present, tentative/unpromoted representatives are ignored and a surfel needs at least two independent physical views before it can trigger a feature-density bonus;
- one abnormal normal is insufficient to declare a feature;
- smooth convex curvature is detected from robust angular spread;
- hard edges/corners are detected from multiple independently supported orientation families;
- when metric positions are supplied, orientation families need repeated members plus real metric span; several contradictory normals packed into one tiny spot do not receive a hard-feature bonus;
- complexity is evaluated over neighbouring spatial cells so a real edge is not lost merely because it lies on a hash-cell boundary;
- feature-rich regions retain more representatives than flat regions while the global memory ceiling remains unchanged;
- the number of orientation families per tiny cell remains bounded to reject mixed/flying-depth manifolds.

Current live-scale guards include a minimum normal confidence of 0.35, a 24-degree orientation-family gate, at least two members in a mature family, about 2 cm of family span when positions are known, about 3.5 cm of spatial baseline before curvature alone can drive density, and a 10 cm scale for full spatial support.

The depth sampler also performs bounded feature-directed physical re-probing. A cell must already contain several accepted physical surfels and cross a robust complexity threshold before it can request extra probes. Extra probes use a deterministic rotating local stencil and read the same physical WebXR depth source; AI output and native planes have no path to request them.

Per sampled XR frame the adaptive center-probe budgets are hard capped:

- 880 base low-discrepancy probes;
- at most 64 generic depth-discontinuity/edge probes;
- at most 96 feature-directed convex/edge/corner probes;
- feature probing is additionally capped per coverage cell so one pathological area cannot consume the frame budget.

This replaces the older one-recursion-level edge sampler, whose recursion depth was bounded but whose total child-probe count could still grow with every detected discontinuity in a frame.

## Finite measured support for edges and corners

A fitted mathematical plane is infinite, while a phone has only measured a finite patch. Therefore an infinite-plane intersection is never accepted as structural geometry by itself.

Each robust structural plane patch retains the actual physical inlier/support points that survived its fit. Candidate geometry is then bounded by those measurements:

- a two-plane edge is evaluated only where both fitted surfaces have measured points close to the mathematical intersection line;
- support points from each plane are projected onto the line and the candidate edge is clipped to the overlapping measured interval;
- non-overlapping support intervals reject the edge even when the infinite fitted planes intersect nearby;
- a three-plane corner requires local measured inliers from all three surfaces around the solved point, not merely nearby plane centroids;
- the uncertainty-aware three-plane solver weights its information/covariance by both offset variance and bounded physical plane-fit confidence;
- a weak plane therefore increases reported corner uncertainty rather than pretending to contribute the same information as a strong plane;
- the endpoint-based two-plane corner path requires finite support from both faces, no measured continuation beyond the endpoint, multi-view stability, and ray-verified FREE beyond the endpoint.

Operational lines have a separate physical scale/density policy after robust fitting and temporal tracking. A line shown to the operator must be at least 9 cm long, have at least six physical support points, satisfy at least about 22 support points per metre, and meet a support requirement that grows with line length. Shorter local fragments may exist only as bounded evidence that can merge into a longer finite measured line; they are not directly exposed as an operational LINE.

A vertex inferred from two mature finite 3D edges also has a scale-aware endpoint gap. The allowed sensor hole is 22% of the shorter edge length, clamped to 2.5–6 cm. Two barely valid short lines therefore cannot bridge the old universal 10 cm gap; only long, densely supported edges can tolerate the larger gap, and the inferred midpoint remains close to measured endpoints.

Candidate tracking also remains local. A tracked edge must satisfy direction, lateral-distance and finite-segment overlap gates before it can reuse an ID. Mature tracks use an innovation history to tighten their association radius. Multi-view evidence is generated from the current raw candidate position rather than the temporally smoothed track state. Endpoint tracks consume at most one observation per analysis frame so duplicated seeds cannot drag the same track several times during one keyframe.

These rules specifically prevent the failure mode where locally plausible infinite planes produce an intersection deep in empty space and temporal smoothing then makes that false candidate appear persistent.

## Mesh scale, connectivity and noise policy

Mesh generation is not started merely because many points exist. Connected physical surface components are analysed first with a spatial hash/BFS rather than all-pairs clustering. Candidate neighbours must satisfy voxel proximity, compatible normals and real support-distance checks; touching hash cells alone are not enough to join components.

For the current ~4 cm surfel association scale, a component needs all of the following before it is eligible for TSDF reconstruction:

- at least 24 accepted points;
- at least seven occupied connection cells;
- occupied-cell density of at least 0.30;
- a major physical extent of at least `max(0.14 m, 3.5 × voxel)`;
- a minor/two-dimensional extent of at least `max(0.075 m, 1.85 × voxel)`.

This prevents a dense tiny blob or a long one-dimensional strip from activating expensive triangulation. The main-thread preflight avoids unnecessary worker work, and the TSDF worker independently recomputes the connected-component mask and integrates only mesh-eligible points as defense in depth. Disconnected undersized islands therefore cannot borrow the point count of a valid slab.

Post-triangulation sanitation then:

- removes local extra faces on non-manifold edges while preserving the larger faces;
- repairs manifold winding consistently;
- detects non-adjacent triangle self-intersections with a bounded spatial broad phase;
- casts bounded short rays in both normal directions to detect nearby non-adjacent faces that normals pierce;
- ignores welded shared-vertex faces for that normal-intrusion diagnostic so a legitimate 90-degree structural fold is not classified as noise merely for being sharp;
- evaluates disconnected mesh components independently and can locally re-certify an isolated healthy component when a different pathological island exhausts a global diagnostic budget;
- discards/quarantines tiny, too narrow, open or diagnostically incomplete mesh islands without deleting their underlying physical surfels.

A remaining refinement is spatial hotspot repair inside one very large connected mesh component: component-wide intrusion/intersection ratios can dilute a small concentrated noisy patch. Any future local removal must require multiple correlated diagnostics (for example local self-intersection + normal intrusion/topology weakness), not a single normal hit, because real thin two-sided structures can legitimately trigger normal intrusion.

## Geophone-aware floating islands and coordinate frames

Sensor-aware surface relevance is intentionally frame-safe and currently **not wired into production mesh deletion**. The scanner and G1–G4 coordinates may only be compared after an explicit calibrated registration into the same coordinate frame.

The relevance policy already refuses a context when `scanFrameId !== sensorFrameId`, ignores unlocked/provisional sensor placements and measures distance to actual component surfels rather than only component centroids. A nearby locked sensor can only mark a quarantined physical component as `promote-near-sensor` metadata for preservation/rescan/future propagation modelling. A far sensor is never evidence that measured geometry is fake.

The current post-scan WebXR snapshot is rendered in its session-local scan frame and merely re-centred for display; this visual centring is **not** a transform into the demo/scene `MAIN_LOCAL` frame. Therefore questions such as “is this floating slab above/below G2?” remain unavailable until the sensor-placement/AR registration pipeline supplies a real rigid transform or places the sensors directly in the active XR frame.

## Compact readiness UI

The live guide intentionally reports capability growth rather than pretending all algorithms are available from the first points:

- `Sxx` — structural edge/corner readiness;
- `Mxx` — mesh readiness, or `M--` before a meaningful component exists;
- `Vn/16` — camera azimuth sectors observed around the measured component center; this is not falsely labelled as percent of physical surface coverage;
- before a stable object center exists, `Vxx` is only early view-progress;
- `A+xx` — approximate additional structural-area growth required when the measured component is still too small;
- `CPU Wcurrent/Wmax` — current heavy-worker budget versus the pre-scan negotiated live maximum.

The persistent immersive HUD is kept small: STABLE, READY, MOTION and CPU remain visible, while detailed weak/improving/stable counts and profiler data stay in Technical details.

## Safety pipeline

The live structural path is therefore:

**ARCore raw/smooth physical depth (+ ToF automatically when available) → physical multi-view surfels → robust metric feature density + bounded physical re-probing → optional polygon-bounded WebXR plane normal stabilization → finite-support robust plane/edge/corner hypotheses → physical FREE-space veto → candidate-specific multi-view tracking → pose-synchronised RGB reprojection/corroboration → manual/automatic target workflow.**

The mesh path is:

**certified physical surfels → connected-component physical scale/density gate → worker-side gate repeated → sparse directional TSDF → bounded topology/normal/self-intersection sanitation → retained/quarantined mesh components.**

Learned depth is not a geometry source in either path. Unknown volume stays UNKNOWN until physical evidence changes its state.

## Primary references

- WebXR Depth Sensing Module: https://immersive-web.github.io/depth-sensing/
- WebXR Raw Camera Access Module: https://immersive-web.github.io/raw-camera-access/
- WebXR Plane Detection Module: https://immersive-web.github.io/real-world-geometry/plane-detection.html
- Google ARCore Depth API: https://developers.google.com/ar/develop/depth
- Google ARCore DepthMode: https://developers.google.com/ar/reference/java/com/google/ar/core/Config.DepthMode
- Google ARCore Raw Depth / confidence: https://developers.google.com/ar/develop/java/depth/raw-depth
- MDN Web Workers: https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API
- MDN `navigator.hardwareConcurrency`: https://developer.mozilla.org/en-US/docs/Web/API/Navigator/hardwareConcurrency
- MDN SharedArrayBuffer / cross-origin isolation: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer
- PCL normal estimation / local covariance: https://pointclouds.org/documentation/tutorials/normal_estimation.html
- Open3D point-cloud plane segmentation: https://www.open3d.org/docs/latest/tutorial/geometry/pointcloud.html#Plane-segmentation
- MDN WebGL best practices: https://developer.mozilla.org/en-US/docs/Web/API/WebGL_API/WebGL_best_practices
