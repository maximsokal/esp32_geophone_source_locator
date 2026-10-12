import { Component, Suspense, lazy, useCallback, useEffect, useMemo, useRef, useState, type ComponentType, type ErrorInfo, type ReactNode } from 'react';
import { useT } from '../../i18n';
import type { DeviceStatus } from '../../domain/device';

export const SCANNER_MAX_IN_PLACE_RETRIES = 3;
export const SCANNER_RELOAD_SESSION_KEY = 'geolocator.scanner.chunkReloadAttempted';

export interface ScannerFeatureProps {
  objectId: string | null;
  sessionId: string | null;
  status: DeviceStatus | null;
  onScanSaved?: () => void | Promise<void>;
}

export interface ScannerSessionStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export type ScannerFeatureLoader = () => Promise<{ default: ComponentType<ScannerFeatureProps> }>;

export interface ScannerFeatureHostProps extends ScannerFeatureProps {
  onBack: () => void;
  onDashboard: () => void;
  loadScanner?: ScannerFeatureLoader;
  reloadPage?: () => void;
  sessionStorage?: ScannerSessionStorage;
}

const loadScannerModule: ScannerFeatureLoader = () => import('../../App');

function getSessionStorage(injected?: ScannerSessionStorage): ScannerSessionStorage | undefined {
  if (injected !== undefined) return injected;
  try {
    return typeof window === 'undefined' ? undefined : window.sessionStorage;
  }
  catch {
    return undefined;
  }
}

function reloadWasAttempted(storage: ScannerSessionStorage | undefined): boolean {
  try {
    return storage?.getItem(SCANNER_RELOAD_SESSION_KEY) === '1';
  }
  catch {
    return false;
  }
}

function clearReloadGuard(storage: ScannerSessionStorage | undefined): void {
  try {
    storage?.removeItem(SCANNER_RELOAD_SESSION_KEY);
  }
  catch {
    /* Private browsing can deny session storage; scanner loading still continues. */
  }
}

function ScannerLoadedNotice({ onLoaded, children }: { onLoaded: () => void; children: ReactNode }) {
  useEffect(onLoaded, [onLoaded]);
  return children;
}

class ScannerLoadBoundary extends Component<
  { children: ReactNode; fallback: ReactNode },
  { failed: boolean }
> {
  state = { failed: false };

  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }

  componentDidCatch(_error: unknown, _info: ErrorInfo): void {
    /* Chunk URLs and loader errors can contain private deployment details. */
  }

  render(): ReactNode {
    return this.state.failed ? this.props.fallback : this.props.children;
  }
}

export default function ScannerFeatureHost({
  objectId,
  sessionId,
  status,
  onScanSaved,
  onBack,
  onDashboard,
  loadScanner = loadScannerModule,
  reloadPage,
  sessionStorage: injectedSessionStorage,
}: ScannerFeatureHostProps) {
  const t = useT();
  const storageRef = useRef<ScannerSessionStorage | undefined>(undefined);
  const storageInitialized = useRef(false);
  if (!storageInitialized.current) {
    storageRef.current = getSessionStorage(injectedSessionStorage);
    storageInitialized.current = true;
  }
  const [attempt, setAttempt] = useState(0);
  const [reloadAttempted, setReloadAttempted] = useState(() => reloadWasAttempted(storageRef.current));
  const reloadAttemptedRef = useRef(reloadAttempted);
  reloadAttemptedRef.current = reloadAttempted;
  const onScannerLoaded = useCallback(() => {
    clearReloadGuard(storageRef.current);
    reloadAttemptedRef.current = false;
    setReloadAttempted(false);
  }, []);

  const ScannerFeature = useMemo(() => lazy(async () => {
    const module = await loadScanner();
    const LoadedScanner = (props: ScannerFeatureProps) => (
      <ScannerLoadedNotice onLoaded={onScannerLoaded}>
        <module.default {...props} />
      </ScannerLoadedNotice>
    );
    return { default: LoadedScanner };
  }), [attempt, loadScanner, onScannerLoaded]);

  const retry = (): void => {
    setAttempt((current) => Math.min(current + 1, SCANNER_MAX_IN_PLACE_RETRIES));
  };

  const reload = (): void => {
    if (reloadAttemptedRef.current) return;
    reloadAttemptedRef.current = true;
    try {
      storageRef.current?.setItem(SCANNER_RELOAD_SESSION_KEY, '1');
    }
    catch {
      /* Keep the one-reload guard in React state when session storage is unavailable. */
    }
    setReloadAttempted(true);
    if (reloadPage !== undefined) reloadPage();
    else if (typeof window !== 'undefined') window.location.reload();
  };

  const failureUi = (
    <div className="console-error" role="alert">
      <p>{t('console.scanner.chunk_load_failed')}</p>
      {!reloadAttempted && (
        <div className="console-actions">
          {attempt < SCANNER_MAX_IN_PLACE_RETRIES && (
            <button onClick={retry}>{t('seismic.retry')}</button>
          )}
          {attempt >= SCANNER_MAX_IN_PLACE_RETRIES && (
            <button onClick={reload}>{t('console.scanner.reload_page')}</button>
          )}
          <button className="secondary" onClick={onDashboard}>{t('nav.home')}</button>
        </div>
      )}
      <button className="secondary" onClick={onBack}>{t('console.back_object')}</button>
    </div>
  );

  return (
    <ScannerLoadBoundary key={attempt} fallback={failureUi}>
      <Suspense fallback={<main className="console-loading">{t('console.loading_xr')}</main>}>
        <ScannerFeature objectId={objectId} sessionId={sessionId} status={status} onScanSaved={onScanSaved} />
      </Suspense>
    </ScannerLoadBoundary>
  );
}
