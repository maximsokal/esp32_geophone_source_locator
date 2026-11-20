import { Suspense, lazy, useEffect, useMemo, useState } from 'react';
import '../../scan.css';
import {
  fetchLocalizationRuns,
  isRequestCancellation,
  loadObjectScanArtifact,
  loadSessionRegistration,
  loadSessionTagPositions,
  retryPreemptedRead,
  runIsPrimaryCandidate,
  type LocalizationRun,
  type LocalizationRunListing,
} from '../../api/deviceApi';
import type { ViewCommand } from '../../components/SceneViewport';
import { ViewportFailureBoundary } from '../../components/ViewportFailureBoundary';
import { clientDiagnostics } from '../../data/clientDiagnostics';
import { useT } from '../../i18n';
import {
  ScanDecodeClient,
  ScanDecodeCancelledError,
  SCAN_DECODE_TRANSIENT_BUDGET_BYTES,
} from '../../xr/scanDecodeClient';
import type { ScanPointCloudSnapshot } from '../../xr/webxrRuntime';
import { decodeRegistrationArtifact, type RegistrationArtifact } from '../../xr/fiducial/registrationArtifact';
import { tagLayoutMarkers, tagLayoutRegistration } from '../../xr/fiducial/tagLayoutScene';
import { decodeTagPositions, type TagPositionsArtifact } from '../../xr/fiducial/tagPositionsArtifact';
import { displayForScanMode } from '../../xr/scanDisplayModes';
import { registrationApplicability } from '../../xr/registrationApplicability';

const ScanPointCloudViewport = lazy(() =>
  import('../../components/ScanPointCloudViewport').then((module) => ({ default: module.ScanPointCloudViewport })));

const VIEW: ViewCommand = { preset: 'perspective', sequence: 0 };
const DISPLAY = displayForScanMode('mesh');

/** The staged scan of a placement that has no recording yet lives under this session id. */
export const STAGING_SESSION_ID = 'staging';

interface Props {
  objectId: string;
  /** The recording's session; null or empty before the recording exists, when the scan is still in staging. */
  sessionId: string | null;
  /** The calculation has run for this session (it can have a source to show). */
  finished: boolean;
}

interface Loaded {
  snapshot: ScanPointCloudSnapshot;
  scanId: string;
  tags: TagPositionsArtifact | null;
  runs: readonly LocalizationRun[];
  registration: RegistrationArtifact | null;
  /** A result exists but the scan has nothing to carry it into. */
  sourceUnplaced: boolean;
}

type State =
  | { phase: 'loading' }
  | { phase: 'none' }
  | { phase: 'failed' }
  | { phase: 'ready'; data: Loaded };

function latestSolvedRun(runs: readonly LocalizationRun[]): LocalizationRun | null {
  return runs
    .filter((run) => runIsPrimaryCandidate(run) && run.source_position !== undefined)
    .slice()
    .sort((left, right) => left.computed_at_ms - right.computed_at_ms)
    .at(-1) ?? null;
}

function geometryIsFromTagLayout(listing: LocalizationRunListing): boolean {
  const computed = listing.geometry.pods.filter((pod) => pod.computed);
  return computed.length > 0 && computed.every((pod) => pod.position_source === 'apriltag_layout');
}

/** Everything the view needs besides the scan itself; never throws, a missing piece only hides its part. */
async function loadCompanions(
  objectId: string, sessionId: string, scanId: string, finished: boolean,
): Promise<Pick<Loaded, 'tags' | 'runs' | 'registration' | 'sourceUnplaced'>> {
  let tags: TagPositionsArtifact | null = null;
  try {
    const text = await loadSessionTagPositions(objectId, sessionId);
    const decoded = text === null ? null : decodeTagPositions(text);
    /* Positions seen in another scan do not belong in this one. */
    tags = decoded !== null && decoded.scan_frame_id === scanId ? decoded : null;
  } catch (reason) {
    if (!isRequestCancellation(reason)) clientDiagnostics.warn('WORKFLOW_SCAN_TAGS_UNREADABLE', { objectId, sessionId, error: reason instanceof Error ? reason.message : String(reason) });
  }
  if (!finished) return { tags, runs: [], registration: null, sourceUnplaced: false };

  let listing: LocalizationRunListing | null = null;
  try {
    listing = await retryPreemptedRead(() => fetchLocalizationRuns(objectId, sessionId));
  } catch (reason) {
    if (!isRequestCancellation(reason)) clientDiagnostics.warn('WORKFLOW_SCAN_RUNS_UNREADABLE', { objectId, sessionId, error: reason instanceof Error ? reason.message : String(reason) });
  }
  const run = listing === null ? null : latestSolvedRun(listing.runs);
  if (listing === null || run === null) return { tags, runs: listing?.runs ?? [], registration: null, sourceUnplaced: false };

  /* Positions the calculation used: the tag layout's, carried by the translation to MAIN, or the Channel Sounding ones, carried by the saved registration. */
  let registration: RegistrationArtifact | null = null;
  if (tags !== null && geometryIsFromTagLayout(listing)) {
    registration = tagLayoutRegistration(tags, run.geometry_computed_at_ms);
  } else {
    try {
      const json = await loadSessionRegistration(objectId, sessionId);
      const saved = json === null ? null : decodeRegistrationArtifact(json);
      registration = registrationApplicability(saved, { geometryComputedAtMs: run.geometry_computed_at_ms, scanFrameId: scanId }).ok ? saved : null;
    } catch {
      registration = null;
    }
  }
  return { tags, runs: listing.runs, registration, sourceUnplaced: registration === null };
}

/**
 * The 3D scan of the placement on the recording page: the scan mesh, MAIN and the geophones where the tags put them, and, once the
 * calculation has run, the source of the seismic event. It reads what the phone already saved on the ESP32 and changes nothing.
 */
export default function WorkflowScanView({ objectId, sessionId, finished }: Props) {
  const t = useT();
  const [state, setState] = useState<State>({ phase: 'loading' });
  const effectiveSession = sessionId !== null && sessionId !== '' ? sessionId : STAGING_SESSION_ID;

  useEffect(() => {
    let cancelled = false;
    const decoder = new ScanDecodeClient();
    setState({ phase: 'loading' });
    void (async () => {
      try {
        const artifact = await loadObjectScanArtifact(objectId, effectiveSession);
        if (cancelled) return;
        if (artifact === null) { setState({ phase: 'none' }); return; }
        const decoded = await decoder.decode(artifact, { transferInput: true, budgetLimitBytes: SCAN_DECODE_TRANSIENT_BUDGET_BYTES });
        if (cancelled) return;
        if (decoded.snapshot.pointCount <= 0) { setState({ phase: 'none' }); return; }
        const scanId = decoded.identity.frameId;
        const companions = await loadCompanions(objectId, effectiveSession, scanId, finished);
        if (cancelled) return;
        setState({ phase: 'ready', data: { snapshot: decoded.snapshot, scanId, ...companions } });
        clientDiagnostics.info('WORKFLOW_SCAN_VIEW_READY', {
          objectId, sessionId: effectiveSession, stablePoints: decoded.snapshot.pointCount,
          tags: companions.tags?.nodes.length ?? 0, source: companions.registration !== null, finished,
        });
      } catch (reason) {
        if (cancelled || reason instanceof ScanDecodeCancelledError || isRequestCancellation(reason)) return;
        clientDiagnostics.warn('WORKFLOW_SCAN_VIEW_FAILED', { objectId, sessionId: effectiveSession, error: reason instanceof Error ? reason.message : String(reason) });
        setState({ phase: 'failed' });
      } finally {
        void clientDiagnostics.flush();
      }
    })();
    return () => { cancelled = true; decoder.dispose(); };
  }, [objectId, effectiveSession, finished]);

  const data = state.phase === 'ready' ? state.data : null;
  const markers = useMemo(() => data?.tags === null || data === null ? [] : tagLayoutMarkers(data.tags), [data]);

  if (state.phase === 'none') return null;
  if (state.phase === 'loading') return <p role="status">{t('workflow.scan3d.loading')}</p>;
  if (state.phase === 'failed') return <p role="alert">{t('workflow.scan3d.unavailable')}</p>;
  const loaded = state.data;
  const sourceNote = !finished ? t('workflow.scan3d.source_pending')
    : loaded.sourceUnplaced ? t('workflow.scan3d.source_unplaced')
      : loaded.registration !== null ? t('workflow.scan3d.source_shown') : null;

  return (
    <section className="workflow-scan3d" aria-label={t('workflow.scan3d.title')}>
      <h3>{t('workflow.scan3d.title')}</h3>
      <ViewportFailureBoundary
        key={loaded.scanId}
        diagnosticEvent="WORKFLOW_SCAN_VIEW_RENDERER_FAILED"
        renderFailure={() => <p role="alert">{t('workflow.scan3d.unavailable')}</p>}
      >
        <Suspense fallback={<p role="status">{t('workflow.scan3d.loading')}</p>}>
          <ScanPointCloudViewport
            snapshot={loaded.snapshot}
            viewCommand={VIEW}
            display={DISPLAY}
            mode="mesh"
            sensorMarkers={markers}
            registrationArtifact={loaded.registration}
            localizationRuns={loaded.runs}
            currentScanId={loaded.scanId}
            scanIdentityKey={`workflow:${loaded.scanId}`}
            objectId={objectId}
            sessionId={effectiveSession}
          />
        </Suspense>
      </ViewportFailureBoundary>
      <p>{loaded.tags === null ? t('workflow.scan3d.no_tags') : t('workflow.scan3d.sensors', { count: loaded.tags.nodes.length })}</p>
      {sourceNote !== null && <p role="status">{sourceNote}</p>}
    </section>
  );
}
