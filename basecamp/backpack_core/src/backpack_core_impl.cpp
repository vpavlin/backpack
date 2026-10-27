// backpack_core_impl.cpp - see backpack_core_impl.h and docs/adr/0011-basecamp-app.md.
#include "backpack_core_impl.h"
#include "bp_fdroid.hpp"
#include "bp_keys.hpp"
#include "qrcodegen.hpp"
#include <cstdlib>

using bp::json;
namespace {
const char* VERSION = "0.2.0";
std::string ok(json j = json::object()) { j["ok"] = true; return j.dump(); }
std::string fail(const std::string& why) { return json{{"ok", false}, {"error", why}}.dump(); }
json qrMatrix(const std::string& text) {
    auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    int n = qr.getSize(); json cells = json::array();
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) cells.push_back(qr.getModule(x, y));
    return json{{"n", n}, {"cells", cells}};
}
}  // namespace

BackpackCoreImpl::~BackpackCoreImpl() = default;   // job threads are detached; the module lives as long as the process

void BackpackCoreImpl::onContextReady() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (const char* d = std::getenv("BACKPACK_DIR")) m_dataDir = d;
    else if (const char* h = std::getenv("HOME")) m_dataDir = std::string(h) + "/.backpack";
    else m_dataDir = ".backpack";
    loadPrefs();
}

// app preferences beside the repos: the last Storage node used (a suggestion, never a default in code)
void BackpackCoreImpl::loadPrefs() {
    try { json p = json::parse(bp::readFile(m_dataDir + "/backpack-app.json")); m_lastNode = p.value("lastNode", ""); } catch (...) {}
}
void BackpackCoreImpl::savePrefs() {
    try { std::error_code ec; bp::fs::create_directories(m_dataDir, ec); bp::writeFileAtomic(m_dataDir + "/backpack-app.json", json{{"lastNode", m_lastNode}}.dump(2), 0644); } catch (...) {}
}

std::string BackpackCoreImpl::testNode(std::string url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return fail("The node's REST address starts with http:// or https://");
    // a blocking HTTP call: run it on a short-lived thread so the module thread never waits on the network
    json res; std::thread([&]() {
        try { json ni = bp::Node{url}.info(); res = json{{"ok", true}, {"peerId", ni["peerId"]}, {"addrs", ni["addrs"]}, {"warning", bp::nodeWarning(url)}}; }
        catch (std::exception& e) { res = json{{"ok", false}, {"error", std::string("No answer from that node: ") + e.what()}}; }
    }).join();
    return res.dump();
}

std::string BackpackCoreImpl::version() { return json{{"ok", true}, {"version", VERSION}}.dump(); }

bool BackpackCoreImpl::validSlug(const std::string& s) const {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    return true;
}
std::string BackpackCoreImpl::repoDir(const std::string& slug) const { return m_dataDir + "/" + slug; }

std::string BackpackCoreImpl::setDataDir(std::string path) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (path.empty()) return fail("Choose a folder");
    std::error_code ec; bp::fs::create_directories(path, ec);
    if (ec) return fail("Can't use " + path + ": " + ec.message());
    m_dataDir = path; return ok(json{{"dataDir", m_dataDir}});
}

std::string BackpackCoreImpl::snapshot() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json repos = json::array();
    std::error_code ec;
    if (bp::fs::is_directory(m_dataDir, ec)) {
        std::vector<std::string> slugs;
        for (auto& e : bp::fs::directory_iterator(m_dataDir, ec)) if (e.is_directory() && bp::fs::exists(e.path() / "repo.json") && bp::fs::exists(e.path() / "key.pem")) slugs.push_back(e.path().filename().string());
        std::sort(slugs.begin(), slugs.end());
        for (auto& s : slugs) {
            try { json r = bp::Repo::load(repoDir(s)).summary(); r["busy"] = m_busyRepo.count(s) > 0; r.erase("apps"); r["appCount"] = bp::Repo::load(repoDir(s)).s["apps"].size(); repos.push_back(r); }
            catch (std::exception& e) { repos.push_back(json{{"slug", s}, {"error", e.what()}}); }
        }
    }
    json jobs = json::array();
    for (auto it = m_jobs.rbegin(); it != m_jobs.rend(); ++it) {
        auto& j = *it->second;
        json log = json::array(); size_t from = j.log.size() > 40 ? j.log.size() - 40 : 0; for (size_t i = from; i < j.log.size(); i++) log.push_back(j.log[i]);
        jobs.push_back(json{{"id", j.id}, {"kind", j.kind}, {"repo", j.repo}, {"state", j.state}, {"error", j.error}, {"log", log},
                            {"progress", json{{"what", j.progressWhat}, {"done", j.done}, {"total", j.total}}}, {"result", j.result.empty() ? json() : json::parse(j.result, nullptr, false)}});
    }
    return ok(json{{"version", VERSION}, {"dataDir", m_dataDir}, {"lastNode", m_lastNode}, {"repos", repos}, {"jobs", jobs}});
}

std::string BackpackCoreImpl::createRepo(std::string slug, std::string name, std::string privateFlag, std::string node) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug)) return fail("A short id: letters, digits, - and _ (it names the folder)");
    if (name.empty() || name.size() > 100) return fail("Give the repo a name (at most 100 characters)");
    if (!node.empty() && node.rfind("http://", 0) != 0 && node.rfind("https://", 0) != 0) return fail("The Storage node is its REST address: http(s)://...");
    try {
        bp::Repo r = bp::Repo::create(repoDir(slug), name, privateFlag == "1", node);
        if (!node.empty()) { m_lastNode = node; savePrefs(); }
        return ok(json{{"repo", r.summary()}});
    }
    catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::repo(std::string slug) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug)) return fail("No such repo");
    try { json r = bp::Repo::load(repoDir(slug)).summary(); r["busy"] = m_busyRepo.count(slug) > 0; return ok(json{{"repo", r}}); }
    catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::link(std::string slug) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug)) return fail("No such repo");
    try {
        bp::Repo r = bp::Repo::load(repoDir(slug));
        if (!r.s.contains("lastLink")) return fail("Publish the repo once: the link names its first index");
        std::string l = r.s["lastLink"];
        return ok(json{{"link", l}, {"private", !r.readKey().empty()}, {"qr", qrMatrix(l)}});
    } catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::makePrivate(std::string slug) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try { bp::Repo r = bp::Repo::load(repoDir(slug)); if (!r.readKey().empty()) return fail("Already private"); r.makePrivate(false); return ok(json{{"repo", r.summary()}}); }
    catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::rotateReadKey(std::string slug) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try { bp::Repo r = bp::Repo::load(repoDir(slug)); r.makePrivate(true); return ok(json{{"repo", r.summary()}}); }
    catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::configure(std::string slug, std::string patchJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try {
        json p = json::parse(patchJson);
        bp::Repo r = bp::Repo::load(repoDir(slug));
        if (p.contains("node")) { std::string n = p["node"]; if (n.rfind("http://", 0) != 0 && n.rfind("https://", 0) != 0) return fail("The Storage node is an http(s) URL"); r.s["node"] = n; m_lastNode = n; savePrefs(); }
        if (p.contains("name")) { std::string n = p["name"]; if (n.empty() || n.size() > 100) return fail("A name of 1-100 characters"); r.s["name"] = n; }
        for (auto side : {"Get", "Put"}) {
            std::string k = std::string("pointers") + side;
            if (!p.contains(k)) continue;
            bp::ojson arr = bp::ojson::array();
            for (auto& u : p[k]) {
                std::string s = u;
                if (std::string(side) == "Get" && s.rfind("https://", 0) != 0) return fail("Pointer addresses phones read must be https");
                if (std::string(side) == "Put" && !(s.rfind("rsync:", 0) == 0 || s.rfind("exec:", 0) == 0 || s.rfind("put:", 0) == 0)) return fail("Pointer targets are rsync:, exec: or put:");
                arr.push_back(s);
            }
            r.s["pointers"][side == std::string("Get") ? "get" : "put"] = arr;
        }
        r.save(); return ok(json{{"repo", r.summary()}});
    } catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::removeVersion(std::string slug, std::string appId, std::string versionCode) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try {
        bp::Repo r = bp::Repo::load(repoDir(slug));
        if (!r.s["apps"].contains(appId)) return fail("No such app");
        auto& app = r.s["apps"][appId]; long long vc = std::atoll(versionCode.c_str());
        std::vector<bp::ojson> keep; for (auto& v : app["versions"]) if (v["versionCode"].get<long long>() != vc) keep.push_back(v);
        if (keep.empty()) r.s["apps"] = bp::without(r.s["apps"], {appId.c_str()});
        else app["versions"] = bp::ojson(keep);
        r.save(); return ok(json{{"repo", r.summary()}, {"note", "Publish to make it visible"}});
    } catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::startJob(const std::string& kind, const std::string& slug, std::function<std::string(Job&)> work) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug)) return fail("No such repo");
    if (m_busyRepo.count(slug)) return fail("This repo is busy with another job");
    auto j = std::make_shared<Job>(); j->id = m_nextJob++; j->kind = kind; j->repo = slug;
    m_jobs[j->id] = j; m_busyRepo[slug] = j->id;
    while (m_jobs.size() > 30) m_jobs.erase(m_jobs.begin());
    std::thread([this, j, slug, work]() {
        std::string result, error;
        try { result = work(*j); } catch (std::exception& e) { error = e.what(); } catch (...) { error = "unexpected error"; }
        std::lock_guard<std::recursive_mutex> lk2(m_mtx);
        j->result = result; j->error = error; j->state = error.empty() ? "done" : "failed";
        if (!error.empty()) j->log.push_back("FAILED: " + error);
        m_busyRepo.erase(slug);
    }).detach();
    return ok(json{{"job", j->id}});
}

// a Repo whose log and progress go into the job (under the module lock)
#define BP_JOB_REPO(dirExpr)                                                                         \
    bp::Repo r = bp::Repo::load(dirExpr);                                                            \
    r.log = [this, &j](const std::string& m) { std::lock_guard<std::recursive_mutex> l(m_mtx); j.log.push_back(m); }; \
    r.progress = [this, &j](const std::string& w, int64_t d, int64_t t) { std::lock_guard<std::recursive_mutex> l(m_mtx); j.progressWhat = w; j.done = d; j.total = t; };

std::string BackpackCoreImpl::addApk(std::string slug, std::string apkPath) {
    if (apkPath.rfind("file://", 0) == 0) apkPath = apkPath.substr(7);
    std::string dir = repoDir(slug), signDir = m_dataDir + "/signing";
    return startJob("add", slug, [this, dir, signDir, apkPath](Job& j) {
        BP_JOB_REPO(dir)
        std::string path = apkPath; bp::TempDir t;
        {   // unsigned (no v2/v3 block): sign it with the package's key, if this computer has one
            bp::Zip z(apkPath);
            bool signedApk = z.cdOffset >= 16 && std::string((const char*)&z.data[z.cdOffset - 16], 16) == "APK Sig Block 42";
            if (!signedApk) {
                const bp::Zip::Entry* me = z.find("AndroidManifest.xml"); if (!me) throw bp::Err("not an APK (no AndroidManifest.xml)");
                std::string pkg = bp::parseManifest(z.read(*me, 8u << 20)).id;
                bp::Keys keys{signDir};
                if (!bp::validPackage(pkg) || !keys.has(pkg)) throw bp::Err("the APK is unsigned and there is no signing key for " + pkg + " here: create or import one (Keys), or add a signed APK");
                path = (t.p / "signed.apk").string();
                std::string cert = bp::signApk(apkPath, path, keys.load(pkg));
                r.log("signed with the " + pkg + " key (" + cert.substr(0, 8) + "...)");
            }
        }
        bool added = r.addApk(path);
        return json{{"added", added}}.dump();
    });
}

std::string BackpackCoreImpl::keys() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    try { return ok(json{{"keys", bp::Keys{m_dataDir + "/signing"}.list()}}); } catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::createKey(std::string package, std::string name) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!bp::validPackage(package)) return fail("Give the app's package name, like org.example.app");
    try { std::string cert; std::thread([&]() { cert = bp::Keys{m_dataDir + "/signing"}.create(package, name); }).join();   // RSA-4096 keygen takes a moment
          return ok(json{{"package", package}, {"cert", cert}, {"note", "Export a backup now: without this key no update of the app can ever be published"}}); }
    catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::importKey(std::string package, std::string p12Path, std::string password) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!bp::validPackage(package)) return fail("Give the app's package name, like org.example.app");
    if (p12Path.rfind("file://", 0) == 0) p12Path = p12Path.substr(7);
    try { return ok(json{{"package", package}, {"cert", bp::Keys{m_dataDir + "/signing"}.importP12(package, p12Path, password)}}); }
    catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::exportKey(std::string package, std::string p12Path, std::string password) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (p12Path.rfind("file://", 0) == 0) p12Path = p12Path.substr(7);
    try { bp::Keys{m_dataDir + "/signing"}.exportP12(package, p12Path, password); return ok(json{{"path", p12Path}}); }
    catch (std::exception& e) { return fail(e.what()); }
}

std::string BackpackCoreImpl::publish(std::string slug, std::string force) {
    std::string dir = repoDir(slug);
    return startJob("publish", slug, [this, dir, force](Job& j) {
        BP_JOB_REPO(dir)
        json res = r.publish(force == "1");
        if (res.value("unchanged", false)) r.log("Nothing changed since #" + std::to_string(res.value("seq", 0)) + ": not published");
        else r.log("Published #" + std::to_string(res.value("seq", 0)));
        return res.dump();
    });
}

std::string BackpackCoreImpl::verify(std::string slug, std::string deep) {
    std::string dir = repoDir(slug);
    return startJob("verify", slug, [this, dir, deep](Job& j) {
        BP_JOB_REPO(dir)
        json res = r.verify(deep == "1");
        if (!res.value("ok", false)) throw bp::Err("problems found: see the log");
        r.log("All good");
        return res.dump();
    });
}

std::string BackpackCoreImpl::importFdroid(std::string slug, std::string repoUrl, std::string fingerprint, std::string versions) {
    std::string dir = repoDir(slug);
    if (repoUrl.rfind("https://", 0) != 0 && repoUrl.rfind("http://", 0) != 0) return fail("The F-Droid repo URL starts with http(s)://");
    return startJob("import", slug, [this, dir, repoUrl, fingerprint, versions](Job& j) {
        BP_JOB_REPO(dir)
        bp::ImportOpts o; o.url = repoUrl; o.fingerprint = fingerprint; o.versions = std::max(1, std::min(5, std::atoi(versions.c_str())));
        json res = bp::importFdroid(r, o);
        r.log(std::to_string(res.value("added", 0)) + " version(s) imported; publish to make them visible");
        return res.dump();
    });
}

std::string BackpackCoreImpl::clearJobs() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    for (auto it = m_jobs.begin(); it != m_jobs.end();) { if (it->second->state != "running") it = m_jobs.erase(it); else ++it; }
    return ok();
}
