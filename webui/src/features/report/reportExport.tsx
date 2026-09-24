import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { fetchLocalizationRuns, loadObjectScanArtifact } from '../../api/deviceApi';
import type { ObjectProfile, RecordingSessionSummary } from '../../domain/device';
import { buildRecordingReport } from '../../domain/reportModel';
import { decodeScanArtifact } from '../../xr/scanArtifact';
import ReportView from './ReportView';

/*
 * MAX-150: build the report from the saved recording and open it in its own
 * print window (browser "Save as PDF"). Lazy chunk: react-dom/server and
 * the scan decoder never reach the core console bundle.
 */

const PRINT_CSS = `
body{font-family:system-ui,sans-serif;color:#111;margin:18mm;font-size:11pt}
h1{font-size:17pt;margin:0 0 4pt}h2{font-size:13pt;margin:14pt 0 6pt;border-bottom:1px solid #999}
table{border-collapse:collapse;width:100%;font-size:10pt}th,td{border:1px solid #bbb;padding:3pt 5pt;text-align:left}
.report-panels{display:flex;flex-wrap:wrap;gap:8pt}.report-panel{margin:0}.report-panel figcaption,.report-qr figcaption{font-size:9pt;color:#444}
.report-qrs{display:flex;gap:16pt}.report-qr{margin:0;text-align:center}.report-note{font-size:9pt;color:#444}
a{color:#0b57d0;word-break:break-all}.report-print-bar{margin-bottom:10pt}
@media print{.report-print-bar{display:none}.report-break{break-before:page}}
`;

export function renderReportDocument(html: string, title: string): string {
  return `<!doctype html><html><head><meta charset="utf-8"><title>${title.replace(/[<>&]/g, '')}</title><style>${PRINT_CSS}</style></head>`
    + `<body><div class="report-print-bar"><button onclick="window.print()">Print / Save as PDF</button></div>${html}</body></html>`;
}

export async function openRecordingReport(
  object: ObjectProfile,
  session: RecordingSessionSummary,
  selectedMethod: string | null = null,
): Promise<void> {
  /* Open synchronously-ish so pop-up blockers see the user gesture. */
  const target = window.open('', '_blank');
  if (target === null) throw new Error('The browser blocked the report window; allow pop-ups for this device.');
  target.document.write('<p style="font-family:sans-serif">Preparing report…</p>');

  /* Sequential: the ESP serves one HTTPS request at a time. */
  const listing = await fetchLocalizationRuns(object.id, session.session_id).catch(() => null);
  let positions: Float32Array | null = null;
  try {
    const artifact = await loadObjectScanArtifact(object.id, session.session_id);
    if (artifact !== null) positions = decodeScanArtifact(artifact).positions;
  }
  catch {
    positions = null;
  }

  const report = buildRecordingReport(object, session, listing, positions, selectedMethod, Date.now());
  const html = renderToStaticMarkup(createElement(ReportView, { report }));
  target.document.open();
  target.document.write(renderReportDocument(html, `${object.name} ${session.session_id} report`));
  target.document.close();
}
