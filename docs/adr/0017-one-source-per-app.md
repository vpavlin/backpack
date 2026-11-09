# ADR 0017: One source repository per installed app

Status: accepted (2026-11-09, vpavlin), Backpack 0.2.21

## Context

A phone can follow several repos, and several of them can list the same package: a developer's own repo and a mirror of it, a dev repo and a public one, two people publishing the same app.

Until now, ADR 0008 said: "Across repos, the highest `versionCode` with a matching key wins". Every repo's Install and Update buttons worked for every app. The signer checks (ADR 0002) stop a repo from replacing an app with one signed by a different key, but nothing stopped another repo that holds builds signed by the right key from taking over:

- a mirror or an old repo that keeps a build the developer withdrew,
- a repo that got a leaked or debug build signed with the release key,
- a compromised repo of a publisher the user never chose for that app.

Its higher `versionCode` would win, and with automatic updates on (the default), it would install without the user seeing anything. vpavlin: "if multiple repos serve a particular app we need to only explicitly accept installs from one (selected by user), otherwise this could become a security issue."

## Decision

**Each installed app has one source repo, and only the user picks it.**

- **Installing an app from a repo makes that repo its source.** Tapping Install in a repo's list is the choice. Backpack records `package → repo key` on the phone (`backpack.sources.v1`) when Android reports the install.
- **Updates come only from the source**, automatic and by hand. A newer version in another repo is ignored.
- **Not chosen yet** (apps installed before 0.2.21, or not by Backpack):
  - if exactly one followed repo lists the app, that repo is its source (nothing is ambiguous), and the first update from it records the choice;
  - if several repos list it, it gets **no updates** until the user picks one. A banner says how many apps are waiting, and the app's detail screen lists the repos.
- **Switching needs a confirmation.** The app's detail screen, under *Where it updates from*, shows the current source and a *Use this one* button for each other repo that lists the app. Tapping Update in a repo that isn't the source does the same. The dialog names both repos with their keys and warns that a repository can ship any build signed with the app's key. Automatic updates never switch.
- **A source that stops listing the app** (or is removed) is shown in red. The app gets no updates until the user picks another repo; no other repo takes over by itself.
- **Enforced in `install()`** (`mobile/src/repo.ts`), before anything is downloaded, so no path (automatic update, a tap, a link) can skip it. The rules are pure functions in `mobile/src/source.ts`, tested by `mobile/test/source.test.ts`.
- **Not installed:** any repo the user taps may install the app, and becomes its source. The detail screen still warns when another repo lists the same package under a different signing key (ADR 0008).

## Consequences

- One repo can no longer take over an app the user got from another, even with a validly signed build and a higher `versionCode`.
- Apps several repos list stop updating after the upgrade to 0.2.21 until the user picks a source for each. This is deliberate: Backpack never recorded which repo it installed them from, so guessing would be exactly the silent choice this ADR removes. The banner makes it visible.
- The choice lives on the phone only. Uninstalling an app keeps its record; installing it again from any repo replaces it.
- The Basecamp app (ADR 0011) only publishes and doesn't install, so nothing changes there.

## Supersedes

ADR 0008's "Across repos, the highest `versionCode` with a matching key wins".
