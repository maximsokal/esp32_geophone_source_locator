from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# Reuse the already-probed compute profile and keep camera/depth capability
# semantics in one pure module. No device/model/label heuristics are introduced.
replace_once(
    'webui/src/xr/webxrRuntime.ts',
    "import { clientDiagnostics } from '../data/clientDiagnostics';\nimport { countCoverageQualityBands } from './coverageQuality';",
    "import { clientDiagnostics } from '../data/clientDiagnostics';\nimport { countCoverageQualityBands } from './coverageQuality';\nimport { deviceComputeProfilePromise } from './deviceComputeProfile';\nimport {\n  deriveNegotiatedXrDepthCapability,\n  probeVideoInputInventory,\n  summarizeComputeCapabilities,\n  type NegotiatedXrDepthCapability,\n} from './xrCapabilityProfile';",
    'runtime capability imports',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    '''export interface WebXrCapabilities {\n  secureContext: boolean;\n  /** Pre-session availability of XRWebGLBinding, not generic getUserMedia(). */\n  cameraApi: boolean;\n  webGl: boolean;\n  xrApi: boolean;\n  immersiveAr: boolean;\n  wakeLockApi: boolean;\n}''',
    '''export interface WebXrCapabilities {\n  secureContext: boolean;\n  /** Backward-compatible alias for rawXrRgbApi. */\n  cameraApi: boolean;\n  /** XRWebGLBinding exists; actual raw RGB is only proven by runtime capture. */\n  rawXrRgbApi: boolean;\n  webGl: boolean;\n  xrApi: boolean;\n  immersiveAr: boolean;\n  wakeLockApi: boolean;\n  videoInputEnumeration: boolean;\n  videoInputsEnumerated: boolean;\n  videoInputCount: number | null;\n  videoInputLabelsExposed: boolean;\n  /** Browser capability only: physical ToF/LiDAR/IR identity is never inferred. */\n  hardwareDepthSensor: 'unknown';\n  workerApi: boolean;\n  sharedMemory: boolean;\n  wasmThreads: boolean;\n  webGpu: boolean;\n}''',
    'extended WebXR capabilities',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    "interface DepthConfiguration { enabled:boolean; usage:string|null; dataFormat:string|null; type:string|null; }",
    "type DepthConfiguration = NegotiatedXrDepthCapability & { enabled:boolean; };",
    'depth configuration type',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    '''function readDepthConfiguration(session:XrSessionLike):DepthConfiguration{\n  try{const usage=session.depthUsage??null;const dataFormat=session.depthDataFormat??null;const type=session.depthType??null;return{enabled:usage==='cpu-optimized'&&session.depthActive!==false,usage,dataFormat,type};}\n  catch{return{enabled:false,usage:null,dataFormat:null,type:null};}\n}''',
    '''function readDepthConfiguration(session:XrSessionLike):DepthConfiguration{\n  try{const negotiated=deriveNegotiatedXrDepthCapability(session);return{...negotiated,enabled:negotiated.cpuDepth};}\n  catch{return{enabled:false,active:false,usage:null,dataFormat:null,type:null,rawDepth:false,smoothDepth:false,cpuDepth:false,gpuDepth:false};}\n}''',
    'depth capability derivation',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    '''export async function probeWebXrCapabilities():Promise<WebXrCapabilities>{\n  const xr=(navigator as NavigatorWithXr).xr;const xrApi=xr!==undefined;let immersiveAr=false;\n  if(xrApi&&window.isSecureContext){try{immersiveAr=await xr.isSessionSupported('immersive-ar');}catch{immersiveAr=false;}}\n  return{secureContext:window.isSecureContext===true,cameraApi:Boolean((window as WindowWithXr).XRWebGLBinding),webGl:hasWebGl(),xrApi,immersiveAr,wakeLockApi:(navigator as NavigatorWithWakeLock).wakeLock!==undefined};\n}''',
    '''export async function probeWebXrCapabilities():Promise<WebXrCapabilities>{\n  const xr=(navigator as NavigatorWithXr).xr;const xrApi=xr!==undefined;let immersiveAr=false;\n  if(xrApi&&window.isSecureContext){try{immersiveAr=await xr.isSessionSupported('immersive-ar');}catch{immersiveAr=false;}}\n  const [computeProfile,videoInputs]=await Promise.all([\n    deviceComputeProfilePromise,\n    probeVideoInputInventory(typeof navigator==='undefined'?undefined:navigator.mediaDevices),\n  ]);\n  const compute=summarizeComputeCapabilities(computeProfile);\n  const rawXrRgbApi=Boolean((window as WindowWithXr).XRWebGLBinding);\n  return{\n    secureContext:window.isSecureContext===true,cameraApi:rawXrRgbApi,rawXrRgbApi,webGl:hasWebGl(),xrApi,immersiveAr,\n    wakeLockApi:(navigator as NavigatorWithWakeLock).wakeLock!==undefined,\n    videoInputEnumeration:videoInputs.enumerationAvailable,videoInputsEnumerated:videoInputs.enumerated,\n    videoInputCount:videoInputs.videoInputCount,videoInputLabelsExposed:videoInputs.labelsExposed,\n    hardwareDepthSensor:videoInputs.hardwareDepthSensor,...compute,\n  };\n}''',
    'pre-session capability probe',
)

# Prefer CPU depth for metric surfel fusion, but let WebXR negotiate GPU depth as a
# fallback so capability telemetry can distinguish "depth exists" from "CPU depth
# is accessible to this scanner" instead of reporting a generic unavailable state.
replace_once(
    'webui/src/xr/webxrRuntime.ts',
    "        depthSensing:{usagePreference:['cpu-optimized'],dataFormatPreference:depthFormats,depthTypeRequest:['raw','smooth'],matchDepthView:true},",
    "        depthSensing:{usagePreference:['cpu-optimized','gpu-optimized'],dataFormatPreference:depthFormats,depthTypeRequest:['raw','smooth'],matchDepthView:true},",
    'CPU/GPU depth preference',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    "        depthError:depthConfiguration.enabled?null:'WebXR tracking is running, but CPU depth was not enabled.',error:null});",
    "        depthError:depthConfiguration.enabled?null:depthConfiguration.gpuDepth?'WebXR negotiated GPU-optimized depth, but metric surfel fusion currently requires CPU depth access.':'WebXR tracking is running, but CPU depth was not enabled.',error:null});",
    'GPU depth scanner limitation message',
)

replace_once(
    'webui/src/xr/webxrRuntime.ts',
    "clientDiagnostics.info('XR_SESSION_RUNNING',{referenceSpace:referenceSpaceType,domOverlay:session.domOverlayState!=null,environmentBlendMode:session.environmentBlendMode??null,depthEnabled:depthConfiguration.enabled,depthUsage:depthConfiguration.usage,depthFormat:depthConfiguration.dataFormat,depthType:depthConfiguration.type,cameraAccessRequested:true,planeDetectionRequested:true,rgbReadback:this.cameraCapture?.usesNonBlockingReadback?'pbo-fence':'sync-or-unavailable',monocularPriorStatus:optionalMonocularPrior.status});",
    "clientDiagnostics.info('XR_SESSION_RUNNING',{referenceSpace:referenceSpaceType,domOverlay:session.domOverlayState!=null,environmentBlendMode:session.environmentBlendMode??null,depthEnabled:depthConfiguration.enabled,depthActive:depthConfiguration.active,depthUsage:depthConfiguration.usage,depthFormat:depthConfiguration.dataFormat,depthType:depthConfiguration.type,rawDepth:depthConfiguration.rawDepth,smoothDepth:depthConfiguration.smoothDepth,cpuDepth:depthConfiguration.cpuDepth,gpuDepth:depthConfiguration.gpuDepth,cameraAccessRequested:true,rawXrRgbApi:Binding!==undefined,planeDetectionRequested:true,rgbReadback:this.cameraCapture?.usesNonBlockingReadback?'pbo-fence':'sync-or-unavailable',monocularPriorStatus:optionalMonocularPrior.status});",
    'session capability telemetry',
)

# Compact UI: "API available" and "captured successfully" are distinct. Expose
# the negotiated usage plus informational video-input/compute capabilities in the
# existing technical details; never display a guessed ToF/LiDAR state.
replace_once(
    'webui/src/components/XrRuntimePanel.tsx',
    '''<span className={runtimeState.rgbGuidanceAvailable?'good-text':''}>RGB {runtimeState.rgbGuidanceAvailable?'✓':'optional'}</span>''',
    '''<span className={runtimeState.rgbGuidanceAvailable?'good-text':''}>RGB {runtimeState.rgbGuidanceAvailable?'✓':capabilities?.rawXrRgbApi?'on start':'—'}</span>''',
    'raw RGB compact status',
)

replace_once(
    'webui/src/components/XrRuntimePanel.tsx',
    '''        <div><dt>Depth type</dt><dd>{runtimeState.depthType??'--'}</dd></div><div><dt>Depth format</dt><dd>{runtimeState.depthDataFormat??'--'}</dd></div><div><dt>Depth size</dt><dd>{runtimeState.depthWidth&&runtimeState.depthHeight?`${runtimeState.depthWidth} × ${runtimeState.depthHeight}`:'--'}</dd></div>\n        <div><dt>RGB guidance</dt><dd>{runtimeState.rgbGuidanceAvailable?`${runtimeState.rgbGuidanceWidth} × ${runtimeState.rgbGuidanceHeight}`:'--'}</dd></div>''',
    '''        <div><dt>Depth type</dt><dd>{runtimeState.depthType??'--'}</dd></div><div><dt>Depth usage</dt><dd>{runtimeState.depthUsage??'--'}</dd></div><div><dt>Depth format</dt><dd>{runtimeState.depthDataFormat??'--'}</dd></div><div><dt>Depth size</dt><dd>{runtimeState.depthWidth&&runtimeState.depthHeight?`${runtimeState.depthWidth} × ${runtimeState.depthHeight}`:'--'}</dd></div>\n        <div><dt>Raw XR RGB API</dt><dd>{capabilities?.rawXrRgbApi?'AVAILABLE':'--'}</dd></div><div><dt>RGB guidance</dt><dd>{runtimeState.rgbGuidanceAvailable?`${runtimeState.rgbGuidanceWidth} × ${runtimeState.rgbGuidanceHeight}`:'--'}</dd></div><div><dt>Video inputs</dt><dd>{capabilities?.videoInputsEnumerated?(capabilities.videoInputCount??0):capabilities?.videoInputEnumeration?'not exposed':'--'}</dd></div><div><dt>Hardware depth sensor</dt><dd>{capabilities?.hardwareDepthSensor?.toUpperCase()??'UNKNOWN'} · not inferred from camera names</dd></div>''',
    'technical XR sensor capabilities',
)

replace_once(
    'webui/src/components/XrRuntimePanel.tsx',
    '''        <div><dt>Live CPU workers</dt><dd>{computeLabel} · {computeScheduler.pressure.toUpperCase()}{computeScheduler.degraded?' throttled':''}</dd></div><div><dt>Active heavy jobs</dt><dd>{computeScheduler.active} · S{computeScheduler.structural} / V{computeScheduler.visual}</dd></div>''',
    '''        <div><dt>Worker API</dt><dd>{capabilities?.workerApi?'AVAILABLE':'--'}</dd></div><div><dt>Shared memory</dt><dd>{capabilities?.sharedMemory?'AVAILABLE':'--'}</dd></div><div><dt>Wasm threads</dt><dd>{capabilities?.wasmThreads?'AVAILABLE':'--'}</dd></div><div><dt>WebGPU</dt><dd>{capabilities?.webGpu?'AVAILABLE':'--'}</dd></div><div><dt>Live CPU workers</dt><dd>{computeLabel} · {computeScheduler.pressure.toUpperCase()}{computeScheduler.degraded?' throttled':''}</dd></div><div><dt>Active heavy jobs</dt><dd>{computeScheduler.active} · S{computeScheduler.structural} / V{computeScheduler.visual}</dd></div>''',
    'technical compute capabilities',
)
