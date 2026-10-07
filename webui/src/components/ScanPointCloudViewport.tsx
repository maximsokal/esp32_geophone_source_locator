import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import type { LocalizationRun, LocalizationSessionGeometry } from '../api/deviceApi';
import { buildLocalizationOverlay } from '../domain/localizationOverlay';
import type { SceneModel } from '../domain/scene';
import { clientDiagnostics } from '../data/clientDiagnostics';
import type { RegistrationArtifact } from '../xr/fiducial/registrationArtifact';
import { REGISTRATION_INAPPLICABLE_KEYS } from '../xr/registrationApplicability';
import { solverCandidateVolume } from '../xr/candidateVolume';
import { fineOccupancyVoxelSizeMeters } from '../xr/occupancyScales';
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
  clamp,
  orbitDistanceLimits,
  prepareScanScene,
  scanViewFitExtent,
  scanDimensionsText,
  scanLayerPlan,
  scanSourceBadgeText,
  toViewportPositions,
  type PreparedScanScene,
  type SensorMarkerLayerSpec,
  type SourceHighlightLayerSpec,
  type ScanLayerSpec,
} from '../xr/scanViewportScene';
import { SparseTsdfMeshClient } from '../xr/sparseTsdfClient';
import type { SparseTsdfMesh } from '../xr/sparseTsdf';
import { MESH_MIN_POINTS } from '../xr/surfaceComponents';
import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import { SceneViewport, type ViewCommand } from './SceneViewport';

interface ScanPointCloudViewportProps {
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
}

const NO_SENSOR_MARKERS: SensorPlacementMarker[] = [];
const NO_LOCALIZATION_RUNS: LocalizationRun[] = [];

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
    context.fillStyle = 'rgba(8, 16, 29, 0.92)';
    context.fillRect(2, 2, 252, 60);
    context.strokeStyle = '#94a3b8';
    context.strokeRect(2, 2, 252, 60);
    context.fillStyle = '#f8fafc';
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
  return <group><mesh geometry={geometry} renderOrder={4}><meshBasicMaterial color="#38cfd2" transparent opacity={wireframe ? 0.30 : 0.22} side={THREE.DoubleSide} depthWrite={false} /></mesh>{wireframe && <mesh geometry={geometry} renderOrder={5}><meshBasicMaterial color="#a5f3fc" transparent opacity={0.22} wireframe side={THREE.DoubleSide} depthWrite={false} /></mesh>}</group>;
}

function LineLayer({ positions, color, opacity, renderOrder }: { positions: Float32Array; color: string; opacity: number; renderOrder: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <lineSegments geometry={geometry} renderOrder={renderOrder}><lineBasicMaterial color={color} transparent opacity={opacity} depthTest={false} /></lineSegments>;
}

function ManualPin({ position, extent, id }: { position: readonly [number, number, number]; extent: number; id: number }) {
  const radius = clamp(extent * 0.022, 0.035, 0.10);
  return <group position={[position[0], position[1], position[2]]} renderOrder={50}><mesh renderOrder={50}><sphereGeometry args={[radius,18,12]} /><meshBasicMaterial color="#f05cff" depthTest={false} /></mesh><mesh rotation={[Math.PI/2,0,0]} renderOrder={51}><torusGeometry args={[radius*1.8,radius*0.18,8,28]} /><meshBasicMaterial color="#ffffff" depthTest={false} /></mesh><sprite position={[0,radius*3.1,0]} scale={[radius*4.5,radius*2,1]} renderOrder={52}><spriteMaterial color="#f05cff" depthTest={false} /></sprite><mesh position={[0,radius*3.1,0]} visible={false} name={`manual-pin-${id}`} /></group>;
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
      <meshBasicMaterial color="#a78bfa" transparent opacity={0.22} depthTest={false} depthWrite={false} />
    </mesh>)}
    {layer.primaryVoxel !== null && <mesh position={layer.primaryVoxel} renderOrder={81}>
      <boxGeometry args={[size * 0.98, size * 0.98, size * 0.98]} />
      <meshBasicMaterial color="#f472b6" transparent opacity={0.78} depthTest={false} depthWrite={false} />
    </mesh>}
    {layer.markers.map(({ positionViewport, primary, edge }, index) => <group key={`source-${index}`} position={positionViewport} renderOrder={82}>
      <mesh renderOrder={82}>
        <sphereGeometry args={[markerSize * (primary ? 1.35 : 0.7), 12, 8]} />
        <meshBasicMaterial color={edge ? '#facc15' : '#f8fafc'} depthTest={false} />
      </mesh>
      {edge && <mesh rotation={[Math.PI / 2, 0, 0]} renderOrder={83}>
        <torusGeometry args={[markerSize * 2, markerSize * 0.18, 6, 16]} />
        <meshBasicMaterial color="#facc15" depthTest={false} />
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
  const texture = useMemo(() => labelTexture(`${marker.node} · ${marker.status.replace('_', ' ')}`), [marker.node, marker.status]);
  useEffect(() => () => texture?.dispose(), [texture]);
  const color = selected ? '#fb923c' : marker.status === 'anchored' ? '#34d399' : marker.status === 'cs_projected' ? '#38bdf8' : '#facc15';
  const size = clamp(extent * 0.018, 0.035, 0.09);
  // Cap large 1-sigma regions at 25% of scan extent (with a 5 cm minimum cap) so they remain visible without swallowing the scan.
  const uncertaintyRadius = marker.uncertaintyM === null ? 0 : Math.min(marker.uncertaintyM, Math.max(0.05, extent * 0.25));
  const opacity = marker.status === 'cs_projected' ? 0.52 : 1;
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
    <sprite position={[0, size * 2.8, 0]} scale={[Math.max(0.7, extent * 0.28), Math.max(0.18, extent * 0.07), 1]} renderOrder={63}>
      <spriteMaterial map={texture ?? undefined} color={color} transparent depthTest={false} />
    </sprite>
  </group>;
}

function CameraController({ command, extent, onInteractionEnd }: { command: ViewCommand; extent: number; onInteractionEnd: (distance: number) => void }) {
  const { camera, gl } = useThree();
  const controls = useMemo(() => new OrbitControls(camera, gl.domElement), [camera, gl.domElement]);
  useEffect(() => {
    const limits = orbitDistanceLimits(extent);
    controls.enableDamping = true; controls.dampingFactor = 0.08; controls.enableRotate = true; controls.enableZoom = true; controls.enablePan = true; controls.screenSpacePanning = true; controls.rotateSpeed = 0.72; controls.zoomSpeed = 0.92; controls.panSpeed = 0.72; controls.minDistance = limits.minDistance; controls.maxDistance = limits.maxDistance; controls.touches.ONE = THREE.TOUCH.ROTATE; controls.touches.TWO = THREE.TOUCH.DOLLY_PAN; controls.target.set(0,0,0); controls.update();
    const handleStart = (): void => clientDiagnostics.info('SCAN_VIEW_GESTURE_START');
    const handleEnd = (): void => { const distance = controls.getDistance(); onInteractionEnd(distance); clientDiagnostics.info('SCAN_VIEW_GESTURE_END', { distance: Number(distance.toFixed(3)), x: Number(camera.position.x.toFixed(3)), y: Number(camera.position.y.toFixed(3)), z: Number(camera.position.z.toFixed(3)) }); };
    controls.addEventListener('start', handleStart); controls.addEventListener('end', handleEnd);
    return () => { controls.removeEventListener('start', handleStart); controls.removeEventListener('end', handleEnd); controls.dispose(); };
  }, [camera, controls, extent, onInteractionEnd]);
  useEffect(() => {
    const pose = cameraPoseForPreset(command.preset, extent);
    controls.target.set(pose.target[0], pose.target[1], pose.target[2]);
    camera.up.set(pose.up[0], pose.up[1], pose.up[2]);
    camera.position.set(pose.position[0], pose.position[1], pose.position[2]);
    camera.lookAt(controls.target); if (camera instanceof THREE.PerspectiveCamera) camera.updateProjectionMatrix(); controls.update(); onInteractionEnd(controls.getDistance());
  }, [camera, command.preset, command.sequence, controls, extent, onInteractionEnd]);
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

function SceneLayers({ prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights, selectedNode, onSensorSelect }: {
  prepared: PreparedScanScene;
  display: ScanDisplayOptions;
  meshPositions: Float32Array;
  wireframe: boolean;
  sensorMarkers: readonly SensorPlacementMarker[];
  sourceHighlights: SourceHighlightScene;
  selectedNode: SensorPlacementNode | null;
  onSensorSelect: (node: SensorPlacementNode) => void;
}) {
  const layers = useMemo(() => scanLayerPlan(prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights), [prepared, display, meshPositions, wireframe, sensorMarkers, sourceHighlights]);
  return <><color attach="background" args={['#07101a']} />{layers.map((layer) => <LayerView key={layer.id} layer={layer} extent={prepared.extent} selectedNode={selectedNode} onSensorSelect={onSensorSelect} />)}</>;
}

export function ScanPointCloudViewport({
  snapshot,
  viewCommand,
  display,
  mode,
  sensorMarkers = NO_SENSOR_MARKERS,
  selectedSensorNode,
  onSensorMarkerSelect,
  sensorGeometry = null,
  registrationArtifact = null,
  objectId = null,
  sessionId = null,
  localizationRuns = NO_LOCALIZATION_RUNS,
  currentScanId = null,
}: ScanPointCloudViewportProps) {
  const { locale } = useLocale();
  const t = useT();
  const prepared = useMemo(() => prepareScanScene(snapshot), [snapshot]);
  const candidateVolume = useMemo(() => solverCandidateVolume(snapshot), [snapshot]);
  const sourceGrid = useMemo(() => sourceVoxelGridForScan(snapshot), [snapshot]);
  const sourceHighlights = useMemo(() => objectId === null || sessionId === null
    ? { reason: null, primary: null, uncertaintyVoxels: [], secondary: [], voxelSizeMeters: 0, historyRuns: [], staleRuns: [] } as SourceHighlightScene
    : buildSourceHighlightScene(localizationRuns, { objectId, sessionId }, registrationArtifact, sourceGrid, { currentScanId }),
  [localizationRuns, objectId, registrationArtifact, currentScanId, sessionId, sourceGrid]);
  const cameraFitExtent = useMemo(() => scanViewFitExtent(
    prepared,
    sourceHighlights,
    candidateVolume,
    display.freeAir || display.unknownSpace,
  ), [candidateVolume, display.freeAir, display.unknownSpace, prepared, sourceHighlights]);
  const tsdfClient = useMemo(() => new SparseTsdfMeshClient(), []);
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
    let cancelled = false;
    tsdfClient.reset();
    setTsdfMesh(null);
    setTsdfError(null);
    if (snapshot.tsdfPointCount < MESH_MIN_POINTS) return () => { cancelled = true; };

    const started = performance.now();
    void tsdfClient.reconstruct(buildTsdfRequestArgs(snapshot)).then((result) => {
      if (cancelled) return;
      setTsdfMesh(result);
      clientDiagnostics.info('SCAN_TSDF_READY', {
        sourcePoints: result.sourcePointCount,
        certifiedPoints: snapshot.tsdfPointCount,
        integratedPoints: result.integratedPointCount,
        nodes: result.nodeCount,
        rawTriangles: result.rawTriangleCount,
        triangles: result.triangleCount,
        quarantinedTriangles: result.quarantinedTriangleCount,
        localHotspots: result.localNoiseHotspotCount,
        localHotspotTriangles: result.localNoiseHotspotTriangleCount,
        discardedTriangles: result.topologyDiscardedTriangleCount,
        nonManifoldEdges: result.nonManifoldEdgeCount,
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
    return () => { cancelled = true; };
  }, [snapshot, tsdfClient]);

  const meshPositions = useMemo(
    () => tsdfMesh === null
      ? new Float32Array(0)
      : toViewportPositions(tsdfMesh.trianglePositions, prepared.centerX, prepared.centerY, prepared.centerZ),
    [prepared.centerX, prepared.centerY, prepared.centerZ, tsdfMesh],
  );

  const fingerprint = displayFingerprint(display); const modeLabel = scanModeLabel(mode, locale);
  const handleInteractionEnd = useCallback((distance: number): void => { setCameraDistance(distance); setInteractionRevision((current) => current+1); }, []);
  const meshQuality = tsdfMesh === null ? null : describeMeshQuality(tsdfMesh, locale);
  const meshStatus = snapshot.tsdfPointCount < MESH_MIN_POINTS
    ? t('scan.badge.mesh_more')
    : tsdfError !== null
      ? t('scan.badge.mesh_unavailable')
      : meshQuality === null
        ? t('scan.badge.mesh_building')
        : t('scan.surfel_count', { label: meshQuality.label, count: formatNumber(locale, snapshot.tsdfPointCount) });
  const sourceReasonLabel = sourceReasonText(sourceHighlights.reason, t);

  return <div className="viewport-shell scan-viewport" aria-label={t('scan.aria_label')} data-scan-mode={mode ?? 'custom'} data-layer-fingerprint={fingerprint} data-camera-revision={interactionRevision} data-camera-distance={cameraDistance.toFixed(4)} data-source-highlight-reason={sourceHighlights.reason ?? undefined}>
    <Canvas className="scan-webgl-canvas" camera={{ position:[3.2,2.5,3.6], fov:50, near:0.01, far:120 }} dpr={[1,1.5]} gl={{ antialias:true, alpha:false, powerPreference:'high-performance' }} onContextMenu={(event) => event.preventDefault()}><SceneLayers prepared={prepared} display={display} meshPositions={meshPositions} wireframe={mode === 'mesh'} sensorMarkers={sensorMarkers} sourceHighlights={sourceHighlights} selectedNode={selectedNode} onSensorSelect={selectSensorNode} /><CameraController command={viewCommand} extent={cameraFitExtent} onInteractionEnd={handleInteractionEnd} /></Canvas>
    <div className="scan-source-badge">{scanSourceBadgeText(snapshot, locale)}</div>
    {candidateVolume.candidateDepthUnavailable && <div className="scan-source-candidate-depth" role="status">{t('scan.candidate_depth_unavailable')}</div>}
    {sourceReasonLabel !== null && <div className="scan-source-highlight-status" role="status">{sourceReasonLabel}</div>}
    {sourceHighlights.historyRuns.length > 0 && <div className="scan-source-history" aria-label={t('scan.source_history')}>
      {sourceHighlights.historyRuns.map(({ run, currency }) => <span key={`${run.method_name}-${run.computed_at_ms}`}>
        {run.method_name} · {t(currency === 'history_previous_scan' ? 'scan.source_history.previous_scan' : 'scan.source_history.scan_unknown')}
      </span>)}
    </div>}
    <div className="scan-mode-overlay">{modeLabel} · {fingerprint.toUpperCase()}</div>
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
    </section>}
    {sensorFrameScene !== null && <section className="scan-sensor-inset" aria-label={t('scan.unregistered_geometry')}>
      <div className="scan-sensor-inset-caption">{t('scan.unregistered_geometry')}</div>
      <SceneViewport scene={sensorFrameScene} selectedId={selectedNode ?? ''} viewCommand={viewCommand}
        onSelect={(node) => { const selected = sensorNode(node); if (selected !== null) selectSensorNode(selected); }} />
    </section>}
    <div className="axis-note">{t('scan.axis_note')}</div>
  </div>;
}
