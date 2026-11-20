import { describe, expect, it } from 'vitest';
// @ts-expect-error plain .mjs dev scripts without type declarations
import { isAllowedPeer, isPrivateIPv4, pickLanAddress } from '../../scripts/phone-lab/lan.mjs';
// @ts-expect-error plain .mjs dev scripts without type declarations
import { renderQrLines } from '../../scripts/phone-lab/qr-terminal.mjs';
import { encodeQr } from '../../src/domain/qrCode';

const iface = (address: string, internal = false) => [{ family: 'IPv4', address, internal }];

describe('LAN address selection', () => {
  it('classifies private, link-local and public IPv4', () => {
    for (const ok of ['10.1.2.3', '172.16.0.1', '172.31.255.1', '192.168.0.148']) expect(isPrivateIPv4(ok)).toBe(true);
    for (const bad of ['169.254.1.1', '172.32.0.1', '8.8.8.8', '192.169.0.1', 'abc', '']) expect(isPrivateIPv4(bad)).toBe(false);
  });

  it('prefers the Wi-Fi adapter over virtual adapters (WSL / Hyper-V / VPN)', () => {
    const picked = pickLanAddress({
      'vEthernet (WSL (Hyper-V firewall))': iface('172.17.112.1'),
      Ethernet: iface('192.168.1.20'),
      'Беспроводная сеть': iface('192.168.0.148'),
      'Wi-Fi VPN tap-1': iface('10.8.0.2'),
    }, undefined);
    expect(picked.address).toBe('192.168.0.148');
    expect(picked.candidates).toHaveLength(4);
  });

  it('prefers a physical adapter over a virtual one when no Wi-Fi name matches', () => {
    expect(pickLanAddress({ 'vEthernet (Default Switch)': iface('172.20.0.1'), Ethernet: iface('192.168.1.20') }, undefined).address).toBe('192.168.1.20');
  });

  it('ignores loopback, link-local and public addresses and returns null when nothing is usable', () => {
    expect(pickLanAddress({ lo: iface('127.0.0.1', true), Wifi: iface('169.254.3.3'), Net: iface('8.8.4.4') }, undefined)).toBeNull();
  });

  it('honours an explicit override', () => {
    expect(pickLanAddress({ Wifi: iface('192.168.0.5') }, '10.0.0.9').address).toBe('10.0.0.9');
  });

  it('answers only loopback and private peers, including IPv4-mapped IPv6', () => {
    for (const ok of ['127.0.0.1', '::1', '::ffff:192.168.0.50', '10.0.0.4']) expect(isAllowedPeer(ok)).toBe(true);
    for (const bad of ['8.8.8.8', '::ffff:8.8.8.8', '2001:db8::1', undefined, '']) expect(isAllowedPeer(bad as string)).toBe(false);
  });
});

describe('terminal QR rendering', () => {
  const stripAnsi = (s: string) => s.replace(/\u001b\[[0-9;]*m/g, '');

  it('maps module pairs to half-block characters with explicit black-on-white colours', () => {
    const lines = renderQrLines([[true, false], [false, true]], 0);
    expect(lines).toHaveLength(1);
    expect(lines[0].startsWith('\u001b[30;47m')).toBe(true);
    expect(lines[0].endsWith('\u001b[0m')).toBe(true);
    expect(stripAnsi(lines[0])).toBe('▀▄');
    expect(stripAnsi(renderQrLines([[true, true], [true, true]], 0)[0])).toBe('██');
    expect(stripAnsi(renderQrLines([[false, false], [false, false]], 0)[0])).toBe('  ');
  });

  it('adds a white quiet zone and keeps every line the same width', () => {
    const code = encodeQr('https://192.168.0.148:5443/')!;
    const lines = renderQrLines(code.modules, 2).map(stripAnsi);
    const width = code.size + 4;
    expect(lines.every((l: string) => l.length === width)).toBe(true);
    expect(lines).toHaveLength(Math.ceil((code.size + 4) / 2));
    expect(lines[0].trim()).toBe('');
  });

  it('draws the finder pattern of a real QR for the phone URL (7x7 dark ring at the top-left)', () => {
    const code = encodeQr('https://192.168.0.148:5443/')!;
    expect(code.version).toBeLessThanOrEqual(4);
    for (let i = 0; i < 7; i += 1) {
      expect(code.modules[0][i]).toBe(true);
      expect(code.modules[6][i]).toBe(true);
      expect(code.modules[i][0]).toBe(true);
      expect(code.modules[i][6]).toBe(true);
    }
    expect(code.modules[1][1]).toBe(false);
    expect(code.modules[3][3]).toBe(true);
  });
});
