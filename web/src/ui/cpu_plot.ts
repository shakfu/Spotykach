// cpu_plot.ts - the CPU history, drawn.
//
// Split out because it is the one piece of the terminal that is pure rendering: given a history and a
// peak it paints a canvas and decides nothing. Keeping it separate is what let the terminal's state
// move into a view-model without dragging a canvas along with it.

/** Draw the history. `max` is the peak-since-reset marker, or null when there is nothing to mark. */
export function drawCpuPlot(
  canvas: HTMLCanvasElement, history: readonly number[], max: number | null,
): void {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const { width: w, height: h } = canvas;
  const style = getComputedStyle(document.body);
  ctx.clearRect(0, 0, w, h);
  // Grow the axis past 100% rather than clipping: a run that exceeds the block budget is exactly the
  // measurement worth seeing, and a flat line pinned to the top says nothing about how far over it is.
  const ceiling = Math.max(100, Math.ceil((max ?? 0) / 25) * 25);
  const span = Math.max(1, history.length - 1);

  ctx.strokeStyle = style.getPropertyValue('--grid') || '#333';
  ctx.lineWidth = 1;
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillStyle = style.getPropertyValue('--muted') || '#888';
  for (let pct = 0; pct <= ceiling; pct += 25) {
    const y = h - (pct / ceiling) * (h - 12) - 6;
    ctx.beginPath();
    ctx.moveTo(28, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.fillText(`${pct}%`, 2, y + 3);
  }

  if (history.length > 1) {
    ctx.strokeStyle = style.getPropertyValue('--accent') || '#4ea1ff';
    ctx.lineWidth = 2;
    ctx.beginPath();
    history.forEach((v, i) => {
      const x = 28 + (i / span) * (w - 30);
      const y = h - (v / ceiling) * (h - 12) - 6;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  if (max != null) {
    const y = h - (max / ceiling) * (h - 12) - 6;
    ctx.strokeStyle = style.getPropertyValue('--danger') || '#e05252';
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(28, y);
    ctx.lineTo(w, y);
    ctx.stroke();
    ctx.setLineDash([]);
  }
}
