import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import DeviceConsoleApp from './DeviceConsoleApp';
import { installClientDiagnostics } from './data/clientDiagnostics';
import './styles.css';

installClientDiagnostics();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <DeviceConsoleApp />
  </StrictMode>,
);
