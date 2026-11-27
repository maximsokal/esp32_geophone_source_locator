/**
 * Checkpoint of the fusion state (Phase 2, crash recovery). The accumulated state is Maps and Sets of plain objects, numbers and one nested occupancy grid;
 * several Maps reference the SAME point objects, and the structured clone that carries a checkpoint between workers keeps those shared references, so a
 * restored accumulator continues exactly where the original was. The acceptance test is bit equality of a crashed-and-restored run with an uninterrupted one.
 *
 * Not part of a checkpoint: the scratch buffers of the live coverage snapshot (rebuilt on demand) and the monocular-prior hook (code, not state).
 */
const SKIPPED = new Set(['occupancy', 'monocularPrior', 'liveCoveragePositions', 'liveCoverageConfidence', 'liveCoverageQualityScratch', 'liveCoverageRankedScratch']);

export interface FusionCheckpointState {
  version: 1;
  accumulator: Record<string, unknown>;
  occupancy: Record<string, unknown>;
}

function stateOf(target: object): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(target)) if (!SKIPPED.has(key) && typeof value !== 'function') out[key] = value;
  return out;
}

/** A LIVE view of the state: the caller must clone it (postMessage or structuredClone) before the accumulator fuses anything else. */
export function captureFusionState(accumulator: object): FusionCheckpointState {
  const occupancy = (accumulator as { occupancy: object }).occupancy;
  return { version: 1, accumulator: stateOf(accumulator), occupancy: stateOf(occupancy) };
}

function assign(target: object, state: Record<string, unknown>, owner: string): void {
  const record = target as Record<string, unknown>;
  const expected = Object.keys(stateOf(target));
  const missing = expected.filter((key) => !(key in state));
  const unknown = Object.keys(state).filter((key) => !expected.includes(key));
  /* A field added to the accumulator without a thought for recovery must fail loudly here, not restore half a scan. */
  if (missing.length > 0 || unknown.length > 0) throw new Error(`fusion checkpoint does not match the ${owner}: missing [${missing.join(', ')}], unknown [${unknown.join(', ')}]`);
  for (const key of expected) record[key] = state[key];
}

/** Puts a CLONED checkpoint into a freshly reset accumulator. */
export function restoreFusionState(accumulator: object, state: FusionCheckpointState): void {
  if (state === null || typeof state !== 'object' || state.version !== 1) throw new Error('unknown fusion checkpoint');
  assign((accumulator as { occupancy: object }).occupancy, state.occupancy, 'occupancy grid');
  assign(accumulator, state.accumulator, 'accumulator');
}
