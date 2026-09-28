import type { I18nKey } from '../../i18n';

export const MATERIAL_IDS = [
  'concrete',
  'reinforced_concrete',
  'brick_masonry',
  'rubble',
  'steel',
  'soil_dry',
  'soil_saturated',
  'custom',
] as const;

export type MaterialId = typeof MATERIAL_IDS[number];

const MATERIAL_NAME_KEYS: Record<MaterialId, I18nKey> = {
  concrete: 'workflow.material.name.concrete',
  reinforced_concrete: 'workflow.material.name.reinforced_concrete',
  brick_masonry: 'workflow.material.name.brick_masonry',
  rubble: 'workflow.material.name.rubble',
  steel: 'workflow.material.name.steel',
  soil_dry: 'workflow.material.name.soil_dry',
  soil_saturated: 'workflow.material.name.soil_saturated',
  custom: 'workflow.material.name.custom',
};

export function materialNameKey(value: string | null | undefined): I18nKey {
  return value !== undefined && value !== null && Object.prototype.hasOwnProperty.call(MATERIAL_NAME_KEYS, value)
    ? MATERIAL_NAME_KEYS[value as MaterialId]
    : 'workflow.material.name.unknown';
}
