import type { DeviceAcquisitionStatus } from '../../domain/device';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';

interface Props {
  acquisition: DeviceAcquisitionStatus | undefined;
  /** A control request is in flight. */
  busy: boolean;
  /** The source runs (live signals or a recording): the rate is fixed from start to stop. */
  locked: boolean;
  onSelect: (sampleRateHz: number) => void;
}

/**
 * The presets of the recording sample rate with the longest recording each can keep up with. The live signals and the
 * screen stay at a reduced rate; only the recording runs faster. Rendered only when the firmware offers presets.
 */
export default function SampleRateSelect({ acquisition, busy, locked, onSelect }: Props) {
  const t = useT();
  const { locale } = useLocale();
  const options = acquisition?.sample_rate_options ?? [];
  if (options.length === 0) return null;
  const selected = acquisition?.selected_sample_rate_hz ?? options[0].sample_rate_hz;
  return (
    <div className="sample-rate-select">
      <label>
        <span>{t('objects.rate_label')}</span>
      <select
        value={selected}
        disabled={busy || locked}
        onChange={(event) => onSelect(Number(event.target.value))}
      >
        {options.map((option) => (
          <option key={option.sample_rate_hz} value={option.sample_rate_hz}>
            {option.max_record_s === 0
              ? t('objects.rate_option_unlimited', { rate: formatNumber(locale, option.sample_rate_hz) })
              : t('objects.rate_option', {
                rate: formatNumber(locale, option.sample_rate_hz),
                seconds: formatNumber(locale, option.max_record_s),
              })}
          </option>
        ))}
      </select>
      </label>
      <small>{locked ? t('objects.rate_stop_first') : t('objects.rate_note')}</small>
    </div>
  );
}
