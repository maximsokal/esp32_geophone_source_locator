import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import DeviceConsoleApp from './DeviceConsoleApp';
import { installClientDiagnostics } from './data/clientDiagnostics';
import { LocaleProvider } from './i18n';
import './styles.css';
import './theme.css';
/*
 * The scanner itself is code-split, but its styles must not be. ESP32 serves a
 * tiny flash-authoritative asset set and a missed lazy CSS chunk makes a valid
 * completed scan impossible to render. Keep all scanner/XR presentation CSS in
 * the stable entry stylesheet; Vite de-duplicates the imports used by the lazy
 * components.
 */
import './scan.css';
import './components/StructuralGuideOverlay.css';

installClientDiagnostics();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <LocaleProvider><DeviceConsoleApp /></LocaleProvider>
  </StrictMode>,
);
