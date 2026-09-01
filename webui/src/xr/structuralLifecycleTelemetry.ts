import type { StructuralCornerCandidate, StructuralEdgeCandidate } from './structuralGuide';

export interface StructuralLifecycleSnapshot {
  edgeHypothesesActive: number;
  edgeHypothesesCreated: number;
  edgeHypothesesRetainedTransitions: number;
  edgeHypothesesExpired: number;
  cornerHypothesesActive: number;
  cornerHypothesesCreated: number;
  cornerHypothesesRetainedTransitions: number;
  cornerHypothesesExpired: number;
}

function nextIds(items: readonly { id: string }[]): Set<string> {
  return new Set(items.map((item) => item.id));
}

function transition(
  previous: ReadonlySet<string>,
  current: ReadonlySet<string>,
): { created: number; retained: number; expired: number } {
  let created = 0, retained = 0, expired = 0;
  for (const id of current) {
    if (previous.has(id)) retained += 1;
    else created += 1;
  }
  for (const id of previous) if (!current.has(id)) expired += 1;
  return { created, retained, expired };
}

/**
 * Lightweight diagnostic state around the existing robust tracker. Candidate IDs
 * are already stable across matched observations, so we can expose lifecycle
 * starvation without changing any geometry or confidence decisions.
 */
export class StructuralLifecycleTelemetry {
  private edgeIds = new Set<string>();
  private cornerIds = new Set<string>();
  private edgeCreated = 0;
  private edgeRetainedTransitions = 0;
  private edgeExpired = 0;
  private cornerCreated = 0;
  private cornerRetainedTransitions = 0;
  private cornerExpired = 0;

  reset(): void {
    this.edgeIds.clear();
    this.cornerIds.clear();
    this.edgeCreated = 0;
    this.edgeRetainedTransitions = 0;
    this.edgeExpired = 0;
    this.cornerCreated = 0;
    this.cornerRetainedTransitions = 0;
    this.cornerExpired = 0;
  }

  update(
    edges: readonly StructuralEdgeCandidate[],
    corners: readonly StructuralCornerCandidate[],
  ): StructuralLifecycleSnapshot {
    const nextEdges = nextIds(edges);
    const nextCorners = nextIds(corners);
    const edgeTransition = transition(this.edgeIds, nextEdges);
    const cornerTransition = transition(this.cornerIds, nextCorners);

    this.edgeCreated += edgeTransition.created;
    this.edgeRetainedTransitions += edgeTransition.retained;
    this.edgeExpired += edgeTransition.expired;
    this.cornerCreated += cornerTransition.created;
    this.cornerRetainedTransitions += cornerTransition.retained;
    this.cornerExpired += cornerTransition.expired;
    this.edgeIds = nextEdges;
    this.cornerIds = nextCorners;

    return {
      edgeHypothesesActive: nextEdges.size,
      edgeHypothesesCreated: this.edgeCreated,
      edgeHypothesesRetainedTransitions: this.edgeRetainedTransitions,
      edgeHypothesesExpired: this.edgeExpired,
      cornerHypothesesActive: nextCorners.size,
      cornerHypothesesCreated: this.cornerCreated,
      cornerHypothesesRetainedTransitions: this.cornerRetainedTransitions,
      cornerHypothesesExpired: this.cornerExpired,
    };
  }
}
