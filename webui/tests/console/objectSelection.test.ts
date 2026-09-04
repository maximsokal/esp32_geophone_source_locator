import { describe, expect, it } from 'vitest';
import type { ObjectProfile, ObjectStoreState } from '../../src/domain/device';
import { reconcileObjectSelection } from '../../src/features/objects/objectSelection';

function profile(id: string, active = false): ObjectProfile {
  return {
    id,
    name: id,
    active,
    location: {
      valid: false,
      latitude: 0,
      longitude: 0,
      accuracy_m: 0,
      altitude_valid: false,
      altitude_m: 0,
      altitude_accuracy_m: 0,
      captured_at_ms: 0,
      source: 'NONE',
    },
    cable_lengths_m: [5, 5, 5, 5],
  };
}

function store(activeId: string, ids: string[]): ObjectStoreState {
  return {
    available: true,
    active_id: activeId,
    default_cable_lengths_m: [5, 5, 5, 5],
    objects: ids.map((id) => profile(id, id === activeId)),
  };
}

describe('object selection reconciliation', () => {
  it('preserves an explicitly selected inactive object during ordinary refreshes', () => {
    const next = store('OBJ_0007', ['OBJ_0006', 'OBJ_0007']);
    expect(reconcileObjectSelection(next, 'OBJ_0006')).toBe('OBJ_0006');
  });

  it('selects the object returned by create even when the previous selection still exists', () => {
    const next = store('OBJ_0007', ['OBJ_0006', 'OBJ_0007']);
    expect(reconcileObjectSelection(next, 'OBJ_0006', 'OBJ_0007')).toBe('OBJ_0007');
  });

  it('follows the active object when reconciling an ambiguous create response', () => {
    const next = store('OBJ_0007', ['OBJ_0006', 'OBJ_0007']);
    expect(reconcileObjectSelection(next, 'OBJ_0006', null, 'active')).toBe('OBJ_0007');
  });

  it('falls back to the active object when the selected object disappeared', () => {
    const next = store('OBJ_0007', ['OBJ_0007']);
    expect(reconcileObjectSelection(next, 'OBJ_0006')).toBe('OBJ_0007');
  });
});
