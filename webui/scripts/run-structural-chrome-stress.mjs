const webPort = Number(process.env.XR_STRESS_WEB_PORT ?? 4173);
process.env.XR_STRESS_URL ??= `http://127.0.0.1:${webPort}/stress/xr-structural-stress.html`;
await import('./run-mobile-chrome-stress.mjs');
