import { useEffect, useState } from 'react';

import { fetchLocalizationRuns, fetchScanInfo } from '../../api/deviceApi';
import type { LocalizationRunListing, ScanArtifactInfo } from '../../api/deviceApi';
import { buildRecordingEvidence } from '../../domain/recordingEvidence';
import type { RecordingEvidence } from '../../domain/recordingEvidence';

const POLICY_LABEL: Record<RecordingEvidence['policy'], string> = {
  unknown: 'unknown',
  insufficient: 'insufficient (< 3 REMOTEs)',
  planar_2d: 'planar only (3 REMOTEs)',
  capable_3d: '3D-capable (4 REMOTEs)',
};

const LOCALIZATION_LABEL: Record<RecordingEvidence['localization'], string> = {
  unknown: 'unknown',
  not_calculated: 'not calculated',
  calculated: 'calculated',
  failed: 'failed (no method solved)',
};

function metres(value: number | null): string {
  return value === null ? '—' : `${value.toFixed(2)} m`;
}

function scanLabel(evidence: RecordingEvidence): string {
  if (evidence.scan === 'unknown') return 'unknown';
  if (evidence.scan === 'absent') return 'absent';
  const size = `${(evidence.scanBytes / 1024).toFixed(0)} KiB`;
  const occupancy = evidence.occupancy === 'available' ? ', occupancy artifact saved' : ', no occupancy artifact';
  return `available (${size}${occupancy}), not registered to the sensors`;
}

/** Pure view, rendered in tests without the network. */
export function RecordingEvidenceView({ evidence }: { evidence: RecordingEvidence }) {
  return (
    <dl className="console-dl evidence-dl">
      <div><dt>REMOTE nodes</dt><dd>{evidence.remoteSummary}{evidence.remotes.length > 0 ? ` (Pod ${evidence.remotes.join(', ')})` : ''}</dd></div>
      <div><dt>Localization policy</dt><dd>{POLICY_LABEL[evidence.policy]}</dd></div>
      <div><dt>Levelled at record time</dt><dd>{evidence.geometryAvailable ? `${evidence.leveledRemotes}/${evidence.remotes.length}` : '—'}</dd></div>
      <div><dt>Geometry (frozen with the recording)</dt><dd>{evidence.geometryComputedAtMs === null ? 'not frozen' : `solved at ${new Date(evidence.geometryComputedAtMs).toLocaleString()}`}</dd></div>
      <div><dt>Worst CS residual</dt><dd>{metres(evidence.worstResidualM)}</dd></div>
      <div><dt>Worst position uncertainty</dt><dd>{metres(evidence.worstUncertaintyM)}{evidence.undeterminedPods.length > 0 ? ` · undetermined: Pod ${evidence.undeterminedPods.join(', ')}` : ''}</dd></div>
      <div><dt>Positions</dt><dd>{evidence.positions.length === 0 ? '—' : evidence.positions.map((p) =>
        `${p.label} ${p.position.x_m.toFixed(1)}/${p.position.y_m.toFixed(1)}/${p.position.z_m.toFixed(1)}`).join(' · ')}</dd></div>
      <div><dt>3D scan</dt><dd>{scanLabel(evidence)}</dd></div>
      <div><dt>AprilTag/ArUco anchor</dt><dd>not available yet (MAX-145)</dd></div>
      <div><dt>Localization</dt><dd>{LOCALIZATION_LABEL[evidence.localization]}{evidence.solvedMethods.length > 0 ? `: ${evidence.solvedMethods.join(', ')}` : ''}{evidence.failedMethods.length > 0 ? ` · failed: ${evidence.failedMethods.join(', ')}` : ''}</dd></div>
    </dl>
  );
}

interface Props {
  objectId: string;
  sessionId: string;
  /** Changes when the operator returns from the scanner or a calculation. */
  refreshKey: number;
}

/**
 * MAX-169: evidence of the OPENED recording only -- its own frozen
 * geometry, its own scan, its own localization runs.
 */
export default function RecordingEvidenceCard({ objectId, sessionId, refreshKey }: Props) {
  const [listing, setListing] = useState<LocalizationRunListing | null>(null);
  const [scan, setScan] = useState<ScanArtifactInfo | null>(null);
  const [occupancy, setOccupancy] = useState<ScanArtifactInfo | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setListing(null);
    setScan(null);
    setOccupancy(null);
    setError(null);
    /* Sequential on purpose: the ESP serves one HTTPS request at a time. */
    void (async () => {
      try {
        const runs = await fetchLocalizationRuns(objectId, sessionId);
        if (cancelled) return;
        setListing(runs);
        const scanInfo = await fetchScanInfo(objectId, sessionId, 'scan');
        if (cancelled) return;
        setScan(scanInfo);
        const occupancyInfo = await fetchScanInfo(objectId, sessionId, 'occupancy');
        if (!cancelled) setOccupancy(occupancyInfo);
      }
      catch (reason) {
        if (!cancelled) setError(reason instanceof Error ? reason.message : String(reason));
      }
    })();
    return () => { cancelled = true; };
  }, [objectId, sessionId, refreshKey]);

  const evidence = buildRecordingEvidence(listing, scan, occupancy);
  return (
    <article className="console-card">
      <div className="card-heading">
        <div><span className="eyebrow">RECORDING {sessionId}</span><h2>Evidence</h2></div>
      </div>
      {error !== null && <p className="overlay-note">Partly unavailable: {error}</p>}
      <RecordingEvidenceView evidence={evidence} />
    </article>
  );
}
