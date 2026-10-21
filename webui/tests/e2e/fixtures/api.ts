import type { BrowserContext, Route } from '@playwright/test';
import { fullDeviceStatus } from '../../console/fixtures/deviceStatusFull';
import type { NrfLogPage } from '../../../src/domain/device';

/** Transport boundary only: production components, parsers and assets stay real. */
export class DeviceApi {
  readonly status = fullDeviceStatus();
  readonly requests: string[] = [];
  readonly problems: string[] = [];
  readonly expectedHttpErrors = new Set<string>();
  statusFailure: 'http' | 'schema' | null = null;
  private statusPause: Promise<void> | null = null;
  private finishStatus: (() => void) | null = null;
  readonly logPage: NrfLogPage = {
    ok: true, node: 'main', channel: null, records: [], next_token: null, reset: false,
    counters: { overwritten_total: 0, lost_internal_total: 0, wire_gap_total: 0, local_drop_total: 0 },
    logs_unsupported: false, boot_id: null,
  };

  constructor() {
    this.status.hardware = {
      required_missing: false,
      modules: [
        { id: 'ad7606', state: 'present', required: true, reason: 'ready', reason_code: 'ready' },
        { id: 'power_monitor', state: 'present', required: false, reason: 'sensor_ok', reason_code: 'sensor_ok' },
      ],
    };
    this.status.workflow!.step = 'SCAN_CHOICE';
    this.status.workflow!.allowed_actions = ['SCAN_START'];
    for (const pod of this.status.pods) pod.state = 'READY';
  }

  holdStatus(): void {
    this.statusPause = new Promise((resolve) => { this.finishStatus = resolve; });
  }

  resumeStatus(): void {
    this.finishStatus?.();
    this.statusPause = null;
    this.finishStatus = null;
  }

  async install(context: BrowserContext, origin: string): Promise<void> {
    await context.routeWebSocket('**/*', (socket) => {
      this.problems.push(`Unexpected WebSocket: ${socket.url()}`);
      socket.close();
    });
    await context.route('**/*', async (route: Route) => {
      const request = route.request();
      const url = new URL(request.url());
      const id = `${request.method()} ${url.pathname}${url.search}`;
      this.requests.push(id);
      if (url.origin === origin) {
        if (request.method() === 'GET' && url.pathname === '/api/status') {
          await this.statusPause;
          if (this.statusFailure === 'http') {
            this.expectedHttpErrors.add(url.pathname);
            await route.fulfill({ status: 503, json: { error: 'fixture_status_unavailable' } });
          } else {
            await route.fulfill({ json: this.statusFailure === 'schema' ? { system: null } : this.status });
          }
          return;
        }
        if (request.method() === 'POST' && url.pathname === '/api/client-log') {
          await route.fulfill({ status: 204 });
          return;
        }
        if (request.method() === 'GET' && url.pathname === '/api/localization/runs' &&
            url.searchParams.get('job') === 'settings' && url.searchParams.get('object_id') === this.status.workflow?.object_id) {
          await route.fulfill({ json: { ok: true, settings: null, presets: [], standard_processing: {} } });
          return;
        }
        if (request.method() === 'GET' && url.pathname === '/api/localization/runs' &&
            url.searchParams.get('object_id') === this.status.workflow?.object_id &&
            url.searchParams.get('session_id') === this.status.workflow?.session_id) {
          await route.fulfill({ json: { geometry: { available: false, pods: [] }, cs_pairs: { available: false, pairs: [] }, picks: [], runs: [] } });
          return;
        }
        if (request.method() === 'GET' && url.pathname === '/api/nrf/logs' &&
            url.searchParams.get('node') === this.logPage.node) {
          await route.fulfill({ json: this.logPage });
          return;
        }
        if (request.method() === 'POST' && url.pathname === '/api/control') {
          const body = request.postDataJSON() as Record<string, unknown>;
          const workflow = this.status.workflow!;
          if (body.action === 'workflow_command' && body.generation === workflow.generation &&
              ((body.command === 'SCAN_START' && workflow.step === 'SCAN_CHOICE') ||
               (body.command === 'BACK' && workflow.step === 'SCANNING'))) {
            workflow.step = body.command === 'SCAN_START' ? 'SCANNING' : 'SCAN_CHOICE';
            workflow.allowed_actions = workflow.step === 'SCANNING' ? ['BACK'] : ['SCAN_START'];
            workflow.generation += 1;
            await route.fulfill({ json: { ok: true, workflow } });
            return;
          }
        }
        if (request.method() === 'GET' && (url.pathname === '/' ||
            /^\/assets\/[^/]+\.(js|css|woff2)$/.test(url.pathname))) {
          await route.continue();
          return;
        }
      }
      this.problems.push(`Unexpected network: ${request.method()} ${url.href}`);
      await route.abort('blockedbyclient');
    });
  }
}
