import { useThree } from '@react-three/fiber';
import { useLayoutEffect, useMemo, useRef, type RefObject } from 'react';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

export type OrbitControlsRef = RefObject<OrbitControls | null>;

export function useOrbitControlsOwner() {
  const controls = useRef<OrbitControls | null>(null);
  useLayoutEffect(() => () => {
    // R3F delays scene disposal; Three must disconnect from the original DOM root.
    controls.current?.dispose(); controls.current = null;
  }, []);
  return controls;
}

export function useOwnedOrbitControls(owner: OrbitControlsRef) {
  const { camera, gl } = useThree();
  const controls = useMemo(() => new OrbitControls(camera, gl.domElement), [camera, gl.domElement]);
  useLayoutEffect(() => {
    owner.current = controls;
    return () => {
      if (owner.current === controls) { controls.dispose(); owner.current = null; }
    };
  }, [controls, owner]);
  return controls;
}
