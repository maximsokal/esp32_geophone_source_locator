import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useEffect, useMemo } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import type { ViewCommand } from './SceneViewport';

interface ScanPointCloudViewportProps {
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
}

interface CloudFrame {
  center: THREE.Vector3;
  extent: number;
}

function buildGeometry(snapshot: ScanPointCloudSnapshot): THREE.BufferGeometry {
  const geometry = new THREE.BufferGeometry();
  const source = snapshot.positions;
  const positions = new Float32Array(source.length);

  for (let index = 0; index < source.length; index += 3) {
    const projectX = source[index];
    const projectY = source[index + 1];
    const projectZ = source[index + 2];

    // Project coordinates are X/Y horizontal + Z height.
    // Three.js uses Y-up.
    positions[index] = projectX;
    positions[index + 1] = projectZ;
    positions[index + 2] = -projectY;
  }

  geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geometry.computeBoundingBox();
  geometry.computeBoundingSphere();
  return geometry;
}

function cloudFrame(snapshot: ScanPointCloudSnapshot): CloudFrame {
  const bounds = snapshot.bounds;
  if (bounds === null) {
    return {
      center: new THREE.Vector3(),
      extent: 2,
    };
  }

  const centerX = (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = (bounds.min[2] + bounds.max[2]) * 0.5;

  const extentX = bounds.max[0] - bounds.min[0];
  const extentY = bounds.max[1] - bounds.min[1];
  const extentZ = bounds.max[2] - bounds.min[2];

  return {
    center: new THREE.Vector3(centerX, centerZ, -centerY),
    extent: Math.max(0.8, extentX, extentY, extentZ),
  };
}

function CameraControls({
  command,
  frame,
}: {
  command: ViewCommand;
  frame: CloudFrame;
}) {
  const { camera, gl } = useThree();
  const controls = useMemo(
    () => new OrbitControls(camera, gl.domElement),
    [camera, gl.domElement],
  );

  useEffect(() => {
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = Math.max(0.2, frame.extent * 0.08);
    controls.maxDistance = Math.max(12, frame.extent * 8);
    controls.target.copy(frame.center);
    controls.update();

    return () => controls.dispose();
  }, [controls, frame]);

  useEffect(() => {
    const distance = Math.max(1.6, frame.extent * 1.65);

    if (command.preset === 'top') {
      camera.up.set(0, 0, -1);
      camera.position.set(
        frame.center.x,
        frame.center.y + distance,
        frame.center.z + 0.001,
      );
    } else if (command.preset === 'front') {
      camera.up.set(0, 1, 0);
      camera.position.set(
        frame.center.x,
        frame.center.y + distance * 0.25,
        frame.center.z + distance,
      );
    } else {
      camera.up.set(0, 1, 0);
      camera.position.set(
        frame.center.x + distance * 0.72,
        frame.center.y + distance * 0.5,
        frame.center.z + distance * 0.78,
      );
    }

    controls.target.copy(frame.center);
    camera.lookAt(frame.center);

    if (
      camera instanceof THREE.PerspectiveCamera ||
      camera instanceof THREE.OrthographicCamera
    ) {
      camera.updateProjectionMatrix();
    }

    controls.update();
  }, [camera, command, controls, frame]);

  useFrame(() => controls.update());
  return null;
}

function PointCloud({ snapshot }: { snapshot: ScanPointCloudSnapshot }) {
  const geometry = useMemo(() => buildGeometry(snapshot), [snapshot]);
  useEffect(() => () => geometry.dispose(), [geometry]);

  const pointSize = Math.max(0.012, Math.min(0.045, snapshot.voxelSizeMeters * 0.7));

  return (
    <points geometry={geometry}>
      <pointsMaterial
        color="#67e8f9"
        size={pointSize}
        sizeAttenuation
        transparent
        opacity={0.92}
      />
    </points>
  );
}

function ScanScene({ snapshot, viewCommand }: ScanPointCloudViewportProps) {
  const frame = useMemo(() => cloudFrame(snapshot), [snapshot]);
  const gridSize = Math.max(4, Math.ceil(frame.extent * 1.5));

  return (
    <>
      <color attach="background" args={['#071019']} />
      <fog attach="fog" args={['#071019', Math.max(10, frame.extent * 3), Math.max(20, frame.extent * 7)]} />
      <ambientLight intensity={0.7} />
      <gridHelper
        args={[gridSize, Math.max(8, gridSize * 2), '#334155', '#172033']}
        position={[frame.center.x, 0, frame.center.z]}
      />
      <axesHelper args={[Math.max(0.5, frame.extent * 0.18)]} />
      <PointCloud snapshot={snapshot} />
      <CameraControls command={viewCommand} frame={frame} />
    </>
  );
}

export function ScanPointCloudViewport(props: ScanPointCloudViewportProps) {
  return (
    <div className="viewport-shell scan-viewport" aria-label="Measured WebXR depth point cloud">
      <Canvas camera={{ fov: 50, near: 0.02, far: 120 }} dpr={[1, 1.6]}>
        <ScanScene {...props} />
      </Canvas>
      <div className="scan-source-badge">REAL WEBXR DEPTH · {props.snapshot.pointCount.toLocaleString()} POINTS</div>
      <div className="axis-note">X / Y = local horizontal · Z = height · drag to orbit</div>
    </div>
  );
}
