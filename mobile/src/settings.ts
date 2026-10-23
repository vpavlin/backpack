// settings.ts — user choices, kept on the device.
import AsyncStorage from "@react-native-async-storage/async-storage";

export interface Settings {
  /** After checking repos, download and install updates for installed apps by itself (ADR 0008). */
  autoInstall: boolean;
  /** Also ask the repos' HTTPS URLs for the update note (ADR 0003). Off: Logos Delivery only (ADR 0007).
   *  Only the small signed note ever comes over HTTP; the index and the APKs always come from Storage. */
  webPointers: boolean;
}
export const DEFAULTS: Settings = { autoInstall: true, webPointers: false };
const KEY = "backpack.settings.v1";
let current: Settings = { ...DEFAULTS };

export async function load(): Promise<Settings> {
  try { current = { ...DEFAULTS, ...JSON.parse((await AsyncStorage.getItem(KEY)) ?? "{}") }; } catch { current = { ...DEFAULTS }; }
  return current;
}
export function get(): Settings { return current; }
export async function set(patch: Partial<Settings>): Promise<Settings> {
  current = { ...current, ...patch };
  await AsyncStorage.setItem(KEY, JSON.stringify(current));
  return current;
}
