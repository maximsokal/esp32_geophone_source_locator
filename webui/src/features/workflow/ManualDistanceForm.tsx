import { useId, useState } from 'react';
import { sendWorkflowCommand } from '../../api/deviceApi';
import type { DeviceWorkflowStatus, RangingPairStatus } from '../../domain/device';
import { useT } from '../../i18n';

interface Props {
  pair: RangingPairStatus;
  generation: number;
  startedAtMs: number;
  updatedAtMs: number;
  onSaved: (workflow: DeviceWorkflowStatus) => void;
  onRefresh: () => Promise<boolean>;
}

export default function ManualDistanceForm({ pair, generation, startedAtMs, updatedAtMs, onSaved, onRefresh }: Props) {
  const t = useT();
  const id = useId();
  const [open, setOpen] = useState(false);
  const [value, setValue] = useState(pair.distance_mm > 0 ? String(pair.distance_mm) : '');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const label = t('workflow.ranging.pair_distance', { a: pair.node_a, b: pair.node_b });
  if (!open) return <button type="button" className="secondary" onClick={() => setOpen(true)}
    aria-label={`${t('workflow.distance.edit')}: ${label}`}>{t('workflow.distance.edit')}</button>;

  const save = async (): Promise<void> => {
    const distance = value.trim() === '' ? NaN : Number(value);
    if (!Number.isSafeInteger(distance) || distance <= 0 || distance > 2147483647) {
      setError(t('workflow.distance.invalid'));
      return;
    }
    setPending(true);
    setError(null);
    try {
      const result = await sendWorkflowCommand('SET_DISTANCE', generation, {}, undefined, {
        node_a: pair.node_a, node_b: pair.node_b, distance_mm: distance,
        ranging_started_at_ms: startedAtMs, ranging_updated_at_ms: updatedAtMs,
      });
      if (result.ok) {
        onSaved(result.workflow);
        setOpen(false);
      } else {
        setError(t(result.reason === 'stale' ? 'workflow.state_changed' : 'workflow.distance.rejected'));
      }
      await onRefresh();
    } catch {
      setError(t('workflow.distance.rejected'));
    } finally {
      setPending(false);
    }
  };
  return <form className="console-form" onSubmit={event => { event.preventDefault(); void save(); }}>
    <label htmlFor={id}>{t('workflow.distance.value')} — {label}</label>
    <input id={id} type="number" inputMode="numeric" min="1" max="2147483647" step="1" required
      value={value} disabled={pending} onChange={event => setValue(event.target.value)} />
    <p>{t('workflow.distance.hint')}</p>
    {error !== null && <p role="alert">{error}</p>}
    <button type="submit" disabled={pending}>{t(pending ? 'workflow.distance.saving' : 'workflow.distance.save')}</button>
    <button type="button" className="secondary" disabled={pending} onClick={() => setOpen(false)}>{t('workflow.distance.cancel')}</button>
  </form>;
}
