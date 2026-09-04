import type { ObjectStoreState } from '../../domain/device';

export type ObjectSelectionMode = 'preserve' | 'active';

export function reconcileObjectSelection(
  store: ObjectStoreState | null,
  currentId: string | null,
  preferredId: string | null = null,
  mode: ObjectSelectionMode = 'preserve',
): string | null {
  if (store === null || store.objects.length === 0) return null;

  const contains = (id: string): boolean => store.objects.some((object) => object.id === id);

  if (preferredId !== null && contains(preferredId)) {
    return preferredId;
  }

  const activeId = store.active_id !== null && contains(store.active_id)
    ? store.active_id
    : store.objects.find((object) => object.active)?.id ?? null;

  if (mode === 'active' && activeId !== null) {
    return activeId;
  }

  if (currentId !== null && contains(currentId)) {
    return currentId;
  }

  return activeId ?? store.objects[0]?.id ?? null;
}
