import { useEffect, useMemo } from 'react';

import { StructuralGuideClient } from './structuralGuideClient';

/**
 * The structural-guide worker client of one mounted overlay. React StrictMode (the dev server and the Phone Lab) runs every effect's cleanup and then its
 * setup again on the SAME memoized client, so a plain `useEffect(() => () => client.dispose(), [])` left the live client disposed for the rest of the scan:
 * every keyframe analysis then failed with "Structural guide worker disposed." (forty warnings in the 2026-11-26 run). Setup therefore re-activates the
 * client; only a real unmount leaves it disposed (TASK 06).
 */
export function useStructuralGuideClient(factory: () => StructuralGuideClient = () => new StructuralGuideClient()): StructuralGuideClient {
  const client = useMemo(factory, []); // eslint-disable-line react-hooks/exhaustive-deps -- one client per mounted overlay
  useEffect(() => {
    client.activate();
    return () => client.dispose();
  }, [client]);
  return client;
}
