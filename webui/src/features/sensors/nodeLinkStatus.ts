import type { DeviceNodeLink } from '../../domain/device';

export const NODE_LINK_STATUS_KEYS: Record<string, 'sensors.node_link.not_connected' | 'sensors.node_link.connected' | 'sensors.node_link.ready' | 'sensors.node_link.measuring' | 'sensors.node_link.stale'> = {
  STALE: 'sensors.node_link.stale',
  NOT_CONNECTED: 'sensors.node_link.not_connected',
  CONNECTED: 'sensors.node_link.connected',
  READY: 'sensors.node_link.ready',
  MEASURING: 'sensors.node_link.measuring',
};

const NODE_LINK_STATUS_CLASSES: Record<string, string> = {
  STALE: 'connected',
  NOT_CONNECTED: 'not-connected',
  CONNECTED: 'connected',
  READY: 'ready',
  MEASURING: 'measuring',
};

/*
 * CSS modifier of a link status. A stale snapshot may only show a failure as
 * such: last-known READY/MEASURING/CONNECTED is neutral, never green/blue.
 * Unknown values are neutral too.
 */
export function nodeLinkClass(status: string, stale: boolean): string {
  if (stale && status !== 'NOT_CONNECTED') return 'unknown';
  return NODE_LINK_STATUS_CLASSES[status] ?? 'unknown';
}

/*
 * node_links.channel is 1-based (1 = MAIN, 2..4 = G1..G3); the Live graphs are
 * 0-based and labelled G1..G4. Match the TFT: waveform i shows link channel i+1.
 * Missing or duplicated channels yield null (neutral), never a guess.
 */
export function waveformLinkStatus(links: readonly DeviceNodeLink[] | undefined, waveformIndex: number): string | null {
  const matches = (links ?? []).filter((link) => link.channel === waveformIndex + 1);
  return matches.length === 1 ? matches[0].status : null;
}
