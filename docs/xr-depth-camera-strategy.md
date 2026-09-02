# WebXR depth / RGB / ToF strategy

This note records the reconstruction policy used by the diploma WebUI. The goal is to keep the browser implementation scientifically defensible while taking advantage of extra phone depth hardware whenever the XR runtime can use it.

## Browser data path

The immersive WebXR session requests:

- `depth-sensing` with CPU access;
- raw depth first, with smoothed depth as a fallback;
- a 16-bit CPU format when supported, with other specification formats as fallbacks;
- depth matched to the current `XRView`;
- `camera-access` for pose-synchronised RGB guidance.

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
4. verified physical depth rays only for FREE-space evidence.

Unknown/occluded interior volume is never converted to FREE merely because it is not visible to RGB or depth.

## Feature-aware structural density

Uniform downsampling is undesirable for rubble geometry because a flat wall and a corner do not carry the same structural information. The structural layer therefore estimates local geometric complexity from already accepted surfel normals:

- normal sign is ignored, because opposite normal directions can represent the same physical surface;
- one abnormal normal is insufficient to declare a feature;
- smooth convex curvature is detected from robust angular spread;
- hard edges/corners are detected from multiple supported orientation families;
- complexity is evaluated over neighbouring spatial cells so a real edge is not lost merely because it lies on a hash-cell boundary;
- feature-rich regions use smaller structural cells and tighter normal-family clustering, retaining more representatives than flat regions;
- the number of orientation families per tiny cell remains bounded to reject mixed/flying-depth manifolds.

The next allocator-level step is to let saturated scan-memory cells use this same complexity estimate when sharing the existing global point budget: flat cells can donate excess representatives while feature-rich cells retain more. The global memory ceiling must not increase merely because a region is classified as complex.

## Primary references

- WebXR Depth Sensing Module: https://immersive-web.github.io/depth-sensing/
- WebXR Raw Camera Access Module: https://immersive-web.github.io/raw-camera-access/
- Google ARCore Depth API: https://developers.google.com/ar/develop/depth
- Google ARCore Raw Depth / confidence: https://developers.google.com/ar/develop/java/depth/raw-depth
- PCL normal estimation / local covariance: https://pointclouds.org/documentation/tutorials/normal_estimation.html
- MDN WebGL best practices: https://developer.mozilla.org/en-US/docs/Web/API/WebGL_API/WebGL_best_practices
