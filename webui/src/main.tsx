import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import App from './App';
import { installClientDiagnostics } from './data/clientDiagnostics';
import './styles.css';

installClientDiagnostics();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
