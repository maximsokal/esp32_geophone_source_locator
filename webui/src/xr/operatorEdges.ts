export type ProjectPoint = readonly [number, number, number];

export interface OperatorEdge {
  id: number;
  start: ProjectPoint;
  end: ProjectPoint;
  confidence: number;
}

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

  reset(): void { this.edges.length = 0; this.nextId = 1; }

  add(start: ProjectPoint, end: ProjectPoint, confidence = 1, dedupeMeters = 0.08): OperatorEdge | null {
    if (distance(start, end) < 0.05) return null;
    const existing = this.edges.find((edge) => sameEndpoints(edge, start, end, dedupeMeters));
    if (existing !== undefined) {
      existing.confidence = Math.max(existing.confidence, confidence);
      existing.start = [...start] as ProjectPoint;
      existing.end = [...end] as ProjectPoint;
      return { ...existing, start: [...existing.start] as ProjectPoint, end: [...existing.end] as ProjectPoint };
    }
    const edge: OperatorEdge = {
      id: this.nextId++,
      start: [...start] as ProjectPoint,
      end: [...end] as ProjectPoint,
      confidence: Math.max(0, Math.min(1, confidence)),
    };
    this.edges.push(edge);
    return { ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint };
  }

  removeLast(): OperatorEdge | null {
    const edge = this.edges.pop();
    return edge === undefined ? null : { ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint };
  }

  get size(): number { return this.edges.length; }

  snapshot(): readonly OperatorEdge[] {
    return this.edges.map((edge) => ({ ...edge, start: [...edge.start] as ProjectPoint, end: [...edge.end] as ProjectPoint }));
  }
}
