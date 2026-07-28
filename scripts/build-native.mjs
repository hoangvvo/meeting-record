import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

if (process.platform === "win32") {
  console.log("Windows native sources are compiled by node-gyp or Cargo");
  process.exit(0);
}

if (process.platform !== "darwin") {
  console.error(`unsupported platform: ${process.platform}`);
  process.exit(1);
}

const result = spawnSync(path.join(root, "scripts", "build-macos.sh"), {
  cwd: root,
  stdio: "inherit",
});
process.exit(result.status ?? 1);
