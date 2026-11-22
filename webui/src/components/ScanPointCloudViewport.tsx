import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import * as THREE from 'three';
import { useOrbitControlsOwner, useOwnedOrbitControls, type OrbitControlsRef } from './useOrbitControls';

import type { LocalizationRun, LocalizationSessionGeometry } from '../api/deviceApi';
import { buildLocalizationOverlay } from '../domain/localizationOverlay';
import type { SceneModel } from '../domain/scene';
import { clientDiagnostics } from '../data/clientDiagnostics';
import type { RegistrationArtifact } from '../xr/fiducial/registrationArtifact';
import { REGISTRATION_INAPPLICABLE_KEYS } from '../xr/registrationApplicability';
import { solverCandidateVolume } from '../xr/candidateVolume';
import { fineOccupancyVoxelSizeMeters } from '../xr/occupancyScales';
import { deriveDeviceComputeProfile, type DeviceComputeProfile } from '../xr/deviceComputeProfile';
import { PostScanAdmissionCoordinator, runAdmittedTsdfRequest } from '../xr/postScanAdmission';
import type { ScanSceneMaterializationNeeds } from '../xr/postScanAdmission';
import { optionalMonocularPrior } from '../xr/monocularPriorRuntime';
import { releaseXrGuidanceRegistryWorkers } from '../xr/xrGuidanceRegistry';
import { describeMeshQuality } from '../xr/meshQualityUi';
import { useLocale, useT } from '../i18n';
import { formatDistance, formatNumber } from '../i18n/format';
import type { SensorPlacementMarker, SensorPlacementNode } from '../xr/sensorPlacementScene';
import { buildSourceHighlightScene, sourceVoxelGridForScan, type SourceHighlightScene } from '../xr/sourceHighlight';
import {
  displayFingerprint,
  scanModeLabel,
  type ScanDisplayOptions,
  type ScanMode,
} from '../xr/scanDisplayModes';
import {
  cameraPoseForPreset,
  shouldRefitCamera,
  clamp,
  orbitDistanceLimits,
  scanViewFitExtent,
  scanDimensionsText,
  scanLayerPlan,
  scanSourceBadgeText,
  ScanSceneMaterializer,
  toViewport,
  toViewportPositions,
  focusPoseForTarget,
  dragModeForPan,
  type CameraHudCommand,
  type PreparedScanScene,
  type SensorMarkerLayerSpec,
  type SourceHighlightLayerSpec,
  type ScanLayerSpec,
} from '../xr/scanViewportScene';
import { SparseTsdfMeshClient } from '../xr/sparseTsdfClient';
import type { SparseTsdfMesh } from '../xr/sparseTsdf';
import { TsdfMeshCache, tsdfSettingsKey } from '../xr/tsdfMeshCache';
import { Esp32VoxelScene, esp32VoxelSource, type Esp32VoxelView, type VoxelPick } from './Esp32VoxelLayers';
import { Esp32VoxelOverlay } from './Esp32VoxelOverlay';
import { MESH_MIN_POINTS } from '../xr/surfaceComponents';
import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import { SceneViewport, type ViewCommand } from './SceneViewport';
import { uiTheme } from '../theme';
import {
  resultViewportIdentityKey,
  VIEWPORT_FAILURE_MAX_ATTEMPTS,
  ViewportFailureBoundary,
  type ViewportFailure,
} from './ViewportFailureBoundary';

export type { Esp32VoxelStatus, Esp32VoxelView } from './Esp32VoxelLayers';

interface ScanPointCloudViewportProps {
  esp32Voxels?: Esp32VoxelView | null;
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
  display: ScanDisplayOptions;
  mode: ScanMode | null;
  sensorMarkers?: readonly SensorPlacementMarker[];
  selectedSensorNode?: SensorPlacementNode | null;
  onSensorMarkerSelect?: (node: SensorPlacementNode) => void;
  sensorGeometry?: LocalizationSessionGeometry | null;
  registrationArtifact?: RegistrationArtifact | null;
  objectId?: string | null;
  sessionId?: string | null;
  localizationRuns?: readonly LocalizationRun[];
  /** CRC identity of the displayed scan; null means that its generation is unknown. */
  currentScanId?: string | null;
  /** React identity for durable and provisional viewer resources. */
  scanIdentityKey?: string;
  /** True during the Finish-triggered scan/occupancy/preview transaction. */
  persistenceBusy?: boolean;
  /** Storage generation advanced at the Finish -> persistence handoff. */
  postScanGeneration?: number;
  computeProfile?: DeviceComputeProfile | null;
}

const NO_SENSOR_MARKERS: SensorPlacementMarker[] = [];
const NO_LOCALIZATION_RUNS: LocalizationRun[] = [];
const FALLBACK_COMPUTE_PROFILE = deriveDeviceComputeProfile({
  reportedLogicalProcessors: 1,
  reportedDeviceMemoryGiB: null,
  workerApi: true,
  crossOriginIsolated: false,
  sharedArrayBufferApi: false,
  wasmApi: true,
  wasmSharedMemory: false,
  webGpuApi: false,
  webGpuAdapter: false,
});

type TsdfRequestSnapshot = Pick<ScanPointCloudSnapshot,
  'tsdfPointCount' | 'tsdfPositions' | 'tsdfNormals' | 'tsdfCoverageConfidence' |
  'voxelSizeMeters' | 'freeAirPositions' | 'occupancyVoxelSizeMeters' | 'occupancyFineVoxelSizeMeters'>;

/** Build the exact post-scan TSDF input, keeping fine FREE spacing separate from coarsened UNKNOWN spacing. */
export function buildTsdfRequestArgs(snapshot: TsdfRequestSnapshot) {
  return {
    // Use ONLY locally certified geometry for the implicit surface. Display
    // surfels remain unchanged, so the operator can still see excluded evidence.
    positions: snapshot.tsdfPositions.slice(),
    normals: snapshot.tsdfNormals.slice(),
    confidence: snapshot.tsdfCoverageConfidence.slice(),
    // A slightly coarser field than the 4-cm association grid suppresses
    // sub-voxel ripples that are unsupported at this depth resolution.
    voxelSizeMeters: snapshot.voxelSizeMeters * 1.20,
    freeSpacePositions: snapshot.freeAirPositions.slice(),
    freeSpaceVoxelSizeMeters: fineOccupancyVoxelSizeMeters(snapshot),
    maxInputPoints: Math.min(9000, Math.max(1000, snapshot.tsdfPointCount)),
    maxTriangles: Math.max(3000, Math.min(18000, snapshot.tsdfPointCount * 14)),
  };
}

function sourceReasonText(reason: SourceHighlightScene['reason'], t: ReturnType<typeof useT>): string | null {
  if (reason === null) return null;
  if (reason === 'outside_grid') return t('scan.source_highlight.outside_grid');
  return t(REGISTRATION_INAPPLICABLE_KEYS[reason]);
}

function nodeForId(nodeId: number | null | undefined): SensorPlacementNode | null {
  return Number.isInteger(nodeId) && nodeId! >= 0 && nodeId! <= 4
    ? (['MAIN', 'G1', 'G2', 'G3', 'G4'] as const)[nodeId!]
    : null;
}

function sensorNode(value: string): SensorPlacementNode | null {
  return (['MAIN', 'G1', 'G2', 'G3', 'G4'] as readonly string[]).includes(value)
    ? value as SensorPlacementNode
    : null;
}

function labelTexture(label: string): THREE.CanvasTexture | null {
  if (typeof document === 'undefined') return null;
  const canvas = document.createElement('canvas');
  canvas.width = 256;
  canvas.height = 64;
  const context = canvas.getContext('2d');
  if (context !== null) {
    context.fillStyle = uiTheme.header;
    context.fillRect(2, 2, 252, 60);
    context.strokeStyle = uiTheme.muted;
    context.strokeRect(2, 2, 252, 60);
    context.fillStyle = uiTheme.text;
    context.font = '700 28px system-ui, sans-serif';
    context.textAlign = 'center';
    context.textBaseline = 'middle';
    context.fillText(label, 128, 32);
  }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

function PointLayer({ positions, color, size, opacity, depthTest = true, renderOrder = 0 }: { positions: Float32Array; color: string; size: number; opacity: number; depthTest?: boolean; renderOrder?: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]);
  if (positions.length === 0) return null;
  return <points geometry={geometry} frustumCulled={false} renderOrder={renderOrder}><pointsMaterial color={color} size={size} sizeAttenuation transparent={opacity < 1} opacity={opacity} depthTest={depthTest} depthWrite={false} /></points>;
}

function SurfaceMesh({ positions, wireframe }: { positions: Float32Array; wireframe: boolean }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); next.computeVertexNormals(); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <group><mesh geometry={geometry} renderOrder={4}><meshBasicMaterial color={uiTheme.accent} transparent opacity={wireframe ? 0.30 : 0.22} side={THREE.DoubleSide} depthWrite={false} /></mesh>{wireframe && <mesh geometry={geometry} renderOrder={5}><meshBasicMaterial color={uiTheme.text} transparent opacity={0.22} wireframe side={THREE.DoubleSide} depthWrite={false} /></mesh>}</group>;
}

function LineLayer({ positions, color, opacity, renderOrder }: { positions: Float32Array; color: string; opacity: number; renderOrder: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <lineSegments geometry={geometry} renderOrder={renderOrder}><lineBasicMaterial color={color} transparent opacity={opacity} depthTest={false} /></lineSegments>;
}

function ManualPin({ position, extent, id }: { position: readonly [number, number, number]; extent: number; id: number }) {
  const radius = clamp(extent * 0.022, 0.035, 0.10);
  return <group position={[position[0], position[1], position[2]]} renderOrder={50}><mesh renderOrder={50}><sphereGeometry args={[radius,18,12]} /><meshBasicMaterial color={uiTheme.accent} depthTest={false} /></mesh><mesh rotation={[Math.PI/2,0,0]} renderOrder={51}><torusGeometry args={[radius*1.8,radius*0.18,8,28]} /><meshBasicMaterial color={uiTheme.text} depthTest={false} /></mesh><sprite position={[0,radius*3.1,0]} scale={[radius*4.5,radius*2,1]} renderOrder={52}><spriteMaterial color={uiTheme.accent} depthTest={false} /></sprite><mesh position={[0,radius*3.1,0]} visible={false} name={`manual-pin-${id}`} /></group>;
}

function SensorMarkerLayer({ layer, extent, selectedNode, onSelect }: {
  layer: SensorMarkerLayerSpec;
  extent: number;
  selectedNode: SensorPlacementNode | null;
  onSelect: (node: SensorPlacementNode) => void;
}) {
  return <>{layer.markers.map(({ marker, positionViewport }) => (
    <SensorMarker key={marker.node} marker={marker} position={positionViewport} extent={extent}
      selected={selectedNode === marker.node} onSelect={onSelect} />
  ))}</>;
}

function SourceHighlightLayer({ layer }: { layer: SourceHighlightLayerSpec }) {
  const size = Math.max(0.025, layer.voxelSizeMeters);
  const markerSize = Math.max(0.035, size * 0.42);
  return <group>
    {layer.uncertaintyVoxels.map((position, index) => <mesh key={`uncertainty-${index}`} position={position} renderOrder={79}>
      <boxGeometry args={[size * 0.96, size * 0.96, size * 0.96]} />
      <meshBasicMaterial color={uiTheme.linkMeasuring} transparent opacity={0.22} depthTest={false} depthWrite={false} />
    </mesh>)}
    {layer.primaryVoxel !== null && <mesh position={layer.primaryVoxel} renderOrder={81}>
      <boxGeometry args={[size * 0.98, size * 0.98, size * 0.98]} />
      <meshBasicMaterial color={uiTheme.error} transparent opacity={0.78} depthTest={false} depthWrite={false} />
    </mesh>}
    {layer.markers.map(({ positionViewport, primary, edge }, index) => <group key={`source-${index}`} position={positionViewport} renderOrder={82}>
      <mesh renderOrder={82}>
        <sphereGeometry args={[markerSize * (primary ? 1.35 : 0.7), 12, 8]} />
        <meshBasicMaterial color={edge ? uiTheme.warning : uiTheme.text} depthTest={false} />
      </mesh>
      {edge && <mesh rotation={[Math.PI / 2, 0, 0]} renderOrder={83}>
        <torusGeometry args={[markerSize * 2, markerSize * 0.18, 6, 16]} />
        <meshBasicMaterial color={uiTheme.warning} depthTest={false} />
      </mesh>}
    </group>)}
  </group>;
}

function SensorMarker({ marker, position, extent, selected, onSelect }: {
  marker: SensorPlacementMarker;
  position: readonly [number, number, number];
  extent: number;
  selected: boolean;
  onSelect: (node: SensorPlacementNode) => void;
}) {
  const texture = useMemo(() => labelTexture(`${marker.node} · ${marker.status.replace('_', ' ')}${marker.provisional === true ? ' · ?' : ''}`), [marker.node, marker.status, marker.provisional]);
  useEffect(() => () => texture?.dispose(), [texture]);
  const color = selected ? uiTheme.accent : marker.status === 'anchored' ? uiTheme.good : marker.status === 'cs_projected' ? uiTheme.linkMeasuring : uiTheme.warning;
  const size = clamp(extent * 0.018, 0.035, 0.09);
  // Cap large 1-sigma regions at 25% of scan extent (with a 5 cm minimum cap) so they remain visible without swallowing the scan.
  const uncertaintyRadius = marker.uncertaintyM === null ? 0 : Math.min(marker.uncertaintyM, Math.max(0.05, extent * 0.25));
  const opacity = marker.provisional === true ? 0.55 : marker.status === 'cs_projected' ? 0.52 : 1;
  return <group position={[position[0], position[1], position[2]]} renderOrder={60}>
    {uncertaintyRadius > 0 && <mesh renderOrder={59}>
      <sphereGeometry args={[uncertaintyRadius, 18, 12]} />
      <meshBasicMaterial color={color} transparent opacity={0.12} depthWrite={false} />
    </mesh>}
    <mesh renderOrder={61} onPointerDown={(event) => { event.stopPropagation(); onSelect(marker.node); }}>
      <sphereGeometry args={[size, 18, 12]} />
      <meshBasicMaterial color={color} transparent={opacity < 1} opacity={opacity} depthTest={false} />
    </mesh>
    {(marker.status === 'visual_only' || selected) && <mesh rotation={[Math.PI / 2, 0, 0]} renderOrder={62}>
      <torusGeometry args={[size * 1.8, size * 0.14, 8, 28]} />
      <meshBasicMaterial color={color} depthTest={false} />
    </mesh>}
    <sprite position={[0, size * 2.8, 0]} scale={[Math.max(0.4, extent * 0.11), Math.max(0.1, extent * 0.028), 1]} renderOrder={63}>
      <spriteMaterial map={texture ?? undefined} color={color} transparent depthTest={false} />
    </sprite>
  </group>;
}

function CameraController({ command, extent, onInteractionEnd, controlsRef, panMode, hud }: { command: ViewCommand; extent: number; onInteractionEnd: (distance: number) => void; controlsRef: OrbitControlsRef; panMode: boolean; hud: CameraHudCommand }) {
  const { camera } = useThree();
  const controls = useOwnedOrbitControls(controlsRef);
  const operatorMoved = useRef(false);
  const lastCommandKey = useRef<string | null>(null);
  useEffect(() => {
    const limits = orbitDistanceLimits(extent);
    controls.enableDamping = true; controls.dampingFactor = 0.08; controls.enableRotate = true; controls.enableZoom = true; controls.enablePan = true; controls.screenSpacePanning = true; controls.rotateSpeed = 0.72; controls.zoomSpeed = 0.92; controls.panSpeed = 0.72; controls.minDistance = limits.minDistance; controls.maxDistance = limits.maxDistance; controls.touches.ONE = dragModeForPan(panMode) === 'pan' ? THREE.TOUCH.PAN : THREE.TOUCH.ROTATE; controls.touches.TWO = THREE.TOUCH.DOLLY_PAN; controls.mouseButtons.LEFT = dragModeForPan(panMode) === 'pan' ? THREE.MOUSE.PAN : THREE.MOUSE.ROTATE; controls.update();
    const handleStart = (): void => { operatorMoved.current = true; clientDiagnostics.info('SCAN_VIEW_GESTURE_START'); };
    const handleEnd = (): void => { const distance = controls.getDistance(); onInteractionEnd(distance); clientDiagnostics.info('SCAN_VIEW_GESTURE_END', { distance: Number(distance.toFixed(3)), x: Number(camera.position.x.toFixed(3)), y: Number(camera.position.y.toFixed(3)), z: Number(camera.position.z.toFixed(3)) }); };
    controls.addEventListener('start', handleStart); controls.addEventListener('end', handleEnd);
    return () => { controls.removeEventListener('start', handleStart); controls.removeEventListener('end', handleEnd); };
  }, [camera, controls, extent, onInteractionEnd, panMode]);
  useEffect(() => {
    const commandKey = `${command.preset}:${command.sequence}`;
    const commandChanged = lastCommandKey.current !== commandKey;
    lastCommandKey.current = commandKey;
    if (!shouldRefitCamera({ commandChanged, operatorMovedCamera: operatorMoved.current })) return;
    operatorMoved.current = false;
    const pose = cameraPoseForPreset(command.preset, extent);
    controls.target.set(pose.target[0], pose.target[1], pose.target[2]);
    camera.up.set(pose.up[0], pose.up[1], pose.up[2]);
    camera.position.set(pose.position[0], pose.position[1], pose.position[2]);
    camera.lookAt(controls.target); if (camera instanceof THREE.PerspectiveCamera) camera.updateProjectionMatrix(); controls.update(); onInteractionEnd(controls.getDistance());
  }, [camera, command.preset, command.sequence, controls, extent, onInteractionEnd]);
  const lastHudSequence = useRef(0);
  useEffect(() => {
    if (hud.sequence === 0 || hud.sequence === lastHudSequence.current) return;
    lastHudSequence.current = hud.sequence;
    if (hud.kind === 'focus' && hud.target !== null) {
      const pose = focusPoseForTarget({ cameraPosition: [camera.position.x, camera.position.y, camera.position.z], previousTarget: [controls.target.x, controls.target.y, controls.target.z], target: hud.target, extent });
      controls.target.set(pose.target[0], pose.target[1], pose.target[2]);
      camera.position.set(pose.position[0], pose.position[1], pose.position[2]);
    } else {
      const pose = cameraPoseForPreset(hud.kind === 'reset' ? 'perspective' : command.preset, extent);
      controls.target.set(pose.target[0], pose.target[1], pose.target[2]);
      camera.up.set(pose.up[0], pose.up[1], pose.up[2]);
      camera.position.set(pose.position[0], pose.position[1], pose.position[2]);
      operatorMoved.current = false;
    }
    camera.lookAt(controls.target); if (camera instanceof THREE.PerspectiveCamera) camera.updateProjectionMatrix(); controls.update(); onInteractionEnd(controls.getDistance());
  }, [camera, command.preset, controls, extent, hud, onInteractionEnd]);
  useFrame(() => controls.update()); return null;
}

function LayerView({ layer, extent, selectedNode, onSensorSelect }: {
  layer: ScanLayerSpec;
  extent: number;
  selectedNode: SensorPlacementNode | null;
  onSensorSelect: (node: SensorPlacementNode) => void;
}) {
  switch (layer.kind) {
    case 'points': return <PointLayer positions={layer.positions} color={layer.color} size={layer.size} opacity={layer.opacity} depthTest={layer.depthTest} renderOrder={layer.renderOrder} />;
    case 'mesh': return <SurfaceMesh positions={layer.positions} wireframe={layer.wireframe} />;
    case 'lines': return <LineLayer positions={layer.positions} color={layer.color} opacity={layer.opacity} renderOrder={layer.renderOrder} />;
    case 'pins': return <>{layer.pins.map((position, index) => <ManualPin key={index} id={index + 1} position={position} extent={extent} />)}</>;
    case 'sensor-markers': return <SensorMarkerLayer layer={layer} extent={extent} selectedNode={selectedNode} onSelect={onSensorSelect} />;
    case 'source-highlights': return <SourceHighlightLayer layer={layer} />;
  }
}

function SceneLayers({ voxelView, onVoxelPick, prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights, selectedNode, onSensorSelect, identityKey, overlayResetKeys, onOverlayFailure, onOverlayRecovered }: {
  voxelView: Esp32VoxelView | null;
  onVoxelPick: (pick: VoxelPick | null) => void;
  prepared: PreparedScanScene;
  display: ScanDisplayOptions;
  meshPositions: Float32Array;
  wireframe: boolean;
  sensorMarkers: readonly SensorPlacementMarker[];
  sourceHighlights: SourceHighlightScene;
  selectedNode: SensorPlacementNode | null;
  onSensorSelect: (node: SensorPlacementNode) => void;
  identityKey: string;
  overlayResetKeys: readonly unknown[];
  onOverlayFailure: (failure: ViewportFailure) => void;
  onOverlayRecovered: () => void;
}) {
  const layers = useMemo(() => scanLayerPlan(prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights), [prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights]);
  const isOverlayLayer = (layer: ScanLayerSpec): boolean => layer.dataType === 'sensor-markers' || layer.dataType === 'source-highlight';
  const baseLayers = layers.filter((layer) => !isOverlayLayer(layer));
  const overlayLayers = layers.filter(isOverlayLayer);
  return <>
    <color attach="background" args={[uiTheme.background]} />
    {baseLayers.map((layer) => <LayerView key={layer.id} layer={layer} extent={prepared.extent} selectedNode={selectedNode} onSensorSelect={onSensorSelect} />)}
    {voxelView !== null && <Esp32VoxelScene view={voxelView} centre={{ x: prepared.centerX, y: prepared.centerY, z: prepared.centerZ }} onPick={onVoxelPick} />}
    <ViewportFailureBoundary
      key={identityKey}
      resetKeys={overlayResetKeys}
      diagnosticEvent="SCAN_RESULT_OVERLAY_RENDER_FAILED"
      onFailure={onOverlayFailure}
      onRecovered={onOverlayRecovered}
      renderFailure={() => <group />}
    >
      {overlayLayers.map((layer) => <LayerView key={layer.id} layer={layer} extent={prepared.extent} selectedNode={selectedNode} onSensorSelect={onSensorSelect} />)}
    </ViewportFailureBoundary>
  </>;
}

export function ScanPointCloudViewport({
  snapshot,
  viewCommand: viewCommandProp,
  display: displayProp,
  mode,
  esp32Voxels = null,
  sensorMarkers = NO_SENSOR_MARKERS,
  selectedSensorNode,
  onSensorMarkerSelect,
  sensorGeometry = null,
  registrationArtifact = null,
  objectId = null,
  sessionId = null,
  localizationRuns = NO_LOCALIZATION_RUNS,
  currentScanId = null,
  scanIdentityKey,
  persistenceBusy = false,
  postScanGeneration = 0,
  computeProfile = null,
}: ScanPointCloudViewportProps) {
  const { locale } = useLocale();
  const t = useT();
  /* With the ESP32 voxels on screen the scan layers are hidden and none of their work (points, TSDF mesh) is requested. */
  const display = useMemo<ScanDisplayOptions>(() => esp32Voxels === null ? displayProp : {
    ...displayProp, goodPoints: false, badPoints: false, mesh: false, freeAir: false, unknownSpace: false, manualPoints: false,
  }, [displayProp, esp32Voxels === null]);
  const controlsOwner = useOrbitControlsOwner();
  const shellRef = useRef<HTMLDivElement | null>(null);
  const [panMode, setPanMode] = useState(false);
  const [hud, setHud] = useState<CameraHudCommand>({ kind: 'fit', sequence: 0, target: null });
  /* The HUD's own 3D/Top/Front presets (issue #41). A newer command from the parent wins again as soon as its object changes. */
  const [viewOverride, setViewOverride] = useState<{ base: ViewCommand; command: ViewCommand } | null>(null);
  const viewCommand = viewOverride !== null && viewOverride.base === viewCommandProp ? viewOverride.command : viewCommandProp;
  const requestPreset = (preset: ViewCommand['preset']): void => setViewOverride((current) => ({
    base: viewCommandProp,
    command: { preset, sequence: 1_000_000 + (current !== null && current.base === viewCommandProp ? current.command.sequence - 1_000_000 + 1 : 1) },
  }));
  const [fullscreen, setFullscreen] = useState(false);
  const [pickedVoxel, setPickedVoxel] = useState<VoxelPick | null>(null);
  const voxelSource = esp32VoxelSource(esp32Voxels);
  useEffect(() => { setPickedVoxel(null); }, [voxelSource]);
  useEffect(() => {
    if (typeof document === 'undefined') return undefined;
    const sync = (): void => setFullscreen(document.fullscreenElement !== null && document.fullscreenElement === shellRef.current);
    document.addEventListener('fullscreenchange', sync);
    return () => document.removeEventListener('fullscreenchange', sync);
  }, []);
  const requestedMaterialization: ScanSceneMaterializationNeeds = {
    good: display.goodPoints,
    bad: display.badPoints,
    freeAir: display.freeAir,
    unknownSpace: display.unknownSpace,
    manualPoints: display.manualPoints,
    dimensions: display.dimensions,
  };
  const admissionCoordinator = useRef(new PostScanAdmissionCoordinator());
  const materializer = useRef<ScanSceneMaterializer | null>(null);
  if (materializer.current === null) materializer.current = new ScanSceneMaterializer();
  const profile = computeProfile ?? FALLBACK_COMPUTE_PROFILE;
  const admission = useMemo(() => admissionCoordinator.current.evaluate({
    generation: postScanGeneration,
    snapshot,
    profile,
    persistenceBusy,
    materialization: requestedMaterialization,
    meshRequested: display.mesh,
    aiRequested: display.mesh && optionalMonocularPrior.isEnabled && optionalMonocularPrior.hasStagedPostScanFrame,
  }), [
    display.badPoints,
    display.dimensions,
    display.freeAir,
    display.goodPoints,
    display.mesh,
    display.manualPoints,
    display.unknownSpace,
    persistenceBusy,
    optionalMonocularPrior.hasStagedPostScanFrame,
    optionalMonocularPrior.isEnabled,
    postScanGeneration,
    profile,
    snapshot,
  ]);
  useEffect(() => {
    clientDiagnostics.info('SCAN_POST_SCAN_ADMISSION', {
      generation: admission.generation,
      persistenceBusy: admission.persistenceBusy,
      stages: admission.stages,
      admitted: admission.admitted,
      deferredReason: admission.deferredReason,
      originalSnapshotBytes: admission.bytes.originalSnapshotBytes,
      plannedViewportCopyBytes: admission.bytes.plannedViewportCopyBytes,
      plannedTsdfSliceBytes: admission.bytes.plannedTsdfSliceBytes,
      plannedWorkerScratchBytes: admission.bytes.plannedWorkerScratchBytes,
      totalPlannedBytes: admission.bytes.totalPlannedBytes,
      optionalAiReservationBytes: admission.bytes.optionalAiReservationBytes,
      memoryBudgetBytes: admission.bytes.memoryBudgetBytes,
      workerConcurrency: admission.bytes.workerConcurrency,
    });
  }, [admission]);
  const admittedNeeds: ScanSceneMaterializationNeeds = admission.admitted.viewportPointMaterialization
    ? requestedMaterialization
    : { good: false, bad: false, freeAir: false, unknownSpace: false, manualPoints: false, dimensions: false };
  const prepared = useMemo(() => materializer.current!.prepare(
    snapshot,
    postScanGeneration,
    admittedNeeds,
  ), [
    admittedNeeds.bad,
    admittedNeeds.dimensions,
    admittedNeeds.freeAir,
    admittedNeeds.good,
    admittedNeeds.manualPoints,
    admittedNeeds.unknownSpace,
    postScanGeneration,
    snapshot,
  ]);
  useEffect(() => () => materializer.current?.release(), []);
  useEffect(() => {
    releaseXrGuidanceRegistryWorkers();
  }, [postScanGeneration]);
  const candidateVolume = useMemo(() => solverCandidateVolume(snapshot), [snapshot]);
  const sourceGrid = useMemo(() => sourceVoxelGridForScan(snapshot), [snapshot]);
  const viewportIdentityKey = scanIdentityKey ?? resultViewportIdentityKey(currentScanId, postScanGeneration);
  const sourceHighlights = useMemo(() => objectId === null || sessionId === null
    ? { reason: null, primary: null, uncertaintyVoxels: [], secondary: [], voxelSizeMeters: 0, historyRuns: [], staleRuns: [] } as SourceHighlightScene
    : buildSourceHighlightScene(localizationRuns, { objectId, sessionId }, registrationArtifact, sourceGrid, { currentScanId }),
  [localizationRuns, objectId, registrationArtifact, currentScanId, sessionId, sourceGrid]);
  const overlayResetKeys = useMemo<readonly unknown[]>(
    () => [viewportIdentityKey, snapshot, sourceGrid, sensorMarkers, sensorGeometry, registrationArtifact, localizationRuns, objectId, sessionId],
    [localizationRuns, objectId, registrationArtifact, sensorGeometry, sensorMarkers, sessionId, snapshot, sourceGrid, viewportIdentityKey],
  );
  const [overlayFailure, setOverlayFailure] = useState<ViewportFailure | null>(null);
  const handleOverlayFailure = useCallback((failure: ViewportFailure): void => setOverlayFailure(failure), []);
  const handleOverlayRecovered = useCallback((): void => setOverlayFailure(null), []);
  const cameraFitExtent = useMemo(() => scanViewFitExtent(
    prepared,
    sourceHighlights,
    candidateVolume,
    display.freeAir || display.unknownSpace,
  ), [candidateVolume, display.freeAir, display.unknownSpace, prepared, sourceHighlights]);
  const tsdfClient = useMemo(() => new SparseTsdfMeshClient(), []);
  const tsdfCache = useMemo(() => new TsdfMeshCache<ScanPointCloudSnapshot, SparseTsdfMesh>(), []);
  const [tsdfMesh, setTsdfMesh] = useState<SparseTsdfMesh | null>(null);
  const [tsdfError, setTsdfError] = useState<string | null>(null);
  const [interactionRevision,setInteractionRevision] = useState(0);
  const [cameraDistance,setCameraDistance] = useState(Math.max(1.5, prepared.extent*1.75));
  const [internalSelectedNode, setInternalSelectedNode] = useState<SensorPlacementNode | null>(null);
  const selectedNode = selectedSensorNode === undefined ? internalSelectedNode : selectedSensorNode;
  const selectSensorNode = useCallback((node: SensorPlacementNode): void => {
    if (selectedSensorNode === undefined) setInternalSelectedNode(node);
    onSensorMarkerSelect?.(node);
  }, [onSensorMarkerSelect, selectedSensorNode]);
  const selectedMarker = sensorMarkers.find((marker) => marker.node === selectedNode) ?? null;
  const localizationOverlay = useMemo(
    () => buildLocalizationOverlay([], sensorGeometry, null),
    [sensorGeometry],
  );
  const sensorFrameScene = useMemo<SceneModel | null>(() => {
    if (registrationArtifact?.valid === true && registrationArtifact.sensor_to_scan !== null) return null;
    if (sensorGeometry?.available !== true) return null;
    const sensors = localizationOverlay.sensors.flatMap((sensor) => {
      const pod = sensorGeometry.pods.find((item) => item.pod === sensor.pod);
      const node = nodeForId(pod?.node_id);
      if (pod === undefined || node === null || pod.computed !== true || pod.assigned === false ||
          ![pod.x_mm, pod.y_mm, pod.z_mm].every((value) => value !== undefined && Number.isFinite(value)) ||
          (node !== 'MAIN' && pod.position_source !== 'channel_sounding' &&
            pod.position_source !== 'channel_sounding_baro')) return [];
      return [{
        id: node,
        label: node,
        role: node === 'MAIN' ? 'main' as const : 'remote' as const,
        position: [sensor.position.x_m, sensor.position.y_m, sensor.position.z_m] as const,
        locked: true,
      }];
    });
    if (sensors.length === 0 || !sensors.some((sensor) => sensor.role === 'remote')) return null;
    if (!sensors.some((sensor) => sensor.id === 'MAIN')) {
      sensors.push({ id: 'MAIN', label: 'MAIN', role: 'main', position: [0, 0, 0], locked: true });
    }
    return { frameId: 'LOCALIZATION_SENSOR_FRAME', units: 'm', source: 'saved', sensors };
  }, [localizationOverlay.sensors, registrationArtifact, sensorGeometry]);

  useEffect(() => () => tsdfClient.dispose(), [tsdfClient]);
  useEffect(() => {
    if ((!display.mesh || admission.bytes.underMemoryPressure || snapshot.tsdfPointCount < MESH_MIN_POINTS) && optionalMonocularPrior.isEnabled) {
      void optionalMonocularPrior.releaseMemory();
    }
  }, [admission.bytes.underMemoryPressure, display.mesh, optionalMonocularPrior.hasStagedPostScanFrame, optionalMonocularPrior.isEnabled, snapshot.tsdfPointCount]);
  useEffect(() => {
    let cancelled = false;
    if (snapshot.tsdfPointCount < MESH_MIN_POINTS) {
      tsdfClient.cancel('Scan does not meet the TSDF minimum point count.', true);
      setTsdfMesh(null);
      setTsdfError(null);
      return () => { cancelled = true; };
    }

    /* Idempotent by scan identity and settings: showing the Mesh layer again, an admission change or a re-render never recomputes a mesh we hold. */
    const settingsKey = tsdfSettingsKey({ allowOptionalAi: admission.admitted.optionalAi });
    const cachedMesh = display.mesh && admission.admitted.tsdfReconstruction ? tsdfCache.get(snapshot, settingsKey) : null;
    if (cachedMesh !== null) {
      tsdfClient.cancel('TSDF result already held for this scan.', true);
      setTsdfMesh(cachedMesh);
      setTsdfError(null);
      clientDiagnostics.info('SCAN_TSDF_CACHE_HIT', { triangles: cachedMesh.triangleCount, components: cachedMesh.meshTopologyComponentCount, settings: settingsKey });
      return () => { cancelled = true; };
    }

    const started = performance.now();
    const reconstruction = runAdmittedTsdfRequest(
      display.mesh,
      admission.admitted.tsdfReconstruction,
      () => buildTsdfRequestArgs(snapshot),
      (input) => tsdfClient.reconstruct(input, {
        postScanWorkerBudget: admission.bytes.workerConcurrency,
        allowOptionalAi: admission.admitted.optionalAi,
      }),
    );
    if (reconstruction === null) {
      tsdfClient.cancel(display.mesh ? 'TSDF reconstruction deferred by post-scan admission.' : 'Mesh layer hidden.', true);
      if (!display.mesh) {
        setTsdfMesh(null);
        setTsdfError(null);
      }
      return () => { cancelled = true; };
    }

    setTsdfMesh(null);
    setTsdfError(null);
    void reconstruction.then((result) => {
      if (cancelled) return;
      tsdfCache.set(snapshot, settingsKey, result);
      setTsdfMesh(result);
      clientDiagnostics.info('SCAN_TSDF_READY', {
        sourcePoints: result.sourcePointCount,
        certifiedPoints: snapshot.tsdfPointCount,
        integratedPoints: result.integratedPointCount,
        refineHolesFilled: (result as { meshRefine?: { holesFilled: number } }).meshRefine?.holesFilled ?? null,
        refineSmoothedVertices: (result as { meshRefine?: { smoothedVertices: number } }).meshRefine?.smoothedVertices ?? null,
        nodes: result.nodeCount,
        rawTriangles: result.rawTriangleCount,
        triangles: result.triangleCount,
        quarantinedTriangles: result.quarantinedTriangleCount,
        localHotspots: result.localNoiseHotspotCount,
        localHotspotTriangles: result.localNoiseHotspotTriangleCount,
        discardedTriangles: result.topologyDiscardedTriangleCount,
        nonManifoldEdges: result.nonManifoldEdgeCount,
        /* The component count of the FINAL triangle mesh (edge adjacency), the baseline for any stitching work (issue #43). */
        meshComponents: result.meshTopologyComponentCount,
        orientationConflicts: result.orientationConflictCount,
        selfIntersections: result.selfIntersectionPairCount,
        selfIntersectionExactTests: result.selfIntersectionExactTestedPairs,
        selfIntersectionBroadPairs: result.selfIntersectionBroadPhaseCandidatePairs,
        selfIntersectionTruncated: result.selfIntersectionTruncated,
        meshNoiseScore: Number(result.meshNoiseScore.toFixed(3)),
        meshReadiness: Number(result.meshReadiness.toFixed(3)),
        voxelMm: Math.round(result.voxelSizeMeters * 1000),
        truncationMm: Math.round(result.truncationMeters * 1000),
        computeMs: Number(result.computeMs.toFixed(1)),
        roundTripMs: Number((performance.now() - started).toFixed(1)),
      });
      void clientDiagnostics.flush();
    }).catch((error) => {
      if (cancelled) return;
      const message = error instanceof Error ? error.message : String(error);
      if (message.includes('reset') || message.includes('superseded') || message.includes('disposed')) return;
      setTsdfError(message);
      clientDiagnostics.warn('SCAN_TSDF_FAILED', error);
    });
    return () => {
      cancelled = true;
      tsdfClient.cancel('TSDF reconstruction demand or admission changed.', true);
    };
  }, [admission.admitted.optionalAi, admission.admitted.tsdfReconstruction, admission.bytes.workerConcurrency, display.mesh, postScanGeneration, snapshot, tsdfCache, tsdfClient]);

  const meshPositions = useMemo(
    () => tsdfMesh === null
      ? new Float32Array(0)
      : toViewportPositions(tsdfMesh.trianglePositions, prepared.centerX, prepared.centerY, prepared.centerZ),
    [prepared.centerX, prepared.centerY, prepared.centerZ, tsdfMesh],
  );

  const fingerprint = displayFingerprint(display); const modeLabel = scanModeLabel(mode, locale);
  const handleInteractionEnd = useCallback((distance: number): void => { setCameraDistance(distance); setInteractionRevision((current) => current+1); }, []);
  const meshQuality = tsdfMesh === null ? null : describeMeshQuality(tsdfMesh, locale);
  const postScanDeferred =
    (Object.values(requestedMaterialization).some(Boolean) && !admission.admitted.viewportPointMaterialization) ||
    (display.mesh && !admission.admitted.tsdfReconstruction);
  const meshStatus = postScanDeferred
    ? t('scan.badge.building_after_save')
    : snapshot.tsdfPointCount < MESH_MIN_POINTS
      ? t('scan.badge.mesh_more')
      : tsdfError !== null
      ? t('scan.badge.mesh_unavailable')
      : meshQuality === null
        ? t('scan.badge.mesh_building')
        : t('scan.surfel_count', { label: meshQuality.label, count: formatNumber(locale, snapshot.tsdfPointCount) });
  const sourceReasonLabel = sourceReasonText(sourceHighlights.reason, t);

  const focusTarget = selectedMarker !== null && selectedMarker.positionScanM !== null
    ? toViewport(selectedMarker.positionScanM[0], selectedMarker.positionScanM[1], selectedMarker.positionScanM[2], prepared.centerX, prepared.centerY, prepared.centerZ)
    : null;
  const requestHud = (kind: CameraHudCommand['kind']): void => setHud((current) => ({ kind, sequence: current.sequence + 1, target: kind === 'focus' ? focusTarget : null }));
  const fullscreenSupported = typeof document !== 'undefined' && document.fullscreenEnabled === true;
  const toggleFullscreen = (): void => {
    const shell = shellRef.current;
    if (shell === null) return;
    if (document.fullscreenElement !== null) void document.exitFullscreen();
    else void shell.requestFullscreen?.();
  };

  return <div ref={shellRef} className="viewport-shell scan-viewport" aria-label={t('scan.aria_label')} data-scan-mode={mode ?? 'custom'} data-layer-fingerprint={fingerprint} data-camera-revision={interactionRevision} data-camera-distance={cameraDistance.toFixed(4)} data-source-highlight-reason={sourceHighlights.reason ?? undefined}>
    <Canvas className="scan-webgl-canvas" camera={{ position:[3.2,2.5,3.6], fov:50, near:0.01, far:120 }} dpr={[1,1.5]} gl={{ antialias:true, alpha:false, powerPreference:'high-performance' }} onContextMenu={(event) => event.preventDefault()}><SceneLayers voxelView={esp32Voxels} onVoxelPick={setPickedVoxel} prepared={prepared} display={display} meshPositions={meshPositions} wireframe={mode === 'mesh'} sensorMarkers={sensorMarkers} sourceHighlights={sourceHighlights} selectedNode={selectedNode} onSensorSelect={selectSensorNode} identityKey={viewportIdentityKey} overlayResetKeys={overlayResetKeys} onOverlayFailure={handleOverlayFailure} onOverlayRecovered={handleOverlayRecovered} /><CameraController command={viewCommand} extent={cameraFitExtent} onInteractionEnd={handleInteractionEnd} controlsRef={controlsOwner} panMode={panMode} hud={hud} /></Canvas>
    <div className="scan-camera-hud" role="group" aria-label={t('scan.camera.hud_label')}>
      {(['perspective', 'top', 'front'] as const).map((preset) => <button key={preset} type="button" data-camera-action={`view-${preset}`} aria-pressed={viewCommand.preset === preset} onClick={() => requestPreset(preset)}>{preset === 'perspective' ? '3D' : t(preset === 'top' ? 'scan.view.top' : 'scan.view.front')}</button>)}
      <button type="button" data-camera-action="fit" onClick={() => requestHud('fit')}>{t('scan.camera.fit')}</button>
      <button type="button" data-camera-action="reset" onClick={() => requestHud('reset')}>{t('scan.camera.reset')}</button>
      <button type="button" data-camera-action="pan" aria-pressed={panMode} onClick={() => setPanMode((value) => !value)}>{t('scan.camera.pan')}</button>
      <button type="button" data-camera-action="focus" disabled={focusTarget === null} onClick={() => requestHud('focus')}>{t('scan.camera.focus')}</button>
      {fullscreenSupported && <button type="button" data-camera-action="fullscreen" aria-pressed={fullscreen} onClick={toggleFullscreen}>{fullscreen ? t('scan.camera.exit_fullscreen') : t('scan.camera.fullscreen')}</button>}
    </div>
    {overlayFailure !== null && <div className="xr-runtime-warning scan-overlay-failure" role="alert">
      <div>{t('scan.overlay.failed')}</div>
      <div>{overlayFailure.message}</div>
      {overlayFailure.attempts < VIEWPORT_FAILURE_MAX_ATTEMPTS && (
        <button type="button" onClick={overlayFailure.retry}>{t('scan.renderer.retry')}</button>
      )}
    </div>}
    <div className="scan-source-badge">{scanSourceBadgeText(snapshot, locale)}</div>
    {candidateVolume.candidateDepthUnavailable && <div className="scan-source-candidate-depth" role="status">{t('scan.candidate_depth_unavailable')}</div>}
    {postScanDeferred && !display.mesh && <div className="scan-mesh-overlay" role="status" aria-live="polite">{t('scan.badge.building_after_save')}</div>}
    {sourceReasonLabel !== null && <div className="scan-source-highlight-status" role="status">{sourceReasonLabel}</div>}
    {sourceHighlights.historyRuns.length > 0 && <div className="scan-source-history" aria-label={t('scan.source_history')}>
      {sourceHighlights.historyRuns.map(({ run, currency }) => <span key={`${run.method_name}-${run.computed_at_ms}`}>
        {run.method_name} · {t(currency === 'history_previous_scan' ? 'scan.source_history.previous_scan' : 'scan.source_history.scan_unknown')}
      </span>)}
    </div>}
    {esp32Voxels === null
      ? <div className="scan-mode-overlay">{modeLabel} · {fingerprint.toUpperCase()}</div>
      : <Esp32VoxelOverlay view={esp32Voxels} picked={pickedVoxel} />}
    {display.dimensions && <div className="scan-dimension-overlay">{scanDimensionsText(prepared, locale)}</div>}
    {display.mesh && <div className="scan-mesh-overlay" data-mesh-quality={meshQuality?.severity ?? 'pending'}>{meshStatus}</div>}
    {selectedMarker !== null && <section className="scan-sensor-details" aria-label={t('scan.selected_sensor_details')} data-sensor-status={selectedMarker.status}>
      <h3>{selectedMarker.node} · {t(`scan.marker_status.${selectedMarker.status}` as 'scan.marker_status.anchored' | 'scan.marker_status.cs_projected' | 'scan.marker_status.visual_only' | 'scan.marker_status.cs_unregistered')}</h3>
      <dl>
        <div><dt>{t('scan.source')}</dt><dd>{selectedMarker.source === 'channel_sounding_baro'
          ? t('scan.channel_sounding_baro')
          : selectedMarker.source === 'channel_sounding' ? t('scan.channel_sounding') : selectedMarker.source === 'apriltag' ? t('scan.tag_apriltag') : selectedMarker.source ?? '—'}</dd></div>
        <div><dt>{t('scan.uncertainty')}</dt><dd>{selectedMarker.uncertaintyM === null ? '—' : formatDistance(locale, selectedMarker.uncertaintyM, 3)}</dd></div>
        <div><dt>{t('scan.residual')}</dt><dd>{selectedMarker.residualM === null ? '—' : formatDistance(locale, selectedMarker.residualM, 3)}</dd></div>
        <div><dt>{t('scan.tag_id')}</dt><dd>{selectedMarker.tagId ?? '—'}</dd></div>
      </dl>
      <p>{t(`scan.marker_detail.${selectedMarker.status}` as 'scan.marker_detail.anchored' | 'scan.marker_detail.cs_projected' | 'scan.marker_detail.visual_only' | 'scan.marker_detail.cs_unregistered')}</p>
      {selectedMarker.provisional === true && <p data-marker-provisional="true">{t('scan.marker_provisional')}</p>}
    </section>}
    {sensorFrameScene !== null && <ViewportFailureBoundary
      key={viewportIdentityKey}
      resetKeys={overlayResetKeys}
      diagnosticEvent="SCAN_RESULT_OVERLAY_RENDER_FAILED"
      renderFailure={({ message, attempts, retry }) => (
        <div className="xr-runtime-warning scan-overlay-failure" role="alert">
          <div>{t('scan.overlay.failed')}</div>
          <div>{message}</div>
          {attempts < VIEWPORT_FAILURE_MAX_ATTEMPTS && (
            <button type="button" onClick={retry}>{t('scan.renderer.retry')}</button>
          )}
        </div>
      )}
    >
      <section className="scan-sensor-inset" aria-label={t('scan.unregistered_geometry')}>
        <div className="scan-sensor-inset-caption">{t('scan.unregistered_geometry')}</div>
        <SceneViewport scene={sensorFrameScene} selectedId={selectedNode ?? ''} viewCommand={viewCommand}
          onSelect={(node) => { const selected = sensorNode(node); if (selected !== null) selectSensorNode(selected); }} />
      </section>
    </ViewportFailureBoundary>}
    <div className="axis-note">{t('scan.axis_note')}</div>
  </div>;
}
