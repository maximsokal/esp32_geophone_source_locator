import { Component, type ReactNode } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';

/** Three consecutive failures per scan identity; retries are always operator initiated. */
export const VIEWPORT_FAILURE_MAX_ATTEMPTS = 3;

export interface ViewportFailure {
  message: string;
  attempts: number;
  retry: () => void;
}

interface ViewportFailureBoundaryProps {
  children: ReactNode;
  renderFailure: (failure: ViewportFailure) => ReactNode;
  diagnosticEvent?: string;
  resetKeys?: readonly unknown[];
  onFailure?: (failure: ViewportFailure) => void;
  onRecovered?: () => void;
}

interface ViewportFailureBoundaryState {
  message: string | null;
  attempts: number;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function resetKeysChanged(previous: readonly unknown[] | undefined, next: readonly unknown[] | undefined): boolean {
  if (previous === next) return false;
  const previousKeys = previous ?? [];
  const nextKeys = next ?? [];
  return previousKeys.length !== nextKeys.length || nextKeys.some((key, index) => !Object.is(key, previousKeys[index]));
}

/** Durable scans use their canonical CRC identity; unsaved scans use App's generation counter. */
export function resultViewportIdentityKey(scanIdentity: string | null, provisionalGeneration: number): string {
  return scanIdentity ?? `provisional-generation:${provisionalGeneration}`;
}

export class ViewportFailureBoundary extends Component<ViewportFailureBoundaryProps, ViewportFailureBoundaryState> {
  state: ViewportFailureBoundaryState = { message: null, attempts: 0 };

  static getDerivedStateFromError(error: unknown): Partial<ViewportFailureBoundaryState> {
    return { message: errorMessage(error) };
  }

  componentDidCatch(error: unknown, info: { componentStack?: string | null }): void {
    const message = errorMessage(error);
    const attempts = this.state.attempts + 1;
    this.setState({ attempts });
    clientDiagnostics.warn(this.props.diagnosticEvent ?? 'SCAN_RESULT_RENDERER_LOAD_FAILED', {
      error: message,
      componentStack: info.componentStack ?? '',
      attempts,
    });
    void clientDiagnostics.flush();
    this.props.onFailure?.({ message, attempts, retry: this.retry });
  }

  componentDidUpdate(previousProps: ViewportFailureBoundaryProps, previousState: ViewportFailureBoundaryState): void {
    if (this.state.message !== null && resetKeysChanged(previousProps.resetKeys, this.props.resetKeys)) {
      this.setState({ message: null });
      return;
    }
    if (previousState.message !== null && this.state.message === null) {
      this.setState({ attempts: 0 });
      this.props.onRecovered?.();
    }
  }

  retry = (): void => {
    if (this.state.message === null || this.state.attempts >= VIEWPORT_FAILURE_MAX_ATTEMPTS) return;
    this.setState({ message: null });
  };

  render(): ReactNode {
    if (this.state.message !== null) {
      return this.props.renderFailure({
        message: this.state.message,
        attempts: Math.max(1, this.state.attempts),
        retry: this.retry,
      });
    }
    return this.props.children;
  }
}
