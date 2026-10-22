import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useEffect } from 'react';
import * as THREE from 'three';
import { useOrbitControlsOwner, useOwnedOrbitControls, type OrbitControlsRef } from '../../components/useOrbitControls';

import { isPlanarSector, type LocalizationOverlayModel } from '../../domain/localizationOverlay';

/*
 * MAX-161: 3D scene for the method comparison. Sensor frame, metres:
 * x east-ish, y north-ish, z up (drawn as three.js y). Lazy chunk: three.js
 * must never reach the core console bundle.
 */

interface Props {
  model: LocalizationOverlayModel;
}

function toThree(x: number, y: number, z: number): [number, number, number] {
  return [x, z, -y];
}

function sceneExtent(model: LocalizationOverlayModel): number {
  let extent = 2;
  for (const s of model.sensors) {
    extent = Math.max(extent, Math.abs(s.position.x_m), Math.abs(s.position.y_m), Math.abs(s.position.z_m));
  }
  for (const s of model.sectors) {
    extent = Math.max(extent, Math.abs(s.position.x_m) + s.radiusM, Math.abs(s.position.y_m) + s.radiusM);
  }
  return extent;
}

function Controls({ extent, controlsRef }: { extent: number; controlsRef: OrbitControlsRef }) {
  const { camera } = useThree();
  const controls = useOwnedOrbitControls(controlsRef);
  useEffect(() => {
    controls.enableDamping = true;
    camera.position.set(extent * 1.4, extent * 1.2, extent * 1.4);
    controls.target.set(0, 0, 0);
    controls.update();
  }, [camera, controls, extent]);
  useFrame(() => controls.update());
  return null;
}

export default function LocalizationOverlayScene({ model }: Props) {
  const extent = sceneExtent(model);
  const controlsOwner = useOrbitControlsOwner();
  return (
    <Canvas className="localization-overlay-canvas" camera={{ fov: 45, near: 0.05, far: extent * 20 }}>
      <ambientLight intensity={0.7} />
      <directionalLight position={[5, 10, 5]} intensity={0.8} />
      <gridHelper args={[Math.ceil(extent * 2.5), Math.ceil(extent * 2.5), '#35546a', '#1c3444']} />
      <Controls extent={extent} controlsRef={controlsOwner} />

      {model.sensors.map((sensor) => (
        <mesh key={`sensor-${sensor.pod}`} position={toThree(sensor.position.x_m, sensor.position.y_m, sensor.position.z_m)}>
          <boxGeometry args={[0.18, 0.18, 0.18]} />
          <meshStandardMaterial color={sensor.pod === 0 ? '#e2e8f0' : '#94a3b8'} />
        </mesh>
      ))}

      {model.sectors.map((sector) => {
        const selected = sector.emphasis === 'selected';
        const planar = isPlanarSector(sector);
        /*
         * MAX-148: a 3-REMOTE / planar_2d run never solved its own height --
         * the backend reports a nominal Z (mean sensor Z) only so the record
         * has a position at all. Drawing it as a sphere at that Z would read
         * as a solved 3D point; a flat disc (thin cylinder, three.js Y axis
         * already matches "up") marks it as a 2D result instead.
         */
        return (
          <group key={sector.method} position={toThree(sector.position.x_m, sector.position.y_m, sector.position.z_m)}>
            <mesh>
              {planar
                ? <cylinderGeometry args={[sector.radiusM, sector.radiusM, 0.02, 32]} />
                : <sphereGeometry args={[sector.radiusM, 32, 16]} />}
              <meshStandardMaterial
                color={sector.color}
                transparent
                opacity={selected ? 0.55 : 0.12}
                emissive={sector.color}
                emissiveIntensity={selected ? 0.35 : 0.0}
                depthWrite={false}
                side={THREE.DoubleSide}
              />
            </mesh>
            {planar && (
              <mesh rotation={[Math.PI / 2, 0, 0]}>
                <ringGeometry args={[sector.radiusM * 0.97, sector.radiusM, 48]} />
                <meshBasicMaterial color={sector.color} transparent opacity={selected ? 0.9 : 0.3} side={THREE.DoubleSide} />
              </mesh>
            )}
            {/* Centre marker keeps overlapping sectors distinguishable. */}
            <mesh>
              <sphereGeometry args={[Math.min(0.06, sector.radiusM / 3), 12, 8]} />
              <meshBasicMaterial color={sector.color} transparent opacity={selected ? 1 : 0.4} />
            </mesh>
          </group>
        );
      })}

      {/* Credible regions of Bayesian runs (selected one vivid). */}
      {model.sectors.filter((sector) => sector.credibleBox !== null).map((sector) => {
        const box = sector.credibleBox!;
        const selected = sector.emphasis === 'selected';
        // Sensor-frame (x, y, z) sizes map to three.js (x, z, y).
        const size: [number, number, number] = [
          Math.max(box.size.x_m, 0.02), Math.max(box.size.z_m, 0.02), Math.max(box.size.y_m, 0.02),
        ];
        return (
          <lineSegments
            key={`region-${sector.method}`}
            position={toThree(box.center.x_m, box.center.y_m, box.center.z_m)}
          >
            <edgesGeometry args={[new THREE.BoxGeometry(size[0], size[1], size[2])]} />
            <lineBasicMaterial color={sector.color} transparent opacity={selected ? 0.9 : 0.25} />
          </lineSegments>
        );
      })}
    </Canvas>
  );
}
