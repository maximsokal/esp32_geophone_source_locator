import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { fetchLocalizationRuns, isRequestCancellation, loadObjectScanArtifact, retryPreemptedRead } from '../../api/deviceApi';
import type { ObjectProfile, RecordingSessionSummary } from '../../domain/device';
import { buildRecordingReport } from '../../domain/reportModel';
import { decodeScanArtifact } from '../../xr/scanArtifact';
import ReportView from './ReportView';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../i18n';

import latin400 from '@fontsource/montserrat/files/montserrat-latin-400-normal.woff2?inline';
import latin700 from '@fontsource/montserrat/files/montserrat-latin-700-normal.woff2?inline';
import cyrillic400 from '@fontsource/montserrat/files/montserrat-cyrillic-400-normal.woff2?inline';
import cyrillic700 from '@fontsource/montserrat/files/montserrat-cyrillic-700-normal.woff2?inline';
import cyrillicExt400 from '@fontsource/montserrat/files/montserrat-cyrillic-ext-400-normal.woff2?inline';
import cyrillicExt700 from '@fontsource/montserrat/files/montserrat-cyrillic-ext-700-normal.woff2?inline';

/*
 * MAX-150: build the report from the saved recording and open it in its own
 * print window (browser "Save as PDF"). Lazy chunk: react-dom/server and
 * the scan decoder never reach the core console bundle.
 */

const FONT_CSS = `
@font-face{font-family:Montserrat;font-style:normal;font-weight:400;src:url('${latin400}') format('woff2');unicode-range:U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+0304,U+0308,U+0329,U+2000-206F,U+20AC,U+2122,U+2191,U+2193,U+2212,U+2215,U+FEFF,U+FFFD}
@font-face{font-family:Montserrat;font-style:normal;font-weight:400;src:url('${cyrillic400}') format('woff2');unicode-range:U+0301,U+0400-045F,U+0490-0491,U+04B0-04B1,U+2116}
@font-face{font-family:Montserrat;font-style:normal;font-weight:400;src:url('${cyrillicExt400}') format('woff2');unicode-range:U+0460-052F,U+1C80-1C8A,U+20B4,U+2DE0-2DFF,U+A640-A69F,U+FE2E-FE2F}
@font-face{font-family:Montserrat;font-style:normal;font-weight:700;src:url('${latin700}') format('woff2');unicode-range:U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+0304,U+0308,U+0329,U+2000-206F,U+20AC,U+2122,U+2191,U+2193,U+2212,U+2215,U+FEFF,U+FFFD}
@font-face{font-family:Montserrat;font-style:normal;font-weight:700;src:url('${cyrillic700}') format('woff2');unicode-range:U+0301,U+0400-045F,U+0490-0491,U+04B0-04B1,U+2116}
@font-face{font-family:Montserrat;font-style:normal;font-weight:700;src:url('${cyrillicExt700}') format('woff2');unicode-range:U+0460-052F,U+1C80-1C8A,U+20B4,U+2DE0-2DFF,U+A640-A69F,U+FE2E-FE2F}
`;

const PRINT_CSS = `
${FONT_CSS}
body{font-family:Montserrat,sans-serif;color:#111;margin:18mm;font-size:11pt}
h1{font-size:17pt;margin:0 0 4pt}h2{font-size:13pt;margin:14pt 0 6pt;border-bottom:1px solid #999}
table{border-collapse:collapse;width:100%;font-size:10pt}th,td{border:1px solid #bbb;padding:3pt 5pt;text-align:left}
.report-panels{display:flex;flex-wrap:wrap;gap:8pt}.report-panel{margin:0}.report-panel figcaption,.report-qr figcaption{font-size:9pt;color:#444}
.report-qrs{display:flex;gap:16pt}.report-qr{margin:0;text-align:center}.report-note{font-size:9pt;color:#444}
a{color:#0b57d0;word-break:break-all}.report-print-bar{margin-bottom:10pt}
@media print{.report-print-bar{display:none}.report-break{break-before:page}}
`;

export function renderReportDocument(html: string, title: string, locale: I18nLocale = 'en'): string {
  return `<!doctype html><html><head><meta charset="utf-8"><title>${title.replace(/[<>&]/g, '')}</title><style>${PRINT_CSS}</style></head>`
    + `<body><div class="report-print-bar"><button onclick="window.print()">${translate(locale, 'report.print_pdf')}</button></div>${html}</body></html>`;
}

export async function openRecordingReport(
  object: ObjectProfile,
  session: RecordingSessionSummary,
  selectedMethod: string | null = null,
  locale: I18nLocale = 'en',
): Promise<void> {
  /* Open synchronously-ish so pop-up blockers see the user gesture. */
  const target = window.open('', '_blank');
  if (target === null) throw new Error(translate(locale, 'report.popup_blocked'));
  target.document.write(`<p style="font-family:Montserrat,sans-serif">${translate(locale, 'report.preparing')}</p>`);

  /* Sequential: the ESP serves one HTTPS request at a time. */
  const listing = await retryPreemptedRead(() => fetchLocalizationRuns(object.id, session.session_id)).catch((reason: unknown) => {
    if (isRequestCancellation(reason)) throw reason;
    return null;
  });
  let positions: Float32Array | null = null;
  try {
    const artifact = await loadObjectScanArtifact(object.id, session.session_id);
    if (artifact !== null) positions = decodeScanArtifact(artifact).positions;
  }
  catch {
    positions = null;
  }

  if (locale === 'uk') await loadUkrainianCatalog();
  const report = buildRecordingReport(object, session, listing, positions, selectedMethod, Date.now());
  const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(ReportView, { report })));
  target.document.open();
  target.document.write(renderReportDocument(html, translate(locale, 'report.title', { object: object.name, session: session.session_id }), locale));
  target.document.close();
}
