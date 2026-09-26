import type { ControlResult } from '../../api/deviceApi';

function failureMessage(result: ControlResult): string {
  const error = result.error ?? 'request was not accepted';
  if (result.error_class === 'acquisition_unavailable') return `Acquisition unavailable: ${error}`;
  return `ESP ${result.action} failed: ${error}`;
}

/** Run UI projections and follow-up reads only after an explicit successful control result. */
export async function runAfterControlConfirmation<T>(
  request: Promise<ControlResult>,
  onSuccess: (result: ControlResult) => T | Promise<T>,
): Promise<T> {
  const result = await request;
  if (result?.ok !== true) throw new Error(failureMessage(result));
  return onSuccess(result);
}
