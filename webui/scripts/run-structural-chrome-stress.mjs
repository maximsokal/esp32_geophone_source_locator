process.env.XR_STRESS_URL ??= 'http://127.0.0.1:4173/stress/xr-structural-stress.html';
await import('./run-mobile-chrome-stress.mjs');
