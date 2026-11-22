/**
 * Hands bytes to the Phone Lab dev server (webui/scripts/phone-lab), which keeps them next to the run's other saves for replay tests.
 * `window.__phoneLab.capture` exists only when that dev server injected its telemetry script; in the production bundle (served by the ESP32) it is
 * absent, so every function here does nothing and costs nothing.
 */
interface PhoneLabHook { capture?: (name: string, bytes: Uint8Array) => void }

function hook(): PhoneLabHook | null {
  const lab = (globalThis as { __phoneLab?: PhoneLabHook }).__phoneLab;
  return lab !== undefined && typeof lab.capture === 'function' ? lab : null;
}

export function phoneLabCaptureAvailable(): boolean {
  return hook() !== null;
}

/** Never throws: a capture must not break the scan flow. */
export function capturePhoneLabBytes(name: string, produce: () => Uint8Array): void {
  const lab = hook();
  if (lab === null) return;
  try { lab.capture?.(name, produce()); } catch { /* capture is best effort */ }
}
