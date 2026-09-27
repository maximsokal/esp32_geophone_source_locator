import { useEffect, useState } from 'react';

import { fetchLocalizationRuns, fetchScanInfo, loadSessionRegistration } from '../../api/deviceApi';
import type { LocalizationRunListing, ScanArtifactInfo } from '../../api/deviceApi';
import { buildRecordingEvidence } from '../../domain/recordingEvidence';
import type { RecordingEvidence } from '../../domain/recordingEvidence';
import { decodeRegistrationArtifact, type RegistrationArtifact } from '../../xr/fiducial/registrationArtifact';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatDate, formatDistance, formatNumber } from '../../i18n/format';

const POLICY_LABEL: Record<RecordingEvidence['policy'], I18nKey> = {
  unknown: 'evidence.policy.unknown',
  insufficient: 'evidence.policy.insufficient',
  planar_2d: 'evidence.policy.planar',
  capable_3d: 'evidence.policy.capable_3d',
};

const LOCALIZATION_LABEL: Record<RecordingEvidence['localization'], I18nKey> = {
  unknown: 'evidence.localization.unknown',
  not_calculated: 'evidence.localization.not_calculated',
  calculated: 'evidence.localization.calculated',
  failed: 'evidence.localization.failed',
};

function metres(value: number | null, locale: 'en' | 'uk'): string {
  return value === null ? '—' : formatDistance(locale, value);
}

function scanLabel(evidence: RecordingEvidence, locale: 'en' | 'uk', t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string): string {
  if (evidence.scan !== 'available') return t(`evidence.scan.${evidence.scan}` as 'evidence.scan.unknown' | 'evidence.scan.absent');
  const size = `${formatNumber(locale, evidence.scanBytes / 1024, { maximumFractionDigits: 0 })} KiB`;
  const occupancy = t(evidence.occupancy === 'available' ? 'evidence.occupancy_saved' : 'evidence.no_occupancy');
  const registration = t(evidence.scanRegistered ? 'evidence.registered' : 'evidence.not_registered');
  return t('evidence.scan.available', { size, occupancy, registration });
}

function anchorLabel(evidence: RecordingEvidence, t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string): string {
  if (evidence.scanRegistered) return t('evidence.anchor_valid_tier', { tier: evidence.registrationTier ?? 'unknown' });
  return evidence.registrationReason === null
    ? t('evidence.anchor_none')
    : t('evidence.anchor_invalid', { reason: evidence.registrationReason });
}

/** Pure view, rendered in tests without the network. */
export function RecordingEvidenceView({ evidence }: { evidence: RecordingEvidence }) {
  const t = useT();
  const { locale } = useLocale();
  return (
    <dl className="console-dl evidence-dl">
      <div><dt>{t('evidence.remote_nodes')}</dt><dd>{evidence.remoteCount === null ? t('evidence.policy.unknown') : t('report.remote_count', { count: formatNumber(locale, evidence.remoteCount) })}{evidence.remotes.length > 0 ? ` (${evidence.remotes.map((pod) => t('report.remote_node', { number: formatNumber(locale, pod) })).join(', ')})` : ''}</dd></div>
      <div><dt>{t('evidence.localization_policy')}</dt><dd>{t(POLICY_LABEL[evidence.policy])}</dd></div>
      <div><dt>{t('evidence.levelled_recording')}</dt><dd>{evidence.geometryAvailable ? `${formatNumber(locale, evidence.leveledRemotes)}/${formatNumber(locale, evidence.remotes.length)}` : '—'}</dd></div>
      <div><dt>{t('evidence.geometry_frozen')}</dt><dd>{evidence.geometryComputedAtMs === null ? t('evidence.not_frozen') : t('evidence.solved_at', { date: formatDate(locale, evidence.geometryComputedAtMs) })}</dd></div>
      <div><dt>{t('evidence.worst_cs_residual')}</dt><dd>{metres(evidence.worstResidualM, locale)}</dd></div>
      <div><dt>{t('evidence.worst_position_uncertainty')}</dt><dd>{metres(evidence.worstUncertaintyM, locale)}{evidence.undeterminedPods.length > 0 ? ` · ${t('evidence.undetermined', { nodes: evidence.undeterminedPods.join(', ') })}` : ''}</dd></div>
      <div><dt>{t('evidence.positions')}</dt><dd>{evidence.positions.length === 0 ? '—' : evidence.positions.map((p) =>
        `${p.pod === 0 ? 'MAIN' : t('report.remote_node', { number: formatNumber(locale, p.pod) })} ${formatNumber(locale, p.position.x_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}/${formatNumber(locale, p.position.y_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}/${p.position.z_m === null ? t('evidence.policy.unknown') : formatNumber(locale, p.position.z_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}`).join(' · ')}</dd></div>
      <div><dt>{t('evidence.scan')}</dt><dd>{scanLabel(evidence, locale, t)}</dd></div>
      <div><dt>{t('evidence.apriltag_anchor')}</dt><dd>{anchorLabel(evidence, t)}</dd></div>
      <div><dt>{t('evidence.localization')}</dt><dd>{t(LOCALIZATION_LABEL[evidence.localization])}{evidence.solvedMethods.length > 0 ? `: ${evidence.solvedMethods.join(', ')}` : ''}{evidence.failedMethods.length > 0 ? ` · ${t('evidence.failed_methods', { methods: evidence.failedMethods.join(', ') })}` : ''}</dd></div>
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
  const t = useT();
  const [listing, setListing] = useState<LocalizationRunListing | null>(null);
  const [scan, setScan] = useState<ScanArtifactInfo | null>(null);
  const [occupancy, setOccupancy] = useState<ScanArtifactInfo | null>(null);
  const [registration, setRegistration] = useState<RegistrationArtifact | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setListing(null);
    setScan(null);
    setOccupancy(null);
    setRegistration(null);
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
        if (cancelled) return;
        setOccupancy(occupancyInfo);
        const registrationJson = await loadSessionRegistration(objectId, sessionId);
        if (!cancelled) setRegistration(registrationJson === null ? null : decodeRegistrationArtifact(registrationJson));
      }
      catch (reason) {
        if (!cancelled) setError(reason instanceof Error ? reason.message : String(reason));
      }
    })();
    return () => { cancelled = true; };
  }, [objectId, sessionId, refreshKey]);

  const evidence = buildRecordingEvidence(listing, scan, occupancy, registration);
  return (
    <article className="console-card">
      <div className="card-heading">
        <div><span className="eyebrow">{t('evidence.recording', { session: sessionId })}</span><h2>{t('evidence.title')}</h2></div>
      </div>
      {error !== null && <p className="overlay-note">{t('evidence.partly_unavailable', { error })}</p>}
      <RecordingEvidenceView evidence={evidence} />
    </article>
  );
}
