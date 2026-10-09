export type ProjectPoint = readonly [number, number, number];
export const MAX_OPERATOR_EDGES = 128;

export interface OperatorEdgeCapacityDiagnostics {
  count: number;
  maximum: number;
  capacityReached: boolean;
}

export interface OperatorEdge {
  id: number;
  start: ProjectPoint;
  end: ProjectPoint;
  confidence: number;
}

export type OperatorEdgeAddRejection = 'too-short' | 'capacity';

function distance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]);
}

function sameEndpoints(a: OperatorEdge, start: ProjectPoint, end: ProjectPoint, tolerance: number): boolean {
  return (
    distance(a.start, start) <= tolerance && distance(a.end, end) <= tolerance
  ) || (
    distance(a.start, end) <= tolerance && distance(a.end, start) <= tolerance
  );
}

export class OperatorEdgeStore {
  private readonly edges: OperatorEdge[] = [];
  private nextId = 1;
  private reachedCapacity = false;
  private lastAddRejection: OperatorEdgeAddRejection | null = null;

  reset(): void { this.edges.length = 0; this.nextId = 1; this.reachedCapacity = false; this.lastAddRejection = null; }

  add(start: ProjectPoint, end: ProjectPoint, confidence = 1, dedupeMeters = 0.08): OperatorEdge | null {
    this.lastAddRejection = null;
    if (distance(start, end) < 0.05) { this.lastAddRejection = 'too-short'; return null; }
    const existing = this.edges.find((edge) => sameEndpoints(edge, start, end, dedupeMeters));
    if (existing !== undefined) {
      existing.confidence = Math.max(existing.confidence, confidence);
      existing.start = [...start] as ProjectPoint;
      existing.end = [...end] as ProjectPoint;
      return { ...existing, start: [...existing.start] as ProjectPoint, end: [...existing.end] as ProjectPoint };
    }
    if (this.edges.length >= MAX_OPERATOR_EDGES) {
      this.reachedCapacity = true;
      this.lastAddRejection = 'capacity';
      return null;
    }
    const edge: OperatorEdge = {
      id: this.nextId++,
      start: [...start] as ProjectPoint,
      end: [...end] as ProjectPoint,
      confidence: Math.max(0, Math.min(1, confidence)),
    };
    this.edges.push(edge);
    if (this.edges.length === MAX_OPERATOR_EDGES) this.reachedCapacity = true;
    return { ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint };
  }

  removeLast(): OperatorEdge | null {
    const edge = this.edges.pop();
    return edge === undefined ? null : { ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint };
  }

  get size(): number { return this.edges.length; }
  get capacityReached(): boolean { return this.reachedCapacity; }
  get lastAddRejectionReason(): OperatorEdgeAddRejection | null { return this.lastAddRejection; }
  get capacityDiagnostics(): OperatorEdgeCapacityDiagnostics {
    return { count: this.size, maximum: MAX_OPERATOR_EDGES, capacityReached: this.reachedCapacity };
  }

  snapshot(): readonly OperatorEdge[] {
    return this.edges.map((edge) => ({ ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint }));
  }
}
