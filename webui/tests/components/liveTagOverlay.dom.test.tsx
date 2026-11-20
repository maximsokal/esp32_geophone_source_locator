import { render, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { LiveTagOverlay } from '../../src/components/LiveTagOverlay';
import { LiveTagTracker } from '../../src/xr/fiducial/liveTagTracker';
import type { WebXrRuntime } from '../../src/xr/webxrRuntime';

/* The runtime is faked only at its projection boundary; the overlay, tracker and geometry are the real ones. */
function fakeRuntime() {
  const camera = { shiftU: 0, visible: true };
  const runtime = {
    projectProjectPointToNormalized: vi.fn((point: readonly [number, number, number]) => (camera.visible
      ? { u: 0.5 + point[0] * 10 + camera.shiftU, v: 0.5 - point[2] * 10, clipDepth: 0, visible: true } : null)),
  };
  return { camera, runtime: runtime as unknown as WebXrRuntime };
}

function anchored(tracker: LiveTagTracker, tagId = 2): void {
  tracker.ingest({
    tagId, corners: [[0.4, 0.6], [0.5, 0.6], [0.5, 0.5], [0.4, 0.5]],
    pose: { ok: true, positionScan: [0, 0, -1], rotationScan: [1, 0, 0, 0, 1, 0, 0, 0, 1], reprojectionErrorNorm: 0.001, distanceM: 1 },
    sizeM: 0.032, capturedAtMs: 1,
  });
}
const polygon = (container: HTMLElement, id: number) => container.querySelector(`polygon[data-tag-id="${id}"]`);

describe('LiveTagOverlay', () => {
  it('draws the anchored tag square with its device name, and follows the camera', async () => {
    const tracker = new LiveTagTracker();
    const { camera, runtime } = fakeRuntime();
    anchored(tracker);
    const { container } = render(<LiveTagOverlay runtime={runtime} tracker={tracker} active />);
    await waitFor(() => expect(polygon(container, 2)).not.toBeNull());
    // BL, BR, TR, TL of a 32 mm square 1 m ahead, through the fake projection.
    expect(polygon(container, 2)!.getAttribute('points')).toBe('34.00,66.00 66.00,66.00 66.00,34.00 34.00,34.00');
    expect(polygon(container, 2)!.getAttribute('data-style')).toBe('anchored');
    expect(container.querySelector('[data-tag-label="G2"]')!.textContent).toBe('G2');
    camera.shiftU = 0.1; // the phone moved: the same world square lands elsewhere on screen
    await waitFor(() => expect(polygon(container, 2)!.getAttribute('points')).toBe('44.00,66.00 76.00,66.00 76.00,34.00 44.00,34.00'));
  });

  it('shows confirmed and lost states with symbols only', async () => {
    const tracker = new LiveTagTracker({ staleMs: 100 });
    const { runtime } = fakeRuntime();
    anchored(tracker);
    const { container } = render(<LiveTagOverlay runtime={runtime} tracker={tracker} active />);
    await waitFor(() => expect(polygon(container, 2)).not.toBeNull());
    tracker.setConfirmed([2]);
    await waitFor(() => expect(polygon(container, 2)!.getAttribute('data-style')).toBe('confirmed'));
    expect(container.querySelector('[data-tag-label="G2"]')!.textContent).toBe('G2 ✓');
    tracker.tick(10_000);
    await waitFor(() => expect(polygon(container, 2)!.getAttribute('data-style')).toBe('lost'));
  });

  it('omits a tag whose corner is behind the camera and draws nothing while inactive', async () => {
    const tracker = new LiveTagTracker();
    const { camera, runtime } = fakeRuntime();
    anchored(tracker);
    camera.visible = false;
    const hidden = render(<LiveTagOverlay runtime={runtime} tracker={tracker} active />);
    await new Promise((resolve) => setTimeout(resolve, 120));
    expect(hidden.container.querySelector('polygon')).toBeNull();
    hidden.unmount();
    const second = fakeRuntime();
    const idle = render(<LiveTagOverlay runtime={second.runtime} tracker={tracker} active={false} />);
    await new Promise((resolve) => setTimeout(resolve, 120));
    expect(idle.container.querySelector('polygon')).toBeNull();
    expect(second.runtime.projectProjectPointToNormalized).not.toHaveBeenCalled();
  });

  it('stops its animation loop when unmounted or deactivated', async () => {
    const tracker = new LiveTagTracker();
    const { runtime } = fakeRuntime();
    anchored(tracker);
    const cancel = vi.spyOn(window, 'cancelAnimationFrame');
    const view = render(<LiveTagOverlay runtime={runtime} tracker={tracker} active />);
    await waitFor(() => expect(polygon(view.container, 2)).not.toBeNull());
    view.rerender(<LiveTagOverlay runtime={runtime} tracker={tracker} active={false} />);
    expect(view.container.querySelector('polygon')).toBeNull();
    expect(cancel).toHaveBeenCalled();
    const calls = (runtime.projectProjectPointToNormalized as unknown as ReturnType<typeof vi.fn>).mock.calls.length;
    await new Promise((resolve) => setTimeout(resolve, 150));
    expect((runtime.projectProjectPointToNormalized as unknown as ReturnType<typeof vi.fn>).mock.calls.length).toBe(calls);
    view.unmount();
  });
});
