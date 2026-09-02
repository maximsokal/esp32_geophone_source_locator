# WebXR depth / RGB / ToF strategy

This note records the reconstruction policy used by the diploma WebUI. The goal is to keep the browser implementation scientifically defensible while taking advantage of extra phone depth hardware whenever the XR runtime can use it.

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

The WebUI therefore must not label the 160x90 depth buffer as a monochrome camera feed.

## Phones with ToF / hardware depth sensors

ARCore Depth can combine depth-from-motion with supported hardware depth sensors such as ToF automatically. When such hardware is available, the WebXR page can benefit from it indirectly through the depth values exposed by the browser/XR runtime. No separate WebUI code should pretend to select a ToF camera by media-device index.

This is especially valuable on low-texture surfaces and when phone motion is small, where a hardware depth sensor can provide information that motion-only depth has difficulty estimating.

## Confidence / grayscale depth-quality image

Native ARCore Raw Depth exposes a matching 8-bit confidence image (Y8, 0..255) in addition to raw depth. That is the genuine grayscale per-pixel confidence source that would be useful for weighting reconstruction measurements.

The WebXR Depth Sensing API does not currently expose that native confidence image as a page-facing image/buffer. The specification instead allows the user agent to treat low-confidence native depth as invalid before exposing depth to the page. Consequently the browser pipeline must not fabricate a confidence map from `luminance-alpha` or claim access to the phone's IR/ToF intensity image.

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

Uniform downsampling is undesirable for rubble geometry because a flat wall and a corner do not carry the same structural information. The structural layer therefore estimates local geometric complexity from already accepted surfel normals:

- normal sign is ignored, because opposite normal directions can represent the same physical surface;
- normal evidence is canonicalized and processed deterministically so arrival order/view order does not change the family result;
- one abnormal normal is insufficient to declare a feature;
- smooth convex curvature is detected from robust angular spread;
- hard edges/corners are detected from multiple independently supported orientation families;
- complexity is evaluated over neighbouring spatial cells so a real edge is not lost merely because it lies on a hash-cell boundary;
- feature-rich regions use smaller structural cells and tighter normal-family clustering, retaining more representatives than flat regions;
- the number of orientation families per tiny cell remains bounded to reject mixed/flying-depth manifolds.

The same physical complexity estimate now also controls upstream memory and sampling without raising the global scan-memory ceiling. Flat saturated cells can donate excess representatives while feature-rich cells retain more measured surfels.

The depth sampler also performs bounded feature-directed physical re-probing. A cell must already contain several accepted physical surfels and cross a robust complexity threshold before it can request extra probes. Extra probes use a deterministic rotating local stencil and read the same physical WebXR depth source; AI output and native planes have no path to request them.

Per sampled XR frame the adaptive center-probe budgets are hard capped:

- at most 64 generic depth-discontinuity/edge probes;
- at most 96 feature-directed convex/edge/corner probes;
- 880 base low-discrepancy probes remain unchanged.

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

Candidate tracking also remains local. A tracked edge must satisfy direction, lateral-distance and finite-segment overlap gates before it can reuse an ID. Mature tracks use an innovation history to tighten their association radius. Multi-view evidence is generated from the current raw candidate position rather than the temporally smoothed track state. Endpoint tracks consume at most one observation per analysis frame so duplicated seeds cannot drag the same track several times during one keyframe.

These rules specifically prevent the failure mode where locally plausible infinite planes produce an intersection deep in empty space and temporal smoothing then makes that false candidate appear persistent.

## Safety pipeline

The live structural path is therefore:

**ARCore raw/smooth physical depth (+ ToF automatically when available) → physical surfels → robust feature density + bounded physical re-probing → optional polygon-bounded WebXR plane normal stabilization → finite-support robust plane/edge/corner hypotheses → physical FREE-space veto → candidate-specific multi-view tracking → pose-synchronised RGB reprojection/corroboration → manual/automatic target workflow.**

Learned depth is not a geometry source in this path. Unknown volume stays UNKNOWN until physical evidence changes its state.

## Primary references

- WebXR Depth Sensing Module: https://immersive-web.github.io/depth-sensing/
- WebXR Raw Camera Access Module: https://immersive-web.github.io/raw-camera-access/
- WebXR Plane Detection Module: https://immersive-web.github.io/real-world-geometry/plane-detection.html
- Google ARCore Depth API: https://developers.google.com/ar/develop/depth
- Google ARCore Raw Depth / confidence: https://developers.google.com/ar/develop/java/depth/raw-depth
- PCL normal estimation / local covariance: https://pointclouds.org/documentation/tutorials/normal_estimation.html
- Open3D point-cloud plane segmentation: https://www.open3d.org/docs/latest/tutorial/geometry/pointcloud.html#Plane-segmentation
- MDN WebGL best practices: https://developer.mozilla.org/en-US/docs/Web/API/WebGL_API/WebGL_best_practices
