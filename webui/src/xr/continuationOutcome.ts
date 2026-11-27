/**
 * The terminal state of a Continue scan (TASK 05). Every Continue operation ends in EXACTLY ONE of four observable states and never in silence:
 *  - success:   the extension was tied to the saved scan and merged;
 *  - rejected:  it could not be tied reliably (tags, geometry, overlap, memory); the saved scan is untouched;
 *  - cancelled: the operation was stopped on purpose (newer scan, deleted scan, other object, the scanner left); the saved scan is untouched;
 *  - error:     the machinery failed (worker error, timeout); the saved scan is untouched.
 * Pure: no React, no I/O. The ledger is what makes "exactly one" a property of the code and not of the call order.
 */

export type ContinuationTerminalState = 'success' | 'rejected' | 'cancelled' | 'error';

export interface ContinuationOutcome {
  state: ContinuationTerminalState;
  /** Machine-readable cause (stable, loggable): e.g. 'tags_not_found', 'overlap', 'viewer_closed', 'worker_timeout'. */
  code: string;
  /** i18n key of the text for the operator (null for a success) and its parameters; translated where it is shown, in the operator's language. */
  messageKey: string | null;
  params?: Record<string, string | number>;
}

/** What the tag lock knew when the extension finished: enough to say WHY there was no lock. */
export interface RelockSummary {
  /** Reliable tags the saved scan carries (the lock needs {@link needed} of them). */
  baseTags: number;
  /** Distinct known tags seen reliably in the new scan right now. */
  found: number;
  needed: number;
  /** The last reason the FSM gave for not locking: 'need_more_tags' | 'collinear' | 'distance_mismatch' | 'residual_too_high' | null. */
  reason: string | null;
}

export type ContinuationGateCode = 'base_tags_insufficient' | 'tags_not_found' | 'collinear' | 'distance_mismatch' | 'residual_too_high' | 'tags_not_locked';
export type ContinuationGateResult = { ok: true } | { ok: false; code: ContinuationGateCode };

/**
 * A Continue is merged only when the new scan was tied to the saved one by at least three suitable AprilTags (a lock that passed the geometry and
 * residual checks). Overlap between the clouds never replaces a missing lock: it only refines and validates a lock that exists (issue #43, TASK 05).
 */
export function continuationGate(seed: unknown | null, summary: RelockSummary | null): ContinuationGateResult {
  if (seed !== null && seed !== undefined) return { ok: true };
  if (summary === null) return { ok: false, code: 'tags_not_locked' };
  if (summary.baseTags < summary.needed) return { ok: false, code: 'base_tags_insufficient' };
  if (summary.found < summary.needed) return { ok: false, code: 'tags_not_found' };
  if (summary.reason === 'collinear' || summary.reason === 'distance_mismatch' || summary.reason === 'residual_too_high') return { ok: false, code: summary.reason };
  return { ok: false, code: 'tags_not_locked' };
}

/**
 * One slot for the operation in flight. `begin` opens a new operation (the previous one, if still open, must have been settled by the caller first);
 * `settle` accepts only the FIRST outcome of the CURRENT operation, so a late callback of a superseded one, or a second path of the same one, is dropped.
 */
export class ContinuationLedger {
  private nextToken = 1;
  private current: { token: number; settled: boolean } | null = null;

  constructor(private readonly onTerminal: (outcome: ContinuationOutcome, token: number) => void) {}

  begin(): number {
    const token = this.nextToken++;
    this.current = { token, settled: false };
    return token;
  }

  get activeToken(): number | null { return this.current !== null && !this.current.settled ? this.current.token : null; }

  /** @returns true if this call produced the terminal state of the operation, false if it was stale or already settled. */
  settle(token: number, outcome: ContinuationOutcome): boolean {
    const current = this.current;
    if (current === null || current.token !== token || current.settled) return false;
    current.settled = true;
    this.onTerminal(outcome, token);
    return true;
  }

  /** Settles whichever operation is open (cancel paths that do not know its token). */
  settleActive(outcome: ContinuationOutcome): boolean {
    const token = this.activeToken;
    return token === null ? false : this.settle(token, outcome);
  }
}
