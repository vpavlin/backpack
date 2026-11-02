// loam-setup.ts — offer to install Loam (Backpack ADR 0016).
// Without Loam, Backpack hears about repo updates only through repos' web addresses, which are off by
// default: no updates at all. Loam brings Logos Delivery (live and cached update notes) and shares one
// Storage cache with the other Logos apps. Backpack keeps its own Storage so it can fetch Loam itself.
// Loam comes from the newest version that any of your repos lists, else from the built-in source below.
// Either way only an APK signed by Loam's key (pinned here) is offered, and install() checks it like
// any other app (sha256 from the signed index, signer, package, versionCode).
import AsyncStorage from "@react-native-async-storage/async-storage";
import * as R from "./repo";

export const LOAM_PKG = "xyz.vpavlin.loam";
export const LOAM_SIGNER = "eb57a553a5e24b5906debfdcf143e243996b9d4149649e9a1aac7674b02b5e2f";
// vpavlin's public repo as of this build (no read key: public). Its index CID is the list of that time;
// a newer one is found through Delivery (once Loam runs) or, as a fallback, its web address.
export const LOAM_SOURCE_LINK = "backpack://repo?i=zDvZRwzm4josYAp3dmBtdW9CQr46QjQBzuUNdwuqZVHmzrP9rkmR&k=0xO-S-MKLKmOdka27NdqrJbdo9HwRiItD-9F4mo6LSs&n=vpavlin%27s+apps&p=16Uiu2HAmKRRjSxvkNWM7iHbXuSANCrrLmopXWtQshSDuFQ4YqHuG&a=%2Fip4%2F128.140.55.128%2Ftcp%2F8499&s=spr%3ACiUIAhIhA2SIYHEuMLRVQoEhBPMJTU-tZnWY78f4BaK4BOCKao4nEgIDARo7CicAJQgCEiEDZIhgcS4wtFVCgSEE8wlNT61mdZjvx_gForgE4IpqjicQlOKh1wYaCgoIBICMN4AGITMqRjBEAiAGrNmlvDrOQYdCDTXMq8bOUa3ngNmZF96Kzysx20awPgIgfh931pt3MWuC1DogpnyVhu61nqrWCQltIP0abbi15wI&u=https%3A%2F%2Fdata.vpavlin.xyz%2Fbackpack%2F0xO-S-MKLKmOdka27NdqrJbdo9HwRiItD-9F4mo6LSs.json";

const ASKED = "backpack.loamAsked.v1";

/** Is Loam installed? (true when this can't be told, so nothing is offered by mistake) */
export async function loamInstalled(): Promise<boolean> {
  if (!R.installer.available()) return true;
  try { return (await R.installer.installedVersion(LOAM_PKG)) > 0; } catch { return true; }
}
/** Ask once, on a phone without Loam. */
export async function shouldAsk(): Promise<boolean> {
  try { if (await AsyncStorage.getItem(ASKED)) return false; } catch { /* ask */ }
  return !(await loamInstalled());
}
export async function markAsked(): Promise<void> { try { await AsyncStorage.setItem(ASKED, new Date().toISOString()); } catch { /* */ } }

function loamIn(repo: R.Repo): R.App | null {
  const a = repo.index?.apps.find((x) => x.id === LOAM_PKG);
  if (!a) return null;
  const versions = a.versions.filter((v) => v.signer === LOAM_SIGNER);
  return versions.length ? { ...a, versions } : null;
}

/**
 * Where to get Loam: the newest genuine Loam in your repos, else the built-in source's list (fetched with
 * Backpack's own Storage). `web`: also ask the source's web address for a newer list (the fallback when
 * the built-in list can't be fetched, e.g. its CID is gone).
 */
export async function findLoam(repos: R.Repo[], onStage?: (s: string) => void, web = false): Promise<{ repo: R.Repo; app: R.App }> {
  let best: { repo: R.Repo; app: R.App } | null = null;
  for (const r of repos) {
    const a = loamIn(r);
    if (a && (!best || a.versions[0].versionCode > best.app.versions[0].versionCode)) best = { repo: r, app: a };
  }
  if (best) return best;
  const src = await R.refresh(R.parseLink(LOAM_SOURCE_LINK), onStage, undefined, { web });
  const a = loamIn(src);
  if (!a) throw new Error("the built-in source doesn't list Loam signed by its key");
  return { repo: src, app: a };
}
