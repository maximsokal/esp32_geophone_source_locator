import type { ControlResult } from '../../api/deviceApi';
import type {
  ObjectLocation,
  ObjectProfile,
  ObjectStoreState,
} from '../../domain/device';
import { normalizeCableLength } from '../../domain/device';

function payloadString(payload: Record<string, unknown>, key: string): string | null {
  const value = payload[key];
  return typeof value === 'string' && value.length > 0 ? value : null;
}

function payloadNumber(payload: Record<string, unknown>, key: string): number | null {
  const value = payload[key];
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function emptyLocation(): ObjectLocation {
  return {
    valid: false,
    latitude: 0,
    longitude: 0,
    accuracy_m: 0,
    altitude_valid: false,
    altitude_m: 0,
    altitude_accuracy_m: 0,
    captured_at_ms: 0,
    source: '',
  };
}

function activateObject(store: ObjectStoreState, objectId: string): ObjectStoreState {
  if (!store.objects.some((object) => object.id === objectId)) return store;
  return {
    ...store,
    active_id: objectId,
    objects: store.objects.map((object) => ({
      ...object,
      active: object.id === objectId,
    })),
  };
}

function createObject(
  store: ObjectStoreState,
  payload: Record<string, unknown>,
  result: ControlResult,
): ObjectStoreState {
  const objectId = result.object_id ?? null;
  if (objectId === null) return store;

  const suppliedName = payloadString(payload, 'name')?.trim();
  const existing = store.objects.find((object) => object.id === objectId) ?? null;
  const created: ObjectProfile = existing === null
    ? {
        id: objectId,
        name: suppliedName || objectId,
        active: true,
        location: emptyLocation(),
        cable_lengths_m: [...store.default_cable_lengths_m],
      }
    : {
        ...existing,
        name: suppliedName || existing.name,
        active: true,
      };

  return {
    ...store,
    active_id: objectId,
    objects: [
      ...store.objects
        .filter((object) => object.id !== objectId)
        .map((object) => ({ ...object, active: false })),
      created,
    ],
  };
}

function deleteObject(store: ObjectStoreState, objectId: string): ObjectStoreState {
  const deletingActive = store.active_id === objectId
    || store.objects.some((object) => object.id === objectId && object.active);
  const objects = store.objects
    .filter((object) => object.id !== objectId)
    .map((object) => deletingActive ? { ...object, active: false } : object);

  return {
    ...store,
    active_id: deletingActive ? null : store.active_id,
    objects,
  };
}

function updateObject(
  store: ObjectStoreState,
  objectId: string,
  updater: (object: ObjectProfile) => ObjectProfile,
): ObjectStoreState {
  if (!store.objects.some((object) => object.id === objectId)) return store;
  return {
    ...store,
    objects: store.objects.map((object) => object.id === objectId ? updater(object) : object),
  };
}

export function applyConfirmedObjectMutation(
  store: ObjectStoreState | null,
  action: string,
  payload: Record<string, unknown>,
  result: ControlResult,
): ObjectStoreState | null {
  if (store === null || store.available === false || result.ok !== true) return store;

  if (action === 'create_object') {
    return createObject(store, payload, result);
  }

  const objectId = payloadString(payload, 'object_id');

  if (action === 'activate_object' || action === 'measurement_start' || action === 'recording_start') {
    return objectId === null ? store : activateObject(store, objectId);
  }

  if (action === 'delete_object') {
    return objectId === null ? store : deleteObject(store, objectId);
  }

  if (action === 'rename_object') {
    const name = payloadString(payload, 'name')?.trim();
    if (objectId === null || !name) return store;
    return updateObject(store, objectId, (object) => ({ ...object, name }));
  }

  if (action === 'set_object_cable_length') {
    const podId = payloadNumber(payload, 'pod_id');
    const length = payloadNumber(payload, 'length_m');
    if (objectId === null || podId === null || length === null) return store;
    const index = Math.round(podId) - 1;
    if (index < 0 || index >= 4 || podId !== Math.round(podId)) return store;

    return updateObject(store, objectId, (object) => {
      const cableLengths = [...object.cable_lengths_m];
      cableLengths[index] = normalizeCableLength(length);
      return { ...object, cable_lengths_m: cableLengths };
    });
  }

  if (action === 'set_default_cable_length') {
    const podId = payloadNumber(payload, 'pod_id');
    const length = payloadNumber(payload, 'length_m');
    if (podId === null || length === null) return store;
    const index = Math.round(podId) - 1;
    if (index < 0 || index >= 4 || podId !== Math.round(podId)) return store;
    const defaults = [...store.default_cable_lengths_m];
    defaults[index] = normalizeCableLength(length);
    return { ...store, default_cable_lengths_m: defaults };
  }

  if (action === 'set_object_location') {
    const latitude = payloadNumber(payload, 'latitude');
    const longitude = payloadNumber(payload, 'longitude');
    const accuracy = payloadNumber(payload, 'accuracy_m');
    const capturedAt = payloadNumber(payload, 'captured_at_ms');
    if (
      objectId === null
      || latitude === null
      || longitude === null
      || accuracy === null
      || capturedAt === null
    ) return store;

    const altitude = payloadNumber(payload, 'altitude_m');
    const altitudeAccuracy = payloadNumber(payload, 'altitude_accuracy_m');
    const location: ObjectLocation = {
      valid: true,
      latitude,
      longitude,
      accuracy_m: accuracy,
      altitude_valid: altitude !== null,
      altitude_m: altitude ?? 0,
      altitude_accuracy_m: altitudeAccuracy ?? 0,
      captured_at_ms: capturedAt,
      source: 'PHONE_GPS',
    };
    return updateObject(store, objectId, (object) => ({ ...object, location }));
  }

  return store;
}
