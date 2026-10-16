// Run: node --experimental-strip-types src/launchOptions.test.ts
import { addHdrKet, hasHdrKet, removeHdrKet } from "./launchOptions.ts";

const cases: [string, string, string][] = [
  // [original, after add, after add+remove]
  ["", "ENABLE_HDRKET=1 %command%", ""],
  ["%command%", "ENABLE_HDRKET=1 %command%", ""],
  ["-novid -high", "ENABLE_HDRKET=1 %command% -novid -high", "%command% -novid -high"],
  ["PROTON_ENABLE_WAYLAND=1 %command% -dx11", "ENABLE_HDRKET=1 PROTON_ENABLE_WAYLAND=1 %command% -dx11",
   "PROTON_ENABLE_WAYLAND=1 %command% -dx11"],
  ["gamemoderun %command%", "ENABLE_HDRKET=1 gamemoderun %command%", "gamemoderun %command%"],
  ["ENABLE_HDRKET=1 %command%", "ENABLE_HDRKET=1 %command%", ""],
  ["  DXVK_HDR=1   %command%  ", "ENABLE_HDRKET=1 DXVK_HDR=1   %command%", "DXVK_HDR=1 %command%"],
  ["XENABLE_HDRKET=1 %command%", "ENABLE_HDRKET=1 XENABLE_HDRKET=1 %command%", "XENABLE_HDRKET=1 %command%"],
];

let failed = 0;
for (const [orig, added, removed] of cases) {
  const a = addHdrKet(orig);
  const r = removeHdrKet(a);
  const ok = a === added && r === removed && hasHdrKet(a) && !hasHdrKet(r) && addHdrKet(a) === a;
  if (!ok) {
    failed++;
    console.log(`FAIL ${JSON.stringify(orig)}\n  add    -> ${JSON.stringify(a)} (want ${JSON.stringify(added)})\n  remove -> ${JSON.stringify(r)} (want ${JSON.stringify(removed)})`);
  }
}
console.log(failed ? `${failed} of ${cases.length} failed` : `all ${cases.length} launch-option cases pass`);
process.exit(failed ? 1 : 0);
