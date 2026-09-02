# XR / reconstruction reference projects review

This note records why several external projects were reviewed and which ideas are safe to reuse in the browser-based WebXR reconstruction pipeline.

## Deep-Live-Cam

Reference: https://github.com/hacksider/Deep-Live-Cam

Deep-Live-Cam is a desktop/Python real-time face-swap pipeline built around ONNX models and camera/video processing. It is useful as a general example of keeping expensive inference optional and hardware-accelerated, but it does not estimate metric scene depth, provide calibrated XR camera poses, build occupancy, or reconstruct arbitrary room/object geometry.

**Decision:** do not import its face-processing pipeline. The diploma already uses a smaller browser-side optional ONNX prior with a stricter safety boundary: AI can only accept/veto already measured surface evidence and cannot manufacture FREE/OCCUPIED space.

## WebAR.rocks.faceDepth

Reference: https://github.com/WebAR-rocks/WebAR.rocks.faceDepth

This browser project is more architecturally relevant because it keeps a learned depth-like representation in a GPU rendering path. Its Three.js helper reads the RGBD texture alpha channel as a normalized value in `[-1, 1]` and applies `depth * depthScale` as mesh displacement. That is a learned relative face deformation, not calibrated metric depth and not a ToF/ARCore depth map.

**Reusable idea:** keep learned geometry/depth priors separate from physical depth and consume them as advisory GPU/AI evidence.

**Do not reuse as metric geometry:** its depth scale cannot define meters, camera-space free volume, or seismic-search occupancy constraints.

## diff-gaussian-rasterization

Reference: https://github.com/slothfulxtx/diff-gaussian-rasterization

The project extends differentiable 3D Gaussian rasterization with rendered depth, normal, alpha and per-Gaussian attributes. It is a CUDA/PyTorch research rasterizer. Its README describes depth as the depth of Gaussian centers and normals as the shortest covariance axis with view-direction sign correction.

**Research value:** anisotropic local covariance / normal representations can be useful for offline analysis and future visualization experiments.

**Production decision:** do not put this CUDA rasterizer in the mobile WebXR hot path. It would add a much larger runtime/GPU requirement without improving the trust boundary of the physical ARCore measurements.

## WebXR Plane Detection is more directly useful

Reference specification: https://immersive-web.github.io/plane-detection/

Chrome 147 ships WebXR Plane Detection. An active XR frame can expose `detectedPlanes`; each `XRPlane` has a tracked `planeSpace`, a polygon, orientation and optional semantic label. The plane-space local +Y axis is the plane normal.

For this project native planes are treated only as **soft structural priors**:

- copy pose/polygon information while the XR frame is active;
- never retain frame-scoped XR objects outside the callback;
- never use a native plane to mark occluded volume as FREE;
- never create a certified surface/corner from a native plane alone;
- only stabilize the orientation of an already measured physical plane patch;
- keep existing physical surfel, free-space and multi-view/RGB certification as the final authority.

This preserves the project rule that unknown/occluded rubble interior remains UNKNOWN while still allowing system plane tracking to reduce plane-normal jitter and structural RANSAC work.

## Hardware depth / monochrome confidence

ARCore Raw Depth on native Android exposes a 16-bit raw depth image plus a matching Y8 confidence image (0..255). Full Depth provides dense smoothed depth but no separate confidence image. Both APIs can use supported hardware depth sensors such as ToF automatically.

The WebXR page therefore should consume WebXR/ARCore depth rather than attempt to open an arbitrary grayscale camera through `getUserMedia()`. A generic camera track does not guarantee ToF semantics, metric calibration, XR pose synchronization or depth confidence.
