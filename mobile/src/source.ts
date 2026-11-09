// source.ts — one source repository per installed app (ADR 0017).
//
// Several repos may list the same package. The phone installs and updates an app only from the repo
// the user chose for it: installing an app from a repo chooses that repo, and moving an installed app
// to another repo needs the user's explicit confirmation. Without this, any repo that obtained a build
// signed with the developer's key (a mirror, a leaked or old build, a compromised repo) could take over
// updates silently, because across repos the highest versionCode used to win.
//
// Pure functions only (no React Native), so Node tests cover them.

/** package name → key of the repo it installs and updates from. */
export type Sources = Record<string, string>;

export type SourceState =
  | { kind: "chosen"; repo: string }        // the user's choice, and that repo still lists the app
  | { kind: "gone"; repo: string }          // the user's choice, but that repo no longer lists it (or was removed)
  | { kind: "only"; repo: string }          // not chosen yet, and exactly one repo lists it: that one
  | { kind: "ambiguous"; repos: string[] }  // not chosen yet, and several repos list it: the user must pick
  | { kind: "none" };                       // no repo lists it

/** Where `pkg` installs and updates from, given the repos (keys) that list it. */
export function sourceState(pkg: string, listedBy: string[], sources: Sources): SourceState {
  const chosen = sources[pkg];
  if (chosen) return listedBy.includes(chosen) ? { kind: "chosen", repo: chosen } : { kind: "gone", repo: chosen };
  if (listedBy.length === 1) return { kind: "only", repo: listedBy[0] };
  if (listedBy.length > 1) return { kind: "ambiguous", repos: [...listedBy] };
  return { kind: "none" };
}

/** The repo automatic updates for an installed `pkg` may come from, or null: none until the user picks. */
export function updateSource(pkg: string, listedBy: string[], sources: Sources): string | null {
  const st = sourceState(pkg, listedBy, sources);
  return st.kind === "chosen" || st.kind === "only" ? st.repo : null;
}

/**
 * May `repoKey` install `pkg` now?
 * - Not installed: yes. Tapping Install in a repo is choosing it.
 * - Installed: only from its source (see `updateSource`), unless the user confirmed the switch.
 *   An installed app that several repos list and nobody chose for also needs that confirmation.
 */
export function mayInstall(pkg: string, repoKey: string, installed: boolean, listedBy: string[], sources: Sources,
  confirmedSwitch = false): boolean {
  if (!installed || confirmedSwitch) return true;
  return updateSource(pkg, listedBy, sources) === repoKey;
}

/** Thrown by install() when an installed app would come from a repo other than its source. */
export class SourceMismatch extends Error {
  pkg: string; repo: string; current: SourceState;
  constructor(pkg: string, repo: string, current: SourceState) {
    super(current.kind === "ambiguous"
      ? `several repositories list ${pkg}: choose where it updates from first`
      : `${pkg} updates from another repository`);
    this.name = "SourceMismatch";
    this.pkg = pkg; this.repo = repo; this.current = current;
  }
}
