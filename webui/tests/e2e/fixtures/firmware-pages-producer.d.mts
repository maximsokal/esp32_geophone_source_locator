export type CapturedResponse = { id: string; file: string; status: number; content_type: string;
  cache_control: string; location: string; sha256: string };
export type Producer = { status: string; inputs: Record<string, string>; cases: CapturedResponse[];
  assets: Record<string, string>; binary_sha256: string };
export function loadFirmwarePages(): Promise<{ producer: Producer; bodies: Record<string, string>;
  assets: Record<string, string>; runPath: string }>;
