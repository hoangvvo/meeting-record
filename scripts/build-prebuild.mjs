import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const args = [require.resolve("prebuildify/bin.js"), "--napi"];
// A glibc binary must never be selected on a musl host.
if (process.platform === "linux") args.push("--tag-libc");
const result = spawnSync(process.execPath, args, { stdio: "inherit" });
process.exit(result.status ?? 1);
