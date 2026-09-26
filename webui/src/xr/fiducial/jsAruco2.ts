import arucoSource from 'js-aruco2/src/aruco.js?raw';
import cvSource from 'js-aruco2/src/cv.js?raw';
import aprilTagDictionarySource from 'js-aruco2/src/dictionaries/apriltag_36h11.js?raw';

export interface ArucoPoint { x: number; y: number; }
export interface ArucoMarker { id: number; corners: ArucoPoint[]; hammingDistance: number; }
interface ArucoApi {
  AR: {
    DICTIONARIES: Record<string, { nBits: number; tau: number; codeList: number[] }>;
    Dictionary: new (name: string) => { codeList: number[]; _hex2bin(code: number, bits: number): string };
    Detector: new (options: { dictionaryName: string; maxHammingDistance: number }) => {
      detectImage(width: number, height: number, rgba: Uint8Array): ArucoMarker[];
    };
  };
}

type CommonJsModule = { exports: Record<string, unknown> };
type CommonJsRequire = (path: string) => CommonJsModule['exports'];

// The upstream files use classic CommonJS `this` assignments instead of ESM
// exports. Run those package sources with the same module-local `this` and
// `require` they expect; keeping this shim here works in Node tests and inline
// Vite workers without editing the dependency.
function loadCommonJs(source: string, require: CommonJsRequire, initialExports: Record<string, unknown> = {}): CommonJsModule['exports'] {
  const module: CommonJsModule = { exports: initialExports };
  const execute = new Function('require', 'module', 'exports', source) as (
    this: Record<string, unknown>,
    require: CommonJsRequire,
    module: CommonJsModule,
    exports: Record<string, unknown>,
  ) => void;
  execute.call(module.exports, require, module, module.exports);
  return module.exports;
}

const cv = loadCommonJs(cvSource, () => { throw new Error('Unexpected js-aruco2 cv dependency'); });
const aruco = loadCommonJs(
  arucoSource,
  (path) => path === './cv' ? cv : (() => { throw new Error(`Unexpected js-aruco2 dependency: ${path}`); })(),
  { CV: cv.CV },
);
const dictionary = loadCommonJs(
  aprilTagDictionarySource,
  (path) => path === '../aruco' ? aruco : (() => { throw new Error(`Unexpected js-aruco2 dependency: ${path}`); })(),
  { AR: aruco.AR },
);

export const AR = dictionary.AR as ArucoApi['AR'];
