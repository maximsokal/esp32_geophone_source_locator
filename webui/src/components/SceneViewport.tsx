import { Canvas, useFrame, useThree } from '@react-three/fiber';
import type { ThreeEvent } from '@react-three/fiber';
import { useEffect, useMemo } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import type { SceneModel, SensorPlacement, TriangleMesh, Vec3 } from '../domain/scene';

export type ViewPreset = 'perspective' | 'top' | 'front';

export interface ViewCommand {
  preset: ViewPreset;
  sequence: number;
}

interface SceneViewportProps {
  scene: SceneModel;
  selectedId: string;
  viewCommand: ViewCommand;
  onSelect: (sensorId: string) => void;
}

function toThree(position: Vec3): [number, number, number] {
  const [x, y, z] = position;
  return [x, z, -y];
}

function makeLabelTexture(label: string): THREE.CanvasTexture {
  const canvas = document.createElement('canvas');
  canvas.width = 256;
  canvas.height = 96;

  const context = canvas.getContext('2d');
  if (context !== null) {
    context.clearRect(0, 0, canvas.width, canvas.height);
    context.fillStyle = 'rgba(11, 18, 32, 0.92)';
    context.fillRect(12, 10, 232, 76);
    context.strokeStyle = 'rgba(148, 163, 184, 0.7)';
    context.lineWidth = 3;
    context.strokeRect(12, 10, 232, 76);
    context.fillStyle = '#f8fafc';
    context.font = '700 40px system-ui, sans-serif';
    context.textAlign = 'center';
    context.textBaseline = 'middle';
    context.fillText(label, 128, 50);
  }

  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.needsUpdate = true;
  return texture;
}

function CameraControls({ command }: { command: ViewCommand }) {
  const { camera, gl } = useThree();
  const controls = useMemo(
    () => new OrbitControls(camera, gl.domElement),
    [camera, gl.domElement],
  );

  useEffect(() => {
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = 1.2;
    controls.maxDistance = 24;
    controls.target.set(0, 0.55, 0);
    controls.update();

    return () => controls.dispose();
  }, [controls]);

  useEffect(() => {
    if (command.preset === 'top') {
      camera.up.set(0, 0, -1);
      camera.position.set(0, 11, 0.001);
    } else if (command.preset === 'front') {
      camera.up.set(0, 1, 0);
      camera.position.set(0, 3.2, 9.5);
    } else {
      camera.up.set(0, 1, 0);
      camera.position.set(7.2, 5.4, 7.8);
    }

    controls.target.set(0, 0.55, 0);
    camera.lookAt(controls.target);

    if (
      camera instanceof THREE.PerspectiveCamera ||
      camera instanceof THREE.OrthographicCamera
    ) {
      camera.updateProjectionMatrix();
    }

    controls.update();
  }, [camera, command, controls]);

  useFrame(() => controls.update());
  return null;
}

function Surface({ mesh }: { mesh: TriangleMesh }) {
  const geometry = useMemo(() => {
    const next = new THREE.BufferGeometry();
    const positions = new Float32Array(mesh.vertices.length * 3);

    mesh.vertices.forEach((vertex, index) => {
      const [x, y, z] = toThree(vertex);
      positions[index * 3] = x;
      positions[index * 3 + 1] = y;
      positions[index * 3 + 2] = z;
    });

    next.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    next.setIndex(mesh.indices);
    next.computeVertexNormals();
    return next;
  }, [mesh]);

  useEffect(() => () => geometry.dispose(), [geometry]);

  return (
    <group>
      <mesh geometry={geometry} receiveShadow>
        <meshStandardMaterial
          color="#475569"
          roughness={0.92}
          metalness={0.02}
          transparent
          opacity={0.78}
          side={THREE.DoubleSide}
        />
      </mesh>
      <mesh geometry={geometry} position={[0, 0.004, 0]}>
        <meshBasicMaterial
          color="#94a3b8"
          transparent
          opacity={0.16}
          wireframe
          side={THREE.DoubleSide}
        />
      </mesh>
    </group>
  );
}

function SensorMarker({
  sensor,
  selected,
  onSelect,
}: {
  sensor: SensorPlacement;
  selected: boolean;
  onSelect: (sensorId: string) => void;
}) {
  const texture = useMemo(() => makeLabelTexture(sensor.label), [sensor.label]);
  useEffect(() => () => texture.dispose(), [texture]);

  const [x, y, z] = toThree(sensor.position);
  const markerColor = selected
    ? '#fb923c'
    : sensor.role === 'main'
      ? '#38bdf8'
      : '#facc15';

  return (
    <group position={[x, y, z]}>
      <mesh
        position={[0, 0.11, 0]}
        castShadow
        onPointerDown={(event: ThreeEvent<PointerEvent>) => {
          event.stopPropagation();
          onSelect(sensor.id);
        }}
      >
        <cylinderGeometry args={[0.12, 0.16, 0.22, 24]} />
        <meshStandardMaterial color={markerColor} emissive={markerColor} emissiveIntensity={0.18} />
      </mesh>

      <mesh rotation={[Math.PI / 2, 0, 0]} position={[0, 0.012, 0]}>
        <torusGeometry args={[selected ? 0.28 : 0.22, 0.02, 10, 36]} />
        <meshBasicMaterial color={markerColor} />
      </mesh>

      <sprite position={[0, 0.52, 0]} scale={[0.82, 0.31, 1]}>
        <spriteMaterial map={texture} transparent depthTest={false} />
      </sprite>
    </group>
  );
}

function SceneContents({
  scene,
  selectedId,
  viewCommand,
  onSelect,
}: SceneViewportProps) {
  return (
    <>
      <color attach="background" args={['#08101d']} />
      <fog attach="fog" args={['#08101d', 14, 28]} />
      <ambientLight intensity={0.75} />
      <directionalLight position={[4, 8, 5]} intensity={1.5} castShadow />

      <gridHelper args={[12, 24, '#334155', '#1e293b']} position={[0, 0, 0]} />
      <axesHelper args={[1.4]} />

      {scene.surface !== undefined && <Surface mesh={scene.surface} />}

      {scene.sensors.map((sensor) => (
        <SensorMarker
          key={sensor.id}
          sensor={sensor}
          selected={sensor.id === selectedId}
          onSelect={onSelect}
        />
      ))}

      <CameraControls command={viewCommand} />
    </>
  );
}

export function SceneViewport(props: SceneViewportProps) {
  return (
    <div className="viewport-shell" aria-label="3D sensor placement view">
      <Canvas
        camera={{ position: [7.2, 5.4, 7.8], fov: 48, near: 0.05, far: 100 }}
        dpr={[1, 1.75]}
        shadows
      >
        <SceneContents {...props} />
      </Canvas>
      <div className="axis-note">X / Y = local horizontal · Z = height</div>
    </div>
  );
}
