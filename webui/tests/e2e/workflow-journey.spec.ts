import { test, expect, catalog } from './fixtures/device';
import { scenarioScreenshot } from './fixtures/scenarios';
import { workflowActions } from './fixtures/visual-states';
import { driveJourneyXr, installJourneyXr, journeyXrState } from './fixtures/xr-session';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';
import { createScanIdentity } from '../../src/xr/scanIdentity';
import { decodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import type { WorkflowStep } from '../../src/domain/device';

for (const locale of ['en', 'uk'] as const) {
  test.describe(`operator goal ${locale}`, () => {
    test.use({ appLocale: locale });
    for (const scan of [true, false]) {
      test(`calibrate, ${scan ? 'use GPS and save a 3D scan' : 'explicitly skip GPS and 3D'}, record and calculate`, async ({ page, context, api }, info) => {
        test.setTimeout(120_000);
        const t = catalog[locale];
        await context.grantPermissions(['geolocation']);
        await context.setGeolocation({ latitude: 50.45, longitude: 30.52, accuracy: 4 });
        if (scan) await installJourneyXr(page);
        const workflow = api.status.workflow!;
        const nodes = ['MAIN', 'G1', 'G2', 'G4'];
        api.status.pods = [1, 2, 4].map(id => ({ ...structuredClone(api.status.pods[0]), id, state: 'READY' }));
        api.status.system.ready_pods = 3;
        workflow.assigned_remotes = 3;
        Object.assign(workflow.placement, { all_stable: false, per_node: nodes.map(node => ({
          node, vertical: true, height_rel_main_mm: 0, altitude_asl_m: 12.5,
          stable: false, reason: 'collecting', samples: 1, range_mm: null,
        })) });
        const enter = (step: WorkflowStep, actions = workflowActions[step]) => {
          Object.assign(workflow, { step, generation: workflow.generation + 1, allowed_actions: [...actions] });
        };
        enter('OBJECT');
        let scanBytes: Uint8Array | null = null;
        let savedPoints = 0;
        let occupancyBytes: Uint8Array | null = null;
        let occupancyScanId: string | null = null;
        const commands: string[] = [];
        api.overrides.set('POST /api/control', async route => {
          const query = new URL(route.request().url()).searchParams;
          if (query.has('scan')) {
            expect(query.get('object_id')).toBe(workflow.object_id);
            expect(query.get('session_id')).toBe('staging');
            const kind = query.get('kind') ?? 'scan';
            if (query.get('scan') === 'info') {
              await route.fulfill({ json: scanBytes === null || kind !== 'scan'
                ? { ok: false, error: 'ESP_ERR_NOT_FOUND' }
                : { ok: true, bytes: scanBytes.length, id: createScanIdentity(scanBytes).frameId } });
            } else {
              expect(query.get('scan')).toBe('save');
              const bytes = route.request().postDataBuffer()!;
              expect(bytes.byteLength).toBeGreaterThan(0);
              if (kind === 'scan') {
                scanBytes = new Uint8Array(bytes);
                const decoded = decodeScanArtifact(scanBytes);
                savedPoints = decoded.pointCount;
                expect(savedPoints, 'Finish must upload actual fused points, not an empty successful fixture').toBeGreaterThan(0);
                await info.attach('captured-scan', { body: bytes, contentType: 'application/octet-stream' });
                Object.assign(workflow, { has_scan: true, scan_upload_status: 'verified' });
              } else if (kind === 'occupancy') {
                occupancyBytes = new Uint8Array(bytes);
                occupancyScanId = query.get('scan_id');
              }
              await route.fulfill({ json: { ok: true } });
            }
            return;
          }
          const body = route.request().postDataJSON();
          expect(body.action).toBe('workflow_command');
          expect(body.generation).toBe(workflow.generation);
          if (body.command === 'GPS_ACCEPT') expect(workflow.step).toBe('GPS_PENDING');
          else if (body.command === 'SCAN_COMPLETE') expect(workflow.step).toBe('SCANNING');
          else expect(workflow.allowed_actions, `command must be allowed at ${workflow.step}`).toContain(body.command);
          commands.push(body.command);
          switch (body.command) {
            case 'START_MEASUREMENT': enter('SENSOR_CHECK'); break;
            case 'GO_CALIBRATION': enter('CALIB_PREP'); break;
            case 'SENSORS_PLACED': enter('CALIBRATING'); break;
            case 'PLACE_SENSORS': enter('PLACEMENT', ['BACK']); break;
            case 'PLACEMENT_CONFIRMED': enter('GPS_CHOICE'); break;
            case 'REQUEST_GPS': Object.assign(workflow, { gps_status: 'pending' }); enter('GPS_PENDING'); break;
            case 'GPS_ACCEPT':
              expect(body).toMatchObject({ latitude: 50.45, longitude: 30.52, accuracy_m: 4 });
              Object.assign(workflow, { gps_status: 'accepted' }); enter('LAYOUT_READY'); break;
            case 'GPS_SKIP': Object.assign(workflow, { gps_status: 'skipped' }); enter('LAYOUT_READY'); break;
            case 'CALCULATE_POSITIONS':
            case 'CALCULATE_POSITIONS_KEEP_WIFI':
              Object.assign(api.status.ranging!.progress!, { active: true, complete: false, success: false, pairs_done: 0, pairs_total: 12 });
              enter('RANGING'); break;
            case 'CONFIRM': enter('SCAN_CHOICE'); break;
            case 'SCAN_START': enter('SCANNING'); break;
            case 'SCAN_SKIP': enter('RECORD_READY'); break;
            case 'SCAN_COMPLETE':
              expect(scanBytes, 'workflow must not advance before scan upload').not.toBeNull();
              expect(savedPoints).toBeGreaterThan(0);
              enter('RECORD_READY'); break;
            case 'START_RECORDING':
              Object.assign(api.status.recording!, { running: true, state: 'RECORDING' });
              enter('RECORDING'); break;
            case 'STOP_RECORDING':
              Object.assign(api.status.recording!, { running: false, state: 'IDLE' });
              Object.assign(workflow, { calc_settings_ready: true });
              enter('STOPPED', ['CALCULATE_SOURCES', ...workflowActions.STOPPED]); break;
            case 'CALCULATE_SOURCES': enter('CALCULATING'); break;
            default: throw new Error(`Unexpected command ${body.command}`);
          }
          await route.fulfill({ json: { ok: true, workflow } });
        });
        let navigations = 0;
        page.on('framenavigated', frame => { if (frame === page.mainFrame()) navigations++; });
        const stage = async (step: WorkflowStep) => {
          await expect(page.locator('.workflow-page h2')).toHaveText(t[`workflow.title.${step.toLowerCase()}` as keyof typeof t]);
          await scenarioScreenshot(page, `${locale}-${scan ? 'primary' : 'skip'}-${step}`);
        };
        const click = async (command: string, step: WorkflowStep) => {
          await page.locator(`[data-command="${command}"]`).click(); await stage(step);
        };
        await page.goto('/');
        await page.getByRole('navigation').getByRole('button', { name: t['workflow.nav'], exact: true }).click();
        await click('START_MEASUREMENT', 'SENSOR_CHECK');
        await click('GO_CALIBRATION', 'CALIB_PREP');
        await click('SENSORS_PLACED', 'CALIBRATING');
        // Only asynchronous device work advances itself; subsequent steps require UI commands.
        Object.assign(workflow.calibration, { phase: 'complete', result: true,
          per_node: nodes.map(node => ({ node, height: { state: 'valid', relative_z_mm: 0 },
            imu: { state: 'calibrated', gyro_bias_mdps: [0, 0, 0], gravity_ref_mg: [0, 0, 1000] }, cs_pairs_ok: true })),
          cs: { pairs_done: 12, pairs_total: 12, max_spread_mm: 5 } });
        enter('CALIB_RESULT', ['PLACE_SENSORS', 'BACK']); await stage('CALIB_RESULT');
        await click('PLACE_SENSORS', 'PLACEMENT');
        await expect(page.locator('[data-command="PLACEMENT_CONFIRMED"]')).toBeDisabled();
        Object.assign(workflow.placement, { all_stable: true, per_node: workflow.placement.per_node.map(node => ({
          ...node, stable: true, reason: 'stable', samples: 4, range_mm: 3,
        })) });
        workflow.allowed_actions = [...workflowActions.PLACEMENT];
        await expect(page.locator('[data-command="PLACEMENT_CONFIRMED"]')).toBeEnabled();
        await click('PLACEMENT_CONFIRMED', 'GPS_CHOICE');
        // Both choices must exist regardless of the branch this particular operator takes.
        await expect(page.locator('[data-command="REQUEST_GPS"]')).toBeVisible();
        await expect(page.locator('[data-command="GPS_SKIP"]')).toBeVisible();
        await click(scan ? 'REQUEST_GPS' : 'GPS_SKIP', 'LAYOUT_READY');
        expect(workflow.gps_status).toBe(scan ? 'accepted' : 'skipped');
        // The user decides explicitly: Wi-Fi off during the measurement, or kept on. One branch of the journey takes each.
        await expect(page.locator('[data-command="CALCULATE_POSITIONS"]')).toHaveText(t['workflow.action.calculate_positions']);
        await expect(page.locator('[data-command="CALCULATE_POSITIONS_KEEP_WIFI"]')).toHaveText(t['workflow.action.calculate_positions_keep_wifi']);
        await expect(page.getByText(t['workflow.layout.wifi_choice'], { exact: true })).toBeVisible();
        await click(scan ? 'CALCULATE_POSITIONS' : 'CALCULATE_POSITIONS_KEEP_WIFI', 'RANGING');
        Object.assign(api.status.ranging!.progress!, { active: false, complete: true, success: true, pairs_done: 12 });
        Object.assign(workflow, { layout_result: true });
        enter('RANGING_RESULT', ['CONFIRM', 'SET_DISTANCE', 'BACK']); await stage('RANGING_RESULT');
        await click('CONFIRM', 'SCAN_CHOICE');
        await expect(page.locator('[data-command="SCAN_START"]')).toBeVisible();
        await expect(page.locator('[data-command="SCAN_SKIP"]')).toBeVisible();
        if (scan) {
          await page.locator('[data-command="SCAN_START"]').click();
          await expect(page.getByRole('heading', { name: t['scan.search_volume_title'], exact: true })).toBeVisible();
          const start = page.getByRole('button', { name: t['xr.start_scan'], exact: true });
          await expect(start).toBeEnabled();
          await scenarioScreenshot(page, `${locale}-primary-scanner-ready`);
          await start.click();
          await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
          await driveJourneyXr(page);
          await scenarioScreenshot(page, `${locale}-primary-scanner-running`);
          await page.locator('.xr-finish-button').click();
          await expect(page.locator('.console-floating-back')).toHaveText(t['console.continue_recording']);
          await page.locator('.console-floating-back').click();
          await stage('RECORD_READY');
          expect((await journeyXrState(page)).active).toBe(false);
        } else await click('SCAN_SKIP', 'RECORD_READY');
        await click('START_RECORDING', 'RECORDING');
        await click('STOP_RECORDING', 'STOPPED');
        await click('CALCULATE_SOURCES', 'CALCULATING');
        Object.assign(workflow, { calculation_class: 'none', progress: 100 });
        enter('RESULT'); await stage('RESULT');
        await expect(page.getByText(t['workflow.result.success'], { exact: true })).toBeVisible();
        await expect(page.locator('[data-command="OPEN_RECORDINGS"]'), 'the result leads to this object recordings').toBeVisible();
        expect(commands).toEqual(['START_MEASUREMENT', 'GO_CALIBRATION', 'SENSORS_PLACED', 'PLACE_SENSORS',
          'PLACEMENT_CONFIRMED', ...(scan ? ['REQUEST_GPS', 'GPS_ACCEPT'] : ['GPS_SKIP']),
          scan ? 'CALCULATE_POSITIONS' : 'CALCULATE_POSITIONS_KEEP_WIFI',
          'CONFIRM', ...(scan ? ['SCAN_START', 'SCAN_COMPLETE'] : ['SCAN_SKIP']), 'START_RECORDING', 'STOP_RECORDING', 'CALCULATE_SOURCES']);
        expect(navigations, 'all steps stay connected without reload or direct stage navigation').toBe(1);
        let occupancyEvidence: { free: number; occupied: number; frameId: string } | null = null;
        if (scan) {
          /* The solver input: the scan's FREE and OCCUPIED voxels, bound to exactly the uploaded scan. */
          await expect.poll(() => occupancyBytes !== null, { message: 'the occupancy artifact is uploaded with the scan' }).toBe(true);
          const decoded = decodeOccupancySolverArtifact(occupancyBytes!);
          const frameId = createScanIdentity(scanBytes!).frameId;
          expect(decoded.header.frame_id, 'occupancy is bound to the uploaded scan').toBe(frameId);
          expect(occupancyScanId, 'the upload names the scan it belongs to').toBe(frameId);
          expect(decoded.free.length / 3, 'scanned-empty voxels reach the device').toBeGreaterThan(0);
          expect(decoded.occupied.length / 3, 'scanned-solid voxels reach the device').toBeGreaterThan(0);
          expect(decoded.header.free_count).toBe(decoded.free.length / 3);
          expect(decoded.header.occupied_count).toBe(decoded.occupied.length / 3);
          expect(decoded.header.voxel_size_m, 'the ESP32 gets voxels no finer than 0.25 m').toBeGreaterThanOrEqual(0.25);
          occupancyEvidence = { free: decoded.header.free_count, occupied: decoded.header.occupied_count, frameId };
        } else {
          expect(occupancyBytes, 'a skipped scan uploads no occupancy').toBeNull();
        }
        await info.attach('goal-evidence', { contentType: 'application/json', body: JSON.stringify({
          goal: scan ? 'GPS + captured 3D + measurement result' : 'explicit bypass + measurement result',
          commands, savedPoints, occupancy: occupancyEvidence, gps: workflow.gps_status, navigations,
          boundary: 'Synthetic GPS/XR and HTTP device; actual production UI, fusion, encoding and WebGL; no physical ESP32/phone',
        }, null, 2) });
      });
    }
  });
}
