import { describe, expect, it } from 'vitest';
import { applyConfirmedObjectMutation } from '../../src/features/objects/objectMutation';
import type { ObjectStoreState } from '../../src/domain/device';

function store(): ObjectStoreState {
  return {
    available: true,
    active_id: 'OBJ_0001',
    default_cable_lengths_m: [5, 5, 5, 5],
    objects: [
      {
        id: 'OBJ_0001',
        name: 'one',
        active: true,
        location: {
          valid: false,
          latitude: 0,
          longitude: 0,
          accuracy_m: 0,
          altitude_valid: false,
          altitude_m: 0,
          altitude_accuracy_m: 0,
          captured_at_ms: 0,
          source: '',
        },
        cable_lengths_m: [5, 5, 5, 5],
      },
      {
        id: 'OBJ_0002',
        name: 'two',
        active: false,
        location: {
          valid: false,
          latitude: 0,
          longitude: 0,
          accuracy_m: 0,
          altitude_valid: false,
          altitude_m: 0,
          altitude_accuracy_m: 0,
          captured_at_ms: 0,
          source: '',
        },
        cable_lengths_m: [5, 5, 5, 5],
      },
    ],
  };
}

const ok = (action: string, objectId?: string) => ({
  ok: true,
  action,
  ...(objectId === undefined ? {} : { object_id: objectId }),
});

describe('confirmed object mutation projection', () => {
  it('adds a newly created object and makes it active immediately', () => {
    const next = applyConfirmedObjectMutation(
      store(),
      'create_object',
      { name: 'three' },
      ok('create_object', 'OBJ_0003'),
    );

    expect(next?.active_id).toBe('OBJ_0003');
    expect(next?.objects.map((object) => [object.id, object.name, object.active])).toEqual([
      ['OBJ_0001', 'one', false],
      ['OBJ_0002', 'two', false],
      ['OBJ_0003', 'three', true],
    ]);
    expect(next?.objects[2].cable_lengths_m).toEqual([5, 5, 5, 5]);
  });

  it('updates an object cable without waiting for an SD reread', () => {
    const next = applyConfirmedObjectMutation(
      store(),
      'set_object_cable_length',
      { object_id: 'OBJ_0001', pod_id: 4, length_m: 20 },
      ok('set_object_cable_length'),
    );

    expect(next?.objects[0].cable_lengths_m).toEqual([5, 5, 5, 20]);
  });

  it('updates global cable defaults locally', () => {
    const next = applyConfirmedObjectMutation(
      store(),
      'set_default_cable_length',
      { pod_id: 2, length_m: 7.25 },
      ok('set_default_cable_length'),
    );

    expect(next?.default_cable_lengths_m).toEqual([5, 7.25, 5, 5]);
  });

  it('projects activate, rename and phone GPS mutations', () => {
    const activated = applyConfirmedObjectMutation(
      store(),
      'activate_object',
      { object_id: 'OBJ_0002' },
      ok('activate_object'),
    );
    expect(activated?.active_id).toBe('OBJ_0002');
    expect(activated?.objects.map((object) => object.active)).toEqual([false, true]);

    const renamed = applyConfirmedObjectMutation(
      activated,
      'rename_object',
      { object_id: 'OBJ_0002', name: 'renamed' },
      ok('rename_object'),
    );
    expect(renamed?.objects[1].name).toBe('renamed');

    const located = applyConfirmedObjectMutation(
      renamed,
      'set_object_location',
      {
        object_id: 'OBJ_0002',
        latitude: 50.45,
        longitude: 30.52,
        accuracy_m: 3.2,
        altitude_m: null,
        altitude_accuracy_m: null,
        captured_at_ms: 12345,
      },
      ok('set_object_location'),
    );
    expect(located?.objects[1].location).toMatchObject({
      valid: true,
      latitude: 50.45,
      longitude: 30.52,
      accuracy_m: 3.2,
      altitude_valid: false,
      captured_at_ms: 12345,
      source: 'PHONE_GPS',
    });
  });

  it('projects measurement_start activation like the legacy recording action', () => {
    const next = applyConfirmedObjectMutation(
      store(),
      'measurement_start',
      { object_id: 'OBJ_0002' },
      ok('measurement_start'),
    );

    expect(next?.active_id).toBe('OBJ_0002');
    expect(next?.objects.map((object) => object.active)).toEqual([false, true]);
  });

  it('clears active state when the active object is deleted', () => {
    const next = applyConfirmedObjectMutation(
      store(),
      'delete_object',
      { object_id: 'OBJ_0001' },
      ok('delete_object'),
    );

    expect(next?.active_id).toBeNull();
    expect(next?.objects).toHaveLength(1);
    expect(next?.objects[0]).toMatchObject({ id: 'OBJ_0002', active: false });
  });
});
