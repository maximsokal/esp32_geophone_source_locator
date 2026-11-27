import '../src/styles.css';
import '../src/scan.css';
import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';

import { ScanPointCloudViewport } from '../src/components/ScanPointCloudViewport';
import type { ViewCommand } from '../src/components/SceneViewport';
import { LocaleProvider } from '../src/i18n';
import { decodeScanArtifact } from '../src/xr/scanArtifact';
import { displayForScanMode } from '../src/xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../src/xr/webxrRuntime';

/*
 * Capture page for Before/After comparisons (Phase 2): the REAL viewer on a scan artifact handed in as bytes by the capture script, with a fixed
 * camera preset. It only uses viewer props that every compared commit has, so the same page runs unchanged at f40b0cec, 4ad30dc3 and the new HEAD.
 */
function base64ToBytes(value: string): Uint8Array {
  const raw = atob(value);
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
  return bytes;
}

function Harness() {
  const [snapshot, setSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [command, setCommand] = useState<ViewCommand>({ preset: 'perspective', sequence: 0 });
  useEffect(() => {
    (window as unknown as { __scanCompare: unknown }).__scanCompare = {
      load: (base64: string) => setSnapshot(decodeScanArtifact(base64ToBytes(base64))),
      preset: (preset: ViewCommand['preset']) => setCommand((current) => ({ preset, sequence: current.sequence + 1 })),
    };
  }, []);
  return (
    <main className="app-shell">
      <output id="scan-state" data-loaded={snapshot !== null}>{snapshot ? `${snapshot.pointCount} stable points` : 'waiting for a scan'}</output>
      <section className="workspace" style={{ gridTemplateColumns: '1fr' }}>
        {snapshot !== null && <ScanPointCloudViewport snapshot={snapshot} viewCommand={command} display={displayForScanMode('mesh')} mode="mesh" />}
      </section>
    </main>
  );
}

createRoot(document.getElementById('root')!).render(<LocaleProvider><Harness /></LocaleProvider>);
