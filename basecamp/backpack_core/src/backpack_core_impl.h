#pragma once
// BackpackCoreImpl - Backpack (ADR 0011) as a Logos CORE module: manage the Backpack repos this computer
// publishes - create, add APKs, publish to Logos Storage, verify, invite links, private repos (ADR 0009).
// The repos are the CLI's (~/.backpack/<repo>), so cli/backpack.mjs and this module can take turns.
//
// Rules (logos-basecamp-module): public methods return std::string JSON; read state through the
// snapshot() action (the view polls it); <=4 args, structured input as one JSON string; no trailing
// comments on declaration lines; ASCII only. Slow work (uploads) runs on worker threads as JOBS; the
// view follows them in snapshot().
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "logos_module_context.h"

class BackpackCoreImpl : public LogosModuleContext {
public:
    ~BackpackCoreImpl() override;

    // Everything the view shows: settings, repos (without secrets), jobs.
    std::string snapshot();
    std::string version();

    // Where the repos live (default ~/.backpack, shared with the CLI).
    std::string setDataDir(std::string path);

    // Repos. privateFlag: "1" = private (ADR 0009). node: the Storage node's REST address that hosts
    // the repo's files (no built-in default); remembered as the suggestion for the next repo.
    std::string createRepo(std::string slug, std::string name, std::string privateFlag, std::string node);
    // Member repos (ADR 0013; privateFlag "2" in createRepo): add a phone by its member code
    // (backpack://member?pk=...), remove one by its key. Takes effect at the next publish.
    std::string addMember(std::string slug, std::string memberLink);
    std::string removeMember(std::string slug, std::string devicePk);
    std::string setLez(std::string slug, std::string configJson);
    std::string syncLez(std::string slug);
    // Does a Storage node answer? {"ok":true,"peerId":..,"addrs":[..],"warning":".."}
    std::string testNode(std::string url);
    std::string repo(std::string slug);
    // The invite link and its QR matrix. A private repo's link carries the read key.
    std::string link(std::string slug);
    std::string makePrivate(std::string slug);
    std::string rotateReadKey(std::string slug);
    // {"node":url, "pointersGet":[...], "pointersPut":[...]} (fields optional)
    std::string configure(std::string slug, std::string patchJson);
    std::string removeVersion(std::string slug, std::string appId, std::string versionCode);

    // APK signing keys, one per app (package), in <data dir>/signing. The key is the app's identity:
    // every later version must be signed with it, so export a backup.
    std::string keys();
    std::string createKey(std::string package, std::string name);
    std::string importKey(std::string package, std::string p12Path, std::string password);
    std::string exportKey(std::string package, std::string p12Path, std::string password);

    // Jobs (run in the background; follow them in snapshot().jobs). An unsigned APK is signed with its
    // package's key when there is one.
    std::string addApk(std::string slug, std::string apkPath);
    std::string publish(std::string slug, std::string force);
    std::string verify(std::string slug, std::string deep);
    std::string importFdroid(std::string slug, std::string repoUrl, std::string fingerprint, std::string versions);
    std::string clearJobs();

protected:
    void onContextReady() override;

private:
    struct Job {
        int id = 0; std::string kind, repo, state = "running", error;
        std::vector<std::string> log; std::string result;
        std::string progressWhat; long long done = 0, total = 0;
    };
    std::string startJob(const std::string& kind, const std::string& slug, std::function<std::string(Job&)> work);
    std::string repoDir(const std::string& slug) const;
    bool validSlug(const std::string& slug) const;

    std::recursive_mutex m_mtx;
    std::string m_dataDir;
    std::string m_lastNode;
    void loadPrefs();
    void savePrefs();
    std::map<int, std::shared_ptr<Job>> m_jobs;
    std::map<std::string, int> m_busyRepo;
    int m_nextJob = 1;
};
