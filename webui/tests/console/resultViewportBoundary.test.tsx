import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement, type ReactNode } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import { ScanPointCloudViewport } from '../../src/components/ScanPointCloudViewport';
import { LocaleProvider, translate } from '../../src/i18n';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';
import {
  resultViewportIdentityKey,
  VIEWPORT_FAILURE_MAX_ATTEMPTS,
  ViewportFailureBoundary,
} from '../../src/components/ViewportFailureBoundary';

vi.mock('@react-three/fiber', async () => {
  const React = await import('react');
  const camera = {
    position: { x: 0, y: 0, z: 0, set: vi.fn() },
    up: { set: vi.fn() },
    lookAt: vi.fn(),
  };
  const domElement = {};
  return {
    Canvas: ({ children, className }: { children: ReactNode; className?: string }) =>
      React.createElement('div', { className, 'data-testid': 'mock-canvas' }, children),
    useFrame: () => undefined,
    useThree: () => ({ camera, gl: { domElement } }),
  };
});

vi.mock('three/addons/controls/OrbitControls.js', () => ({
  OrbitControls: class {
    enableDamping = false;
    dampingFactor = 0;
    enableRotate = false;
    enableZoom = false;
    enablePan = false;
    screenSpacePanning = false;
    rotateSpeed = 0;
    zoomSpeed = 0;
    panSpeed = 0;
    minDistance = 0;
    maxDistance = 0;
    touches: Record<string, unknown> = {};
    mouseButtons: Record<string, unknown> = {};
    target = { set: vi.fn() };
    addEventListener(): void {}
    removeEventListener(): void {}
    update(): void {}
    getDistance(): number { return 1; }
    dispose(): void {}
  },
}));

const mounted: ReactTestRenderer[] = [];

function textOf(node: ReactTestInstance | string): string {
  return typeof node === 'string' ? node : node.children.map(textOf).join('');
}

function button(renderer: ReactTestRenderer, label = 'Retry'): ReactTestInstance {
  const result = renderer.root.findAllByType('button').find((candidate) => textOf(candidate) === label);
  if (result === undefined) throw new Error(`Expected button: ${label}`);
  return result;
}

function failureUi({ message, attempts, retry }: { message: string; attempts: number; retry: () => void }) {
  return createElement('div', { role: 'alert' },
    createElement('span', null, `Viewport failed: ${message}`),
    attempts < VIEWPORT_FAILURE_MAX_ATTEMPTS
      ? createElement('button', { type: 'button', onClick: retry }, 'Retry')
      : null,
  );
}

function boundary(key: string, child: ReactNode, props: { onFailure?: (failure: { message: string; attempts: number }) => void } = {}) {
  return createElement(ViewportFailureBoundary, {
    key,
    renderFailure: failureUi,
    diagnosticEvent: 'SCAN_RESULT_RENDERER_LOAD_FAILED',
    onFailure: props.onFailure,
  }, child);
}

function mount(element: ReactNode): ReactTestRenderer {
  let renderer!: ReactTestRenderer;
  act(() => { renderer = create(element); });
  mounted.push(renderer);
  return renderer;
}

afterEach(() => {
  for (const renderer of [...mounted]) {
    act(() => renderer.unmount());
    mounted.splice(mounted.indexOf(renderer), 1);
  }
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

beforeEach(() => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.spyOn(console, 'error').mockImplementation(() => undefined);
});

describe('result viewport failure boundary', () => {
  it('builds the App viewport key from canonical identity or provisional generation, never snapshot counts', async () => {
    const appModule = await import('../../src/App');
    expect(appModule.resultViewportKey).toBeTypeOf('function');

    const scanA = {
      objectId: 'OBJ_0001', pointCount: 80, depthFrameCount: 12, durableScanId: 'scan-crc32:aaaaaaaa',
    };
    const scanB = {
      objectId: 'OBJ_0001', pointCount: 80, depthFrameCount: 12, durableScanId: 'scan-crc32:bbbbbbbb',
    };
    expect(appModule.resultViewportKey(scanA, 7)).not.toBe(appModule.resultViewportKey(scanB, 7));

    const provisionalA = { objectId: 'OBJ_0001', pointCount: 80, depthFrameCount: 12, durableScanId: null };
    const provisionalB = { ...provisionalA, pointCount: 4, depthFrameCount: 2 };
    expect(appModule.resultViewportKey(provisionalA, 8)).toBe('OBJ_0001:provisional-generation:8');
    expect(appModule.resultViewportKey(provisionalB, 8)).toBe(appModule.resultViewportKey(provisionalA, 8));
    expect(appModule.resultViewportKey(provisionalA, 9)).not.toBe(appModule.resultViewportKey(provisionalA, 8));
  });

  it('keeps the production viewport shell and reports a thrown localization overlay locally', () => {
    vi.stubGlobal('document', {
      createElement: () => { throw new Error('localization overlay construction failed'); },
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
    });
    const marker = {
      node: 'G1', status: 'anchored', source: 'channel_sounding', positionScanM: [0.5, 0.2, 0.1],
      uncertaintyM: 0.01, residualM: 0.002, tagId: null, details: 'test marker',
    } as const;
    const element = createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScanPointCloudViewport, {
      snapshot: makeValidScanSnapshot(),
      viewCommand: { preset: 'perspective', sequence: 0 },
      display: { goodPoints: true, badPoints: false, freeAir: false, unknownSpace: false, manualPoints: false, dimensions: false, mesh: false },
      mode: 'quality',
      sensorMarkers: [marker],
      scanIdentityKey: 'OBJ_0001:scan-crc32:aaaaaaaa',
    }));
    const renderer = mount(element);

    expect(renderer.root.findByProps({ className: 'viewport-shell scan-viewport' })).toBeDefined();
    expect(renderer.root.findByProps({ 'data-testid': 'mock-canvas' })).toBeDefined();
    const alert = renderer.root.findByProps({ role: 'alert' });
    expect(textOf(alert)).toContain(translate('en', 'scan.overlay.failed'));
    expect(textOf(alert)).toContain('localization overlay construction failed');
  });

  it('resets only when the canonical scan identity changes, even when snapshot counts match', () => {
    const scanA = { identity: 'scan-crc32:aaaaaaaa', objectId: 'OBJ_0001', pointCount: 80, depthFrameCount: 12 };
    const scanB = { identity: 'scan-crc32:bbbbbbbb', objectId: 'OBJ_0001', pointCount: 80, depthFrameCount: 12 };
    expect(scanB).toMatchObject({ objectId: scanA.objectId, pointCount: scanA.pointCount, depthFrameCount: scanA.depthFrameCount });
    const keyA = resultViewportIdentityKey(scanA.identity, 7);
    const keyB = resultViewportIdentityKey(scanB.identity, 7);
    expect(keyA).not.toBe(keyB);
    expect(resultViewportIdentityKey(null, 8)).toBe('provisional-generation:8');

    let failA = true;
    function Renderer({ scan }: { scan: typeof scanA }) {
      if (scan.identity === scanA.identity && failA) throw new Error('scan A renderer failure');
      return createElement('div', { 'data-scan-id': scan.identity }, scan.identity);
    }

    const renderer = mount(boundary(keyA, createElement(Renderer, { scan: scanA })));
    expect(textOf(renderer.root.findByProps({ role: 'alert' }))).toContain('scan A renderer failure');

    failA = false;
    act(() => renderer.update(boundary(keyA, createElement(Renderer, { scan: scanB }))));
    expect(renderer.root.findByProps({ role: 'alert' })).toBeDefined();
    expect(renderer.root.findAllByProps({ 'data-scan-id': scanB.identity })).toHaveLength(0);

    act(() => renderer.update(boundary(keyB, createElement(Renderer, { scan: scanB }))));
    expect(renderer.root.findByProps({ 'data-scan-id': scanB.identity })).toBeDefined();
    expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  });

  it('keeps the base scan visible when only its overlay fails and retries the overlay in place', () => {
    const identityKey = resultViewportIdentityKey('scan-crc32:cccccccc', 1);
    let overlayThrows = true;
    function Overlay() {
      if (overlayThrows) throw new Error('source overlay failed');
      return createElement('span', { 'data-testid': 'overlay-ready' }, 'Source overlay');
    }
    const tree = () => createElement('main', null,
      createElement('div', { 'data-testid': 'base-scan' }, 'Base scan'),
      boundary(identityKey, createElement(Overlay)),
    );

    const renderer = mount(tree());
    expect(renderer.root.findByProps({ 'data-testid': 'base-scan' })).toBeDefined();
    expect(textOf(renderer.root.findByProps({ role: 'alert' }))).toContain('source overlay failed');

    overlayThrows = false;
    act(() => { button(renderer).props.onClick(); });
    expect(renderer.root.findByProps({ 'data-testid': 'base-scan' })).toBeDefined();
    expect(renderer.root.findByProps({ 'data-testid': 'overlay-ready' })).toBeDefined();
    expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);
  });

  it('recovers from a transient renderer construction error after explicit Retry', () => {
    let constructionAttempts = 0;
    let allowRender = false;
    const failureAttempts: number[] = [];
    function Renderer() {
      constructionAttempts += 1;
      if (!allowRender) throw new Error('temporary construction failure');
      return createElement('div', null, 'Renderer ready');
    }

    const key = resultViewportIdentityKey('scan-crc32:dddddddd', 2);
    const child = () => boundary(key, createElement(Renderer), { onFailure: ({ attempts }) => failureAttempts.push(attempts) });
    const renderer = mount(child());
    expect(textOf(renderer.root.findByProps({ role: 'alert' }))).toContain('temporary construction failure');
    const failedConstructionAttempts = constructionAttempts;
    allowRender = true;
    act(() => { button(renderer).props.onClick(); });
    expect(constructionAttempts).toBeGreaterThan(failedConstructionAttempts);
    expect(textOf(renderer.root)).toContain('Renderer ready');
    expect(renderer.root.findAllByProps({ role: 'alert' })).toHaveLength(0);

    allowRender = false;
    act(() => renderer.update(child()));
    expect(failureAttempts).toEqual([1, 1]);
  });

  it('stops after three consecutive failures without timers or repeated diagnostics on rerender', () => {
    vi.useFakeTimers();
    const diagnostic = vi.spyOn(clientDiagnostics, 'warn');
    let renderAttempts = 0;
    const observedFailures: number[] = [];
    function AlwaysFails() {
      renderAttempts += 1;
      throw new Error('deterministic failure');
    }
    const key = resultViewportIdentityKey('scan-crc32:eeeeeeee', 3);
    const render = () => boundary(key, createElement(AlwaysFails), {
      onFailure: ({ attempts }) => observedFailures.push(attempts),
    });
    const renderer = mount(render());

    for (let failure = 1; failure < VIEWPORT_FAILURE_MAX_ATTEMPTS; failure += 1) {
      expect(renderer.root.findAllByType('button')).toHaveLength(1);
      act(() => { button(renderer).props.onClick(); });
    }

    expect(observedFailures).toEqual([1, 2, 3]);
    // React's test renderer may replay each failed function render while recovering the boundary.
    expect(renderAttempts).toBeLessThanOrEqual(VIEWPORT_FAILURE_MAX_ATTEMPTS * 2 + 1);
    expect(textOf(renderer.root.findByProps({ role: 'alert' }))).toContain('deterministic failure');
    expect(renderer.root.findAllByType('button')).toHaveLength(0);
    expect(diagnostic).toHaveBeenCalledTimes(VIEWPORT_FAILURE_MAX_ATTEMPTS);
    const attemptsAtLimit = renderAttempts;
    act(() => renderer.update(render()));
    expect(renderAttempts).toBe(attemptsAtLimit);
    expect(diagnostic).toHaveBeenCalledTimes(VIEWPORT_FAILURE_MAX_ATTEMPTS);
    expect(vi.getTimerCount()).toBe(0);
  });
});
