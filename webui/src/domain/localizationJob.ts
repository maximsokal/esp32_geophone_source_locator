/**
 * "Calculate source" runs as a background job on the device (firmware
 * component localization_job): /api/control only queues it, and
 * /api/localization/runs?job=status reports its progress, every method's
 * measured duration and the worker stack's high-water mark. This module is
 * the pure client side of that: waiting for the job, and the texts the
 * panel shows. Nothing here times or estimates anything itself -- every
 * number is the device's own measurement.
 */

import { TranslatedError, translate, type I18nLocale } from '../i18n';
import { formatNumber } from '../i18n/format';
import { ApiRequestOwnerRetiredError, isRequestCancellation } from '../api/deviceApi';

export type LocalizationJobState = 'idle' | 'queued' | 'running' | 'succeeded' | 'failed';

export interface LocalizationJobTiming {
  method: string;
  ms: number;
  skipped_for_time: boolean;
}

export interface LocalizationJobStatus {
  state: LocalizationJobState;
  job_id: number;
  object_id: string;
  session_id: string;
  computed_at_ms: number;
  current_method: string;
  elapsed_ms: number;
  budget_ms: number;
  error?: string;
  summary: {
    participating_channel_count: number;
    methods_attempted: number;
    methods_solved: number;
    methods_persisted: number;
    methods_skipped_for_time: number;
    timings: LocalizationJobTiming[];
  };
  stack: {
    size_bytes: number;
    free_min_bytes: number;
    margin_bytes: number;
    margin_ok: boolean;
  };
}

/** Poll cadence, and how long past the job's own budget to keep waiting
 * (persisting ~21 records to SD and the fusion follow the last method). */
export const JOB_POLL_INTERVAL_MS = 1000;
export const JOB_WAIT_GRACE_MS = 30000;

export function isJobFinished(status: LocalizationJobStatus): boolean {
  return status.state === 'succeeded' || status.state === 'failed';
}

function seconds(locale: I18nLocale, ms: number): string {
  return `${formatNumber(locale, ms / 1000, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} s`;
}

/** One line of live progress, as the device reports it. */
export function jobProgressText(status: LocalizationJobStatus, locale: I18nLocale = 'en'): string {
  switch (status.state) {
    case 'queued':
      return translate(locale, 'calc.job_queued');
    case 'running':
      return translate(locale, 'calc.job_running', {
        method: status.current_method === '' ? translate(locale, 'calc.job_starting') : status.current_method,
        elapsed: seconds(locale, status.elapsed_ms),
        budget: seconds(locale, status.budget_ms),
      });
    case 'succeeded':
      return translate(locale, 'calc.job_done', {
        elapsed: seconds(locale, status.elapsed_ms),
        solved: formatNumber(locale, status.summary.methods_solved),
        attempted: formatNumber(locale, status.summary.methods_attempted),
      }) + (status.summary.methods_skipped_for_time > 0
        ? translate(locale, 'calc.job_skipped', {
          count: formatNumber(locale, status.summary.methods_skipped_for_time),
          budget: seconds(locale, status.budget_ms),
        })
        : '');
    case 'failed':
      return translate(locale, 'calc.job_failed', {
        elapsed: seconds(locale, status.elapsed_ms),
        error: status.error ?? translate(locale, 'calc.job_unknown_error'),
      });
    default:
      return translate(locale, 'calc.job_idle');
  }
}

/** The worker's measured stack headroom. */
export function stackText(status: LocalizationJobStatus, locale: I18nLocale = 'en'): string {
  if (status.stack.free_min_bytes <= 0) return '';
  const kib = (bytes: number): string => `${formatNumber(locale, bytes / 1024, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} KiB`;
  return translate(locale, 'calc.stack_summary', {
    used: kib(status.stack.size_bytes - status.stack.free_min_bytes),
    total: kib(status.stack.size_bytes),
    free: kib(status.stack.free_min_bytes),
  }) + (status.stack.margin_ok ? '' : translate(locale, 'calc.stack_below', { margin: kib(status.stack.margin_bytes) }));
}

/** Timings sorted slowest first, with their share of the whole run. */
export function jobTimingRows(status: LocalizationJobStatus): Array<LocalizationJobTiming & { share: number }> {
  const total = status.summary.timings.reduce((sum, timing) => sum + timing.ms, 0);
  return [...status.summary.timings]
    .sort((a, b) => b.ms - a.ms)
    .map((timing) => ({ ...timing, share: total > 0 ? timing.ms / total : 0 }));
}

export interface WaitOptions {
  objectId: string;
  sessionId: string;
  budgetMs: number;
  onProgress?: (status: LocalizationJobStatus) => void;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
}

const defaultSleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Polls until this object/session's job finishes. Transient poll errors are
 * retried (the device may be busy writing SD); the wait gives up only after
 * the job's budget plus JOB_WAIT_GRACE_MS.
 */
export async function waitForLocalizationJob(
  fetchStatus: () => Promise<LocalizationJobStatus>,
  options: WaitOptions,
): Promise<LocalizationJobStatus> {
  const sleep = options.sleep ?? defaultSleep;
  const now = options.now ?? (() => Date.now());
  const deadline = now() + options.budgetMs + JOB_WAIT_GRACE_MS;
  let lastError: unknown = null;
  for (;;) {
    try {
      const status = await fetchStatus();
      lastError = null;
      if (status.object_id === options.objectId && status.session_id === options.sessionId) {
        options.onProgress?.(status);
        if (isJobFinished(status)) return status;
      }
    } catch (reason: unknown) {
      if (reason instanceof ApiRequestOwnerRetiredError) throw reason;
      lastError = isRequestCancellation(reason) ? null : reason;
    }
    if (now() >= deadline) {
      throw TranslatedError.fromKey('calc.job_timeout', {
        duration: (locale) => seconds(locale, options.budgetMs + JOB_WAIT_GRACE_MS),
        lastPoll: lastError instanceof Error
          ? TranslatedError.fromKey('calc.job_last_poll', { error: lastError.message })
          : '',
      });
    }
    await sleep(JOB_POLL_INTERVAL_MS);
  }
}
