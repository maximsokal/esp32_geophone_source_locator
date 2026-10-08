import type { DeviceStatus } from '../domain/device';
import { translate, useLocale } from '../i18n';
import type { I18nLocale } from '../i18n';

interface Props {
  status: DeviceStatus | null;
}

export function storageUnavailableBannerText(
  status: DeviceStatus | null,
  locale: I18nLocale = 'en',
): string | null {
  return status?.storage_available === false
    ? translate(locale, 'web.storage.unavailable_banner')
    : null;
}

export default function StorageUnavailableBanner({ status }: Props) {
  const { locale } = useLocale();
  const text = storageUnavailableBannerText(status, locale);
  if (text === null) return null;
  return <div className="console-alert warning" role="status">{text}</div>;
}
