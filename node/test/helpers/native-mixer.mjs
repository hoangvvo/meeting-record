import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const native = require("node-gyp-build")(fileURLToPath(new URL("../../", import.meta.url)));

export function createMixer(sampleRate, sources = 1) {
  return new native.PcmMixer(sampleRate, sources);
}
