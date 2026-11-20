// Picks the PC address a phone on the same home Wi-Fi can reach.
const VIRTUAL = /vethernet|wsl|virtualbox|vmware|hyper-v|docker|loopback|bluetooth|tailscale|zerotier|vpn|tap-|tun/i;
const WIFI = /wi-?fi|wlan|wireless|беспровод/i;

export function isPrivateIPv4(address) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(address);
  if (m === null) return false;
  const [a, b] = [Number(m[1]), Number(m[2])];
  if (a === 169 && b === 254) return false;
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168);
}

/** True for peers a phone-lab server may answer: loopback or private IPv4 (also IPv4-mapped IPv6). */
export function isAllowedPeer(remoteAddress) {
  if (typeof remoteAddress !== 'string') return false;
  const plain = remoteAddress.replace(/^::ffff:/i, '');
  return plain === '::1' || plain === '127.0.0.1' || isPrivateIPv4(plain);
}

/** interfaces: os.networkInterfaces() shape. Returns { address, name, candidates } or null. */
export function pickLanAddress(interfaces, override) {
  const candidates = [];
  for (const [name, list] of Object.entries(interfaces)) {
    for (const entry of list ?? []) {
      if (entry.family !== 'IPv4' || entry.internal || !isPrivateIPv4(entry.address)) continue;
      candidates.push({ name, address: entry.address, virtual: VIRTUAL.test(name), wifi: WIFI.test(name) });
    }
  }
  if (override !== undefined && override !== '') {
    return { address: override, name: 'override', candidates };
  }
  const ranked = [...candidates].sort((a, b) => Number(a.virtual) - Number(b.virtual) || Number(b.wifi) - Number(a.wifi));
  return ranked.length === 0 ? null : { address: ranked[0].address, name: ranked[0].name, candidates };
}
