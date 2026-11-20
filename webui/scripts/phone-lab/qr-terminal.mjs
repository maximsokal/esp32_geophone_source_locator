/**
 * Renders a QR module matrix (modules[y][x], true = dark, no quiet zone) for a terminal. Black-on-white cells are painted with
 * explicit ANSI colours, so the polarity is right on dark and light themes alike. Two module rows per text row (half blocks).
 */
export function renderQrLines(modules, quiet = 2) {
  const size = modules.length;
  const at = (x, y) => (x >= 0 && y >= 0 && x < size && y < size ? modules[y][x] : false);
  const lines = [];
  for (let y = -quiet; y < size + quiet; y += 2) {
    let line = '\u001b[30;47m';
    for (let x = -quiet; x < size + quiet; x += 1) {
      const top = at(x, y);
      const bottom = at(x, y + 1);
      line += top && bottom ? '\u2588' : top ? '\u2580' : bottom ? '\u2584' : ' ';
    }
    lines.push(`${line}\u001b[0m`);
  }
  return lines;
}
