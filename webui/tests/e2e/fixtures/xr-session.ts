import type { Page } from '@playwright/test';
import { FakeXrLayer, FakeXrSession } from '../../xr/support/xrFakes';

/** Simulates the device XR protocol only; the production runtime, Workers and WebGL still run. */
export async function installJourneyXr(page: Page, blendMode: 'opaque' | 'alpha-blend' | 'additive' = 'opaque') {
  await page.addInitScript({ content: `(() => {
    const FakeXrSession = ${FakeXrSession.toString()};
    const FakeXrLayer = ${FakeXrLayer.toString()};
    let session = null;
    let requests = 0;
    Object.defineProperty(navigator, 'xr', { configurable: true, value: {
      isSessionSupported: async () => true,
      requestSession: async () => {
        requests++;
        session = new FakeXrSession();
        Object.assign(session, { environmentBlendMode: ${JSON.stringify(blendMode)}, depthUsage: 'cpu-optimized', depthDataFormat: 'luminance-alpha', depthType: 'raw', depthActive: true });
        return session;
      }
    }});
    Object.assign(window, { XRWebGLLayer: FakeXrLayer, XRWebGLBinding: undefined,
      __journeyXr: {
        state: () => ({ requests, active: session?.active === true }),
        frame: (index) => {
          const matrix = new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,index * 0.025,0,0,1]);
          const position = { x: index * 0.025, y: 0, z: 0 };
          const orientation = { x: 0, y: 0, z: 0, w: 1 };
          const projectionMatrix = new Float32Array([1,0,0,0,0,1,0,0,0,0,-1.002,-1,0,0,-0.2002,0]);
          const transform = { matrix, position, orientation };
          const view = { projectionMatrix, transform };
          session.driveNextFrame(1000 + index * 250, {
            session, getViewerPose: () => ({ transform, views: [view] }),
            getDepthInformation: () => ({ width: 64, height: 48, getDepthInMeters: () => 1.5 })
          });
        }
      }
    });
    for (const prototype of [WebGLRenderingContext.prototype, WebGL2RenderingContext.prototype]) {
      Object.defineProperty(prototype, 'makeXRCompatible', { configurable: true, value: async () => {} });
    }
  })();` });
}

type JourneyXr = { state: () => { requests: number; active: boolean }; frame: (index: number) => void };
export async function driveJourneyXr(page: Page) {
  for (let index = 0; index < 24; index++) {
    await page.evaluate(i => (window as unknown as { __journeyXr: JourneyXr }).__journeyXr.frame(i), index);
    // Let the real fusion Workers finish between the synthetic device frames.
    await page.waitForTimeout(50);
  }
}

export function journeyXrState(page: Page) {
  return page.evaluate(() => (window as unknown as { __journeyXr: JourneyXr }).__journeyXr.state());
}
