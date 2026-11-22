import { buildLiveMesh, type LiveMeshInput, type LiveMeshResult } from './liveMeshCore';

export interface LiveMeshRequest { type: 'mesh'; requestId: number; input: LiveMeshInput }
export type LiveMeshResponse =
  | { type: 'result'; requestId: number; result: LiveMeshResult; elapsedMs: number }
  | { type: 'error'; requestId: number; message: string };

interface WorkerScope {
  onmessage: ((event: { data: LiveMeshRequest }) => void) | null;
  postMessage(message: LiveMeshResponse, transfer?: Transferable[]): void;
}
const scope = self as unknown as WorkerScope;
scope.onmessage = (event) => {
  const { requestId, input } = event.data;
  const started = performance.now();
  try {
    const result = buildLiveMesh(input);
    scope.postMessage({ type: 'result', requestId, result, elapsedMs: performance.now() - started }, [result.trianglesXr.buffer]);
  } catch (error) {
    scope.postMessage({ type: 'error', requestId, message: error instanceof Error ? error.message : String(error) });
  }
};
