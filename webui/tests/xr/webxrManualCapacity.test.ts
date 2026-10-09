import { afterEach, describe, expect, it, vi } from 'vitest';

import { loadUkrainianCatalog, translate } from '../../src/i18n';
import { authorizeCertifiedManualPlacement, MANUAL_ANNOTATION_CAPACITY_MESSAGE, MAX_MANUAL_CONTROL_POINTS, ManualControlPointStore } from '../../src/xr/manualControlPoints';
import { MAX_OPERATOR_EDGES, OperatorEdgeStore } from '../../src/xr/operatorEdges';
import { WebXrRuntime, type WebXrRuntimeState } from '../../src/xr/webxrRuntime';

vi.mock('../../src/xr/manualPlacementPreview', () => ({
  previewManualPlacementFromStableSurface: () => ({
    resolution: { accepted: true, position: [1000, 0, 0], mode: 'corner', confidence: 0.9 },
  }),
}));

interface RuntimeTestAccess {
  state: WebXrRuntimeState;
  session: object | null;
  evidenceFrozen: boolean;
  manualPoints: ManualControlPointStore;
  operatorEdges: OperatorEdgeStore;
}

const runtimes: WebXrRuntime[] = [];

function createRunningRuntime(): { runtime: WebXrRuntime; internals: RuntimeTestAccess } {
  const runtime = new WebXrRuntime();
  runtimes.push(runtime);
  const internals = runtime as unknown as RuntimeTestAccess;
  internals.state = { ...runtime.getState(), phase: 'running' };
  internals.session = {};
  internals.evidenceFrozen = false;
  return { runtime, internals };
}

afterEach(async () => {
  await Promise.all(runtimes.splice(0).map((runtime) => runtime.dispose()));
});

describe('WebXrRuntime manual annotation capacity', () => {
  it('publishes the pin-capacity key for overlay rendering after a direct manual add is refused', () => {
    const { runtime, internals } = createRunningRuntime();
    for (let index = 0; index < MAX_MANUAL_CONTROL_POINTS; index += 1) internals.manualPoints.add([index * 2, 0, 0]);
    authorizeCertifiedManualPlacement([1000, 0, 0], 0.9);

    expect(() => runtime.addSuggestedManualPoint([1000, 0, 0])).toThrow(MANUAL_ANNOTATION_CAPACITY_MESSAGE);
    expect(runtime.getState().manualPointErrorKey).toBe('xr.manual.pin_capacity_reached');
  });

  it('publishes the pin-capacity key when the normalized pointer path is refused', () => {
    const { runtime, internals } = createRunningRuntime();
    for (let index = 0; index < MAX_MANUAL_CONTROL_POINTS; index += 1) internals.manualPoints.add([index * 2, 0, 0]);
    authorizeCertifiedManualPlacement([1000, 0, 0], 0.9);

    runtime.queueManualPointAtNormalized(0.5, 0.5);

    expect(runtime.getState().manualPointError).toBe(MANUAL_ANNOTATION_CAPACITY_MESSAGE);
    expect(runtime.getState().manualPointErrorKey).toBe('xr.manual.pin_capacity_reached');
  });

  it('publishes the edge-capacity key with English and Ukrainian translations', async () => {
    const { runtime, internals } = createRunningRuntime();
    for (let index = 0; index < MAX_OPERATOR_EDGES; index += 1) {
      const start = index * 2;
      internals.operatorEdges.add([start, 0, 0], [start + 0.5, 0, 0]);
    }

    expect(runtime.addOperatorEdge([1000, 0, 0], [1000.5, 0, 0])).toBeNull();
    const key = 'xr.manual.edge_capacity_reached';
    expect(runtime.getState().manualPointErrorKey).toBe(key);
    await loadUkrainianCatalog();
    const english = translate('en', key);
    const ukrainian = translate('uk', key);
    expect(english.trim()).not.toBe('');
    expect(ukrainian.trim()).not.toBe('');
    expect(ukrainian).not.toBe(english);
  });

  it('does not report capacity when a degenerate edge is rejected at a full store', () => {
    const { runtime, internals } = createRunningRuntime();
    for (let index = 0; index < MAX_OPERATOR_EDGES; index += 1) {
      const start = index * 2;
      internals.operatorEdges.add([start, 0, 0], [start + 0.5, 0, 0]);
    }

    expect(runtime.addOperatorEdge([1000, 0, 0], [1000.01, 0, 0])).toBeNull();
    expect(runtime.getState().manualPointErrorKey).toBeNull();
  });
});
