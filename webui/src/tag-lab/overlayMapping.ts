/**
 * Maps detector corners (normalized x/(w-1), y/(h-1) of the analysed frame, top-left origin, v down) onto the
 * on-screen box that shows the same image with `object-fit: contain`.
 */
export interface DisplayBox { width: number; height: number; }
export interface FrameSize { width: number; height: number; }

export interface ContainFit { scale: number; offsetX: number; offsetY: number; }

export function containFit(frame: FrameSize, box: DisplayBox): ContainFit {
  const scale = Math.min(box.width / frame.width, box.height / frame.height);
  return { scale, offsetX: (box.width - frame.width * scale) / 2, offsetY: (box.height - frame.height * scale) / 2 };
}

export function frameCornerToDisplay(
  corner: readonly [number, number],
  frame: FrameSize,
  box: DisplayBox,
  mirrored = false,
): [number, number] {
  const fit = containFit(frame, box);
  const px = corner[0] * (frame.width - 1);
  const py = corner[1] * (frame.height - 1);
  const x = fit.offsetX + px * fit.scale;
  return [mirrored ? box.width - x : x, fit.offsetY + py * fit.scale];
}
