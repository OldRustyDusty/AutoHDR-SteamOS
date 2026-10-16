// SPDX-License-Identifier: MIT
//
// Turning HDR Ket on for a game means putting ENABLE_HDRKET=1 in front of
// %command% in its Steam launch options. These helpers only ever touch that
// one token, so whatever else the user has there is left alone.

export const TOKEN = "ENABLE_HDRKET=1";

const tokenPattern = /(^|\s)ENABLE_HDRKET=1(?=\s|$)/g;

export function hasHdrKet(options: string): boolean {
  return new RegExp(tokenPattern.source).test(options);
}

export function addHdrKet(options: string): string {
  const current = options.trim();
  if (hasHdrKet(current)) return current;
  if (!current) return `${TOKEN} %command%`;
  if (current.includes("%command%")) return `${TOKEN} ${current}`;
  // Plain arguments (no %command%) are passed to the game; keep them there.
  return `${TOKEN} %command% ${current}`;
}

export function removeHdrKet(options: string): string {
  const stripped = options.replace(tokenPattern, " ").replace(/\s+/g, " ").trim();
  // "%command%" alone means the same as no launch options; leave it empty.
  return stripped === "%command%" ? "" : stripped;
}
