// backpack_core_impl.cpp - see backpack_core_impl.h and docs/adr/0011-basecamp-app.md.
#include "backpack_core_impl.h"
#include "bp_fdroid.hpp"
#include "bp_keys.hpp"
#include "bp_subs.hpp"
#include "qrcodegen.hpp"
#include "logos_sdk.h"   // generated: modules().loam_core / .delivery_module / .storage_module
#include <QObject>
#include <QTimer>
#include <chrono>
#include <condition_variable>
#include <cstdlib>

using bp::json;
namespace {
const char* VERSION = "0.5.1";
std::string ok(json j = json::object()) { j["ok"] = true; return j.dump(); }
std::string fail(const std::string& why) { return json{{"ok", false}, {"error", why}}.dump(); }
json qrMatrix(const std::string& text) {
    auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    int n = qr.getSize(); json cells = json::array();
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) cells.push_back(qr.getModule(x, y));
    return json{{"n", n}, {"cells", cells}};
}
}  // namespace

// a Repo whose log and progress go into the job (under the module lock)
#define BP_JOB_REPO(dirExpr)                                                                         \
    bp::Repo r = bp::Repo::load(dirExpr);                                                            \
    r.log = [this, &j](const std::string& m) { std::lock_guard<std::recursive_mutex> l(m_mtx); j.log.push_back(m); }; \
    r.progress = [this, &j](const std::string& w, int64_t d, int64_t t) { std::lock_guard<std::recursive_mutex> l(m_mtx); j.progressWhat = w; j.done = d; j.total = t; };

BackpackCoreImpl::~BackpackCoreImpl() = default;   // job threads are detached; the module lives as long as the process

void BackpackCoreImpl::onContextReady() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (const char* d = std::getenv("BACKPACK_DIR")) m_dataDir = d;
    else if (const char* h = std::getenv("HOME")) m_dataDir = std::string(h) + "/.backpack";
    else m_dataDir = ".backpack";
    loadPrefs();
    if (!m_loop) m_loop = new QObject();   // lives on this (the module's) event-loop thread
    startWatcher();
    // calls to other modules wait until the module has loaded (the runtime rejects them from here)
    QTimer::singleShot(8000, m_loop, [this] { announceDue(); });
    // mirroring (ADR 0015): first look after 20 s, then every BACKPACK_MIRROR_SEC (default 120)
    int mirrorEvery = 120;
    if (const char* e = std::getenv("BACKPACK_MIRROR_SEC")) mirrorEvery = std::max(15, std::atoi(e));
    QTimer::singleShot(20000, m_loop, [this] { mirrorTick(); });
    auto* mt = new QTimer(m_loop);
    QObject::connect(mt, &QTimer::timeout, m_loop, [this] { mirrorTick(); });
    mt->start(mirrorEvery * 1000);
}

// Paid repos publish themselves when someone joins or a device is taken off (not when a membership only
// runs out). Every BACKPACK_WATCH_SEC seconds (default 300): read the chain quietly; publish as a job
// only when there is a reason, so an idle repo leaves no trace. Runs as long as the module does: in
// Basecamp while it's open, or headless (logosctl) as an always-on publisher.
void BackpackCoreImpl::startWatcher() {
    int every = 300;
    if (const char* e = std::getenv("BACKPACK_WATCH_SEC")) every = std::max(30, std::atoi(e));
    std::thread([this, every]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(every));
            std::vector<std::string> slugs;
            {
                std::lock_guard<std::recursive_mutex> lk(m_mtx);
                std::error_code ec;
                if (bp::fs::is_directory(m_dataDir, ec))
                    for (auto& e : bp::fs::directory_iterator(m_dataDir, ec))
                        if (bp::fs::exists(e.path() / "repo.json") && bp::fs::exists(e.path() / "key.pem")) slugs.push_back(e.path().filename().string());
            }
            for (auto& slug : slugs) watchOnce(slug);
            announceDue();
        }
    }).detach();
}

void BackpackCoreImpl::watchOnce(const std::string& slug) {
    std::string dir;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (!validSlug(slug) || m_busyRepo.count(slug)) return;
        m_busyRepo[slug] = 0;   // held while reading, so no job edits the repo meanwhile
        dir = repoDir(slug);
    }
    std::string why;
    try {
        bp::Repo r = bp::Repo::load(dir);
        if (r.usesLez() && r.lezAuto()) { r.syncLez(); why = r.lezChangeNeedsPublish(); }
    } catch (...) {}   // chain or disk unreachable: try again next round
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_busyRepo.erase(slug);
    }
    if (why.empty()) return;
    startJob("auto-publish", slug, [this, dir, why](Job& j) {
        BP_JOB_REPO(dir)
        r.log("LEZ: " + why + ": publishing");
        json res = r.publish(false);
        r.log("Published #" + std::to_string(res.value("seq", 0)) + "; the update note goes out over Delivery");
        if (!res.value("unchanged", false)) announce(r.dir.filename().string());
        return res.dump();
    });
}

// ── Logos Delivery: update notes out (loam_core), followed repos' notes in (Store) ──────────────────
namespace {
const long long kResendSec = 12 * 3600;   // Store keeps a message for a limited time: send it again
const char* kPreset = "logos.test";        // the network Backpack phones are on (cluster 2)
const int kCluster = 2, kShards = 8;
const char* kEntry[] = {
    "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmQ9X2xDfPG3uL77V9piYDhjq14JhKCtcmNYsTMKNqrKCj",
    "/dns4/node-02.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmB8NYprrfQrgWVzsJtYWkfjsXbmJEGNMG6othXsQ53BwG",
    "/dns4/node-01.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmF8WtwGPmeGHgYAX2277jHgy5cW9F7zsB8EqUjBZQAZQ3",
    "/dns4/node-02.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmUuXhUW9bdJpzN1kfDziFiUZo4bszTk66cvr7uuyCHXR7",
    "/dns4/node-01.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmL3oU95jh1BZHozn3uNhx8HEneirgr8M1jEAapzXGDqRF",
    "/dns4/node-02.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAm28CoBZjpyxsanC8tQpbvZ7bZJnVYuB1EgFzb571qpWsV",
};
std::string noteTopic(const std::string& id) { return "/backpack/1/" + id + "/json"; }
// the shard a content topic lands on (autosharding: sha256(app + version), last 8 bytes, mod shards)
std::string pubsubTopicFor(const std::string& contentTopic) {
    std::vector<std::string> parts; size_t i = 1;
    while (i <= contentTopic.size()) { size_t j = contentTopic.find('/', i); parts.push_back(contentTopic.substr(i, j == std::string::npos ? std::string::npos : j - i)); if (j == std::string::npos) break; i = j + 1; }
    std::string a = parts.size() > 1 ? parts[0] + parts[1] : contentTopic;
    bp::Bytes d = bp::sha256((const unsigned char*)a.data(), a.size());
    uint64_t v = 0; for (int k = 24; k < 32; k++) v = v << 8 | d[k];
    return "/waku/2/rs/" + std::to_string(kCluster) + "/" + std::to_string(v % kShards);
}
long long nowSec() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
// A note as it comes back: the signed JSON, possibly base64-wrapped once or twice by the senders' layers.
void noteCandidates(const std::string& bytes, std::vector<std::string>& out) {
    std::string cur = bytes;
    for (int depth = 0; depth < 3 && cur.size() <= 16384; depth++) {
        size_t f = cur.find_first_not_of(" \t\r\n");
        if (f != std::string::npos && cur[f] == '{') { out.push_back(cur); return; }
        try { bp::Bytes b = bp::unb64url(cur); cur.assign(b.begin(), b.end()); } catch (...) { return; }
    }
}
}  // namespace

void BackpackCoreImpl::onLoop(std::function<void()> fn) {
    if (m_loop) QTimer::singleShot(0, m_loop, std::move(fn));
}

// Every published repo whose note hasn't gone out in the last 12 hours (all of them after a start).
void BackpackCoreImpl::announceDue() {
    std::vector<std::string> due;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        std::error_code ec; long long now = nowSec();
        if (bp::fs::is_directory(m_dataDir, ec))
            for (auto& e : bp::fs::directory_iterator(m_dataDir, ec)) {
                std::string slug = e.path().filename().string();
                if (!bp::fs::exists(e.path() / "latest.json") || !bp::fs::exists(e.path() / "key.pem")) continue;
                if (now - m_netSentAt[slug] >= kResendSec) due.push_back(slug);
            }
    }
    for (auto& s : due) announce(s);
}

// Queue the repo's current note (latest.json, already signed) for Delivery. Any thread.
void BackpackCoreImpl::announce(const std::string& slug) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    try {
        bp::Repo r = bp::Repo::load(repoDir(slug));
        std::string text = bp::readFile((bp::fs::path(repoDir(slug)) / "latest.json").string());
        if (text.empty() || text.size() > 8192) return;
        std::string topic = noteTopic(r.pointerId());
        for (auto it = m_netQueue.begin(); it != m_netQueue.end(); ) it = it->first == topic ? m_netQueue.erase(it) : it + 1;
        m_netQueue.emplace_back(topic, text);
        m_netSentAt[slug] = nowSec();
    } catch (...) { return; }
    onLoop([this] { netFlush(); });
}

// Bring up the shared node once (loop thread). Another app may have started it already: then
// start() only re-announces its status.
void BackpackCoreImpl::netStart() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (m_netStarted) return;
        m_netStarted = true; m_netStatus = "starting";
    }
    json cfg{{"logLevel", "INFO"}, {"mode", "Core"}, {"preset", kPreset}, {"relay", true}, {"entryNodes", json::array()}};
    for (auto e : kEntry) cfg["entryNodes"].push_back(e);
    // phones read notes from plain relay (and Store), not SDS channels
    modules().loam_core.setTopicModeAsync("/backpack/", "relay", [](std::string) {});
    modules().loam_core.onStatusChanged([this](const std::string& st) {
        onLoop([this, st] {
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_netStatus = st; }
            if (st == "Connected") netFlush();
        });
    });
    modules().loam_core.startAsync(cfg.dump(), [this](std::string err) {
        if (err.empty()) return;
        onLoop([this, err] { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_netStatus = "error: " + err; m_netStarted = false; });
    });
}

// Send what's queued once the node is up (loop thread).
void BackpackCoreImpl::netFlush() {
    std::deque<std::pair<std::string, std::string>> q;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (!m_netStarted) { onLoop([this] { netStart(); }); return; }
        if (m_netStatus != "Connected" || m_netQueue.empty()) return;
        q.swap(m_netQueue);
    }
    for (auto& [topic, text] : q) {
        std::string b64 = bp::b64std(bp::Bytes(text.begin(), text.end()));
        auto send = [this, topic, b64] {
            modules().loam_core.sendSealedAsync(topic, b64, [this, topic](std::string err) {
                std::lock_guard<std::recursive_mutex> lk(m_mtx);
                m_netSent[topic] = err.empty() ? bp::nowIso() : "failed: " + err;
            });
        };
        bool joined; { std::lock_guard<std::recursive_mutex> lk(m_mtx); joined = m_netJoined.count(topic) > 0; }
        if (joined) send();
        else modules().loam_core.joinAsync(topic, [this, topic, send](std::string) {
            onLoop([this, topic, send] { { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_netJoined.insert(topic); } send(); });
        });
    }
}

// The notes Store holds for a topic, from every store node we ask (worker threads only: waits).
std::vector<std::string> BackpackCoreImpl::storeNotes(const std::string& topic) {
    for (int i = 0; i < 60; i++) {   // the node must be up to query Store
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (m_netStatus == "Connected") break; if (!m_netStarted) onLoop([this] { netStart(); }); }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (m_netStatus != "Connected") throw bp::Err("the Delivery node isn't up (" + m_netStatus + ")"); }
    struct St { std::mutex m; std::condition_variable cv; int left = 0, ok = 0; std::vector<std::string> notes; std::string err; };
    auto st = std::make_shared<St>();
    // ask every fleet node: some don't answer (2026-10-28: the node-01s refused the dial), any one is enough
    st->left = (int)(sizeof(kEntry) / sizeof(kEntry[0]));
    static std::atomic<int> counter{0};
    for (auto peer : kEntry) {
        json req{{"requestId", "backpack-" + std::to_string(++counter)}, {"includeData", true}, {"paginationForward", false}, {"paginationLimit", 20},
                 {"pubsubTopic", pubsubTopicFor(topic)}, {"contentTopics", json::array({topic})},
                 {"timeStart", (long long)(nowSec() - 86400 + 120) * 1000000000LL}};   // store nodes refuse a range over 24 h
        std::string reqS = req.dump(), peerS = peer;
        onLoop([this, st, reqS, peerS] {
            modules().delivery_module.storeQueryAsync(reqS, peerS, 15000, [st](StdLogosResult r) {
                std::vector<std::string> got; std::string err;
                try {
                    if (!r.success) throw bp::Err(r.error.empty() ? "no answer" : r.error);
                    // the response JSON, as text or already parsed
                    json resp = r.value.is_string() ? json::parse(r.value.get<std::string>()) : json::parse(r.value.dump());
                    for (auto& m : resp.value("messages", json::array())) {
                        if (!m.is_object() || !m.contains("message") || !m["message"].is_object()) continue;
                        auto p = m["message"].value("payload", std::string());
                        bp::Bytes b = bp::unb64url(p); noteCandidates(std::string(b.begin(), b.end()), got);
                    }
                } catch (std::exception& e) { err = e.what(); }
                std::lock_guard<std::mutex> l(st->m);
                for (auto& g : got) st->notes.push_back(g);
                if (!err.empty()) st->err = err; else st->ok++;
                if (--st->left == 0) st->cv.notify_all();
            });
        });
    }
    std::unique_lock<std::mutex> l(st->m);
    st->cv.wait_for(l, std::chrono::seconds(25), [&] { return st->left == 0; });
    if (st->notes.empty() && !st->err.empty() && st->ok == 0) throw bp::Err("Store: " + st->err);
    return st->notes;
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
            try { json r = bp::Repo::load(repoDir(s)).summary(); r["busy"] = m_busyRepo.count(s) > 0;
                  auto sent = m_netSent.find(noteTopic(r.value("pointerId", std::string()))); r["noteSent"] = sent == m_netSent.end() ? json() : json(sent->second); r.erase("apps"); r["appCount"] = bp::Repo::load(repoDir(s)).s["apps"].size(); repos.push_back(r); }
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
    json subs = json::array(), me = json::object();
    try {
        bp::Subs x = bp::Subs::load(m_dataDir); me = x.s["me"];
        for (auto& [k, v] : x.s["subs"].items()) { json o = v; o["busy"] = m_busyRepo.count(subSlug(k)) > 0; subs.push_back(o); }
    } catch (...) {}
    return ok(json{{"version", VERSION}, {"dataDir", m_dataDir}, {"lastNode", m_lastNode}, {"repos", repos}, {"jobs", jobs}, {"subs", subs}, {"me", me}, {"delivery", m_netStatus}, {"mirror", json::parse(m_mirrorStatus, nullptr, false)}});
}

std::string BackpackCoreImpl::createRepo(std::string slug, std::string name, std::string privateFlag, std::string node) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug)) return fail("A short id: letters, digits, - and _ (it names the folder)");
    if (name.empty() || name.size() > 100) return fail("Give the repo a name (at most 100 characters)");
    if (!node.empty() && node.rfind("http://", 0) != 0 && node.rfind("https://", 0) != 0) return fail("The Storage node is its REST address: http(s)://...");
    try {
        bp::Repo r = bp::Repo::create(repoDir(slug), name, privateFlag == "1", node, privateFlag == "2");
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
        return ok(json{{"link", l}, {"private", !r.readKey().empty() && !r.isMembers()}, {"members", r.isMembers()}, {"qr", qrMatrix(l)}});
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

std::string BackpackCoreImpl::addMember(std::string slug, std::string memberLink) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try { bp::Repo r = bp::Repo::load(repoDir(slug)); r.addMember(memberLink); return ok(json{{"repo", r.summary()}, {"note", "Publish to give it access"}}); }
    catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::removeMember(std::string slug, std::string devicePk) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try { bp::Repo r = bp::Repo::load(repoDir(slug)); r.removeMember(devicePk); return ok(json{{"repo", r.summary()}, {"note", "Publish to take its access away for new versions"}}); }
    catch (std::exception& e) { return fail(e.what()); }
}

// LEZ memberships (ADR 0010): {"wallet","sequencer","program","lib"}; {} turns it off
std::string BackpackCoreImpl::setLez(std::string slug, std::string configJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!validSlug(slug) || m_busyRepo.count(slug)) return fail("The repo is busy");
    try {
        json c = json::parse(configJson, nullptr, false); if (c.is_discarded() || !c.is_object()) return fail("The LEZ settings are a JSON object");
        bp::Repo r = bp::Repo::load(repoDir(slug)); r.setLez(c);
        if (c.empty()) return ok(json{{"repo", r.summary()}, {"note", "LEZ memberships off"}});
    } catch (std::exception& e) { return fail(e.what()); }
    // then on the chain: create this repo's config (or update its price/period) and read its members
    std::string dir = repoDir(slug);
    return startJob("lez", slug, [this, dir](Job& j) {
        BP_JOB_REPO(dir)
        r.lezEnsure();
        return r.syncLez().dump();
    });
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
        else { r.log("Published #" + std::to_string(res.value("seq", 0)) + "; the update note goes out over Delivery"); announce(r.dir.filename().string()); }
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

std::string BackpackCoreImpl::syncLez(std::string slug) {
    std::string dir = repoDir(slug);
    return startJob("lez", slug, [this, dir](Job& j) {
        BP_JOB_REPO(dir)
        return r.syncLez().dump();
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

// ---- following repos and paying for memberships (bp_subs.hpp) ----
// A followed repo's jobs run under the slug "sub_<key>" (a b64url key is slug-safe), one at a time per repo.
std::string BackpackCoreImpl::subSlug(const std::string& key) const { return "sub_" + key; }

#define BP_JOB_SUBS                                                                                  \
    bp::Subs x = bp::Subs::load(dataDir);                                                            \
    x.log = [this, &j](const std::string& m) { std::lock_guard<std::recursive_mutex> l(m_mtx); j.log.push_back(m); }; \
    x.fromDelivery = [this](const std::string& key) { return storeNotes(noteTopic(key)); };

std::string BackpackCoreImpl::follow(std::string link) {
    std::string key;
    try { key = bp::parseRepoLink(link).key; } catch (std::exception& e) { return fail(e.what()); }
    std::string dataDir = m_dataDir;
    return startJob("follow", subSlug(key), [this, dataDir, link](Job& j) {
        BP_JOB_SUBS
        json r = x.follow(link);
        x.log("following " + r.value("name", std::string("the repo")) + (bp::Subs::paid(r) ? ": a paid repo" : ""));
        return json{{"key", r["key"]}}.dump();
    });
}
std::string BackpackCoreImpl::unfollow(std::string key) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (m_busyRepo.count(subSlug(key))) return fail("busy");
    try { bp::Subs x = bp::Subs::load(m_dataDir); x.unfollow(key); return ok(); } catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::refreshSub(std::string key) {
    std::string dataDir = m_dataDir;
    return startJob("check", subSlug(key), [this, dataDir, key](Job& j) {
        BP_JOB_SUBS
        json r = x.refresh(key);
        if (r.contains("membershipError")) x.log("membership: " + r["membershipError"].get<std::string>());
        x.log("update note #" + std::to_string(r.value("seq", 0)));
        return std::string("{}");
    });
}
std::string BackpackCoreImpl::setLezAccount(std::string meJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    try { bp::Subs x = bp::Subs::load(m_dataDir); x.setMe(json::parse(meJson)); return ok(); } catch (std::exception& e) { return fail(e.what()); }
}
std::string BackpackCoreImpl::payMembership(std::string key, std::string periods, std::string phoneLink, std::string expectPrice) {
    uint64_t n = 0;
    try { n = std::stoull(periods); } catch (...) { return fail("How many periods?"); }
    if (n < 1 || n > 1200) return fail("1 to 1200 periods");
    std::string dataDir = m_dataDir;
    return startJob("pay", subSlug(key), [this, dataDir, key, n, phoneLink, expectPrice](Job& j) {
        BP_JOB_SUBS
        json r = x.pay(key, n, phoneLink, expectPrice);
        x.afterChange(key);
        x.log("Done: the publisher's next update lets your devices in");
        return r.dump();
    });
}
std::string BackpackCoreImpl::addMyDevice(std::string key, std::string phoneLink) {
    std::string dataDir = m_dataDir;
    return startJob("device", subSlug(key), [this, dataDir, key, phoneLink](Job& j) {
        BP_JOB_SUBS
        json r = x.addDevice(key, phoneLink);
        x.afterChange(key);
        return r.dump();
    });
}
std::string BackpackCoreImpl::removeMyDevice(std::string key, std::string devicePk) {
    std::string dataDir = m_dataDir;
    return startJob("device", subSlug(key), [this, dataDir, key, devicePk](Job& j) {
        BP_JOB_SUBS
        json r = x.removeDevice(key, devicePk);
        x.afterChange(key);
        return r.dump();
    });
}

// ── Mirroring (ADR 0015): followers hold the files of the repos they follow and serve them ─────────────
namespace {
long long mirrorNowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
bool resTrue(const StdLogosResult& r) {
    return r.success && ((r.value.is_boolean() && r.value.get<bool>()) || (r.value.is_string() && r.value.get<std::string>() == "true"));
}
json resJson(const StdLogosResult& r) {
    if (r.value.is_string()) { json j = json::parse(r.value.get<std::string>(), nullptr, false); return j.is_discarded() ? json() : j; }
    return r.value;
}
bool isCid(const std::string& c) {
    if (c.size() < 40 || c.size() > 120) return false;
    for (char ch : c) if (!isalnum((unsigned char)ch)) return false;
    return true;
}
}  // namespace

std::string BackpackCoreImpl::setMirror(std::string key, std::string on) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    try {
        bp::Subs x = bp::Subs::load(m_dataDir);
        json& s = x.sub(key);
        if (on == "0") s["mirrorOff"] = true; else s.erase("mirrorOff");
        x.save();
    } catch (std::exception& e) { return fail(e.what()); }
    onLoop([this] { mirrorTick(); });
    return ok();
}

// A mirror list arrived (downloadToUrl of the list CID): remember its CIDs. Other downloads are not ours.
void BackpackCoreImpl::mirrorListDone(const std::string& payload) {
    json p = json::parse(payload, nullptr, false);
    if (!p.is_object()) return;
    std::string cid = p.value("sessionId", std::string());
    if (!m_mirrorListing.count(cid)) return;   // someone else's download
    m_mirrorListing.erase(cid);
    std::string dir;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); dir = m_dataDir; }
    std::string tmp = dir + "/mirror-tmp/" + cid + ".json";
    std::error_code ec;
    if (p.value("success", false)) {
        try {
            std::string raw = bp::readFile(tmp);
            if (raw.size() <= (1u << 20)) {
                json l = json::parse(raw);
                std::vector<std::string> cids;
                for (auto& c : l.value("cids", json::array())) if (c.is_string() && isCid(c) && cids.size() < 5000) cids.push_back(c);
                m_mirrorNewLists[cid] = cids;   // merged into the state by the next tick
            }
        } catch (std::exception& e) { fprintf(stderr, "Backpack mirror: list %s unreadable: %s\n", cid.c_str(), e.what()); }
    } else fprintf(stderr, "Backpack mirror: list %s failed: %s\n", cid.c_str(), p.value("error", std::string()).c_str());
    bp::fs::remove(tmp, ec);
    onLoop([this] { mirrorTick(); });
}

// Hold what the followed repos' latest pointers name; drop what nothing names any more. fetch() has no
// completion event, so holding is observed with exists(). Runs on the module's event loop.
void BackpackCoreImpl::mirrorTick() {
    // never inside another tick: its sync calls to storage_module run nested events, which may start one
    if (m_mirrorInTick) { m_mirrorAgain = true; return; }
    m_mirrorInTick = true;
    try { mirrorTickBody(); }
    catch (std::exception& e) { fprintf(stderr, "Backpack mirror: tick failed: %s\n", e.what()); }
    catch (...) { fprintf(stderr, "Backpack mirror: tick failed\n"); }
    m_mirrorInTick = false;
    if (m_mirrorAgain) { m_mirrorAgain = false; onLoop([this] { mirrorTick(); }); }
}

void BackpackCoreImpl::mirrorTickBody() {
    std::string dir; json subs;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        dir = m_dataDir;
        try { subs = bp::Subs::load(dir).s["subs"]; } catch (...) { return; }
    }
    if (!m_mirrorEvents) {
        try { modules().storage_module.onStorageDownloadDone([this](const std::string& payload) { onLoop([this, payload] { mirrorListDone(payload); }); }); m_mirrorEvents = true; }
        catch (...) { return; }   // storage not up yet: next tick
    }
    json st; try { st = json::parse(bp::readFile(dir + "/mirror.json")); } catch (...) { st = json::object(); }
    if (!st.is_object()) st = json::object();
    if (!st.contains("held") || !st["held"].is_object()) st["held"] = json::object();
    if (!st.contains("lists") || !st["lists"].is_object()) st["lists"] = json::object();
    for (auto& [L, cids] : m_mirrorNewLists) st["lists"][L] = cids;
    m_mirrorNewLists.clear();
    long long now = mirrorNowMs();

    // what each followed repo asks us to hold
    std::map<std::string, std::set<std::string>> want;   // cid -> repo keys
    std::set<std::string> listsInUse;
    json perRepo = json::object();
    for (auto& [key, x] : subs.items()) {
        if (x.value("mirrorOff", false) || !x.contains("hold")) continue;
        const json& h = x["hold"];
        // dial the repo's own nodes first (every 10 minutes): fetching across nodes needs a direct connection
        for (auto& n : h.value("nodes", json::array())) {
            std::string pid = n.value("peerId", std::string());
            if (pid.empty() || (m_mirrorDialed.count(pid) && now - m_mirrorDialed[pid] < 10 * 60000)) continue;
            m_mirrorDialed[pid] = now;
            std::vector<std::string> addrs; for (auto& a : n.value("addrs", json::array())) if (a.is_string()) addrs.push_back(a);
            try { modules().storage_module.connectAsyncResult(pid, addrs, [](logos::AsyncResult<StdLogosResult>) {}, 60000); } catch (...) {}
        }
        for (const char* f : {"index", "keys", "mirror"}) if (h.contains(f) && h[f].is_string() && isCid(h[f])) want[h[f]].insert(key);
        std::string L = h.value("mirror", std::string());
        if (L.empty() || !isCid(L)) continue;
        listsInUse.insert(L);
        if (st["lists"].contains(L)) { for (auto& c : st["lists"][L]) want[c.get<std::string>()].insert(key); continue; }
        if (m_mirrorListing.count(L) && now - m_mirrorListing[L] < 10 * 60000) continue;
        std::error_code ec; bp::fs::create_directories(dir + "/mirror-tmp", ec);
        m_mirrorListing[L] = now;
        if (!st["held"].contains(L)) st["held"][L]["pending"] = true;   // we download it: ours, so a cleanup removes it
        try { modules().storage_module.downloadToUrlAsyncResult(L, dir + "/mirror-tmp/" + L + ".json", false, 65536, false, true, [](logos::AsyncResult<StdLogosResult>) {}, 60000); }
        catch (...) { m_mirrorListing.erase(L); }
    }

    // fetch what's missing (advertise=true: we announce and serve it), note what has arrived
    for (auto& [cid, keys] : want) {
        bool have = false;
        try { have = resTrue(modules().storage_module.exists(cid)); } catch (...) {}
        if (have) {
            // already in the node before we wanted it (another app's data): hold it, never remove it
            if (!st["held"].contains(cid)) st["held"][cid]["preexisting"] = true;
            json& h = st["held"][cid];
            h.erase("pending");
            if (!h.contains("since")) h["since"] = bp::nowIso();
            h["repos"] = json(std::vector<std::string>(keys.begin(), keys.end()));
            m_mirrorFetching.erase(cid);
            continue;
        }
        auto f = m_mirrorFetching.find(cid);
        if (f != m_mirrorFetching.end() && now - f->second < 10 * 60000) continue;
        m_mirrorFetching[cid] = now;
        st["held"][cid]["pending"] = true;   // ours from now on, so a later cleanup may remove it
        try { modules().storage_module.fetchAsyncResult(cid, false, true, [](logos::AsyncResult<StdLogosResult>) {}, 60000); } catch (...) {}
    }

    // drop what no followed repo names any more: only CIDs this mirror fetched
    std::vector<std::string> drop;
    for (auto& [cid, h] : st["held"].items()) if (!want.count(cid)) drop.push_back(cid);
    for (auto& cid : drop) {
        if (!st["held"][cid].value("preexisting", false)) { try { modules().storage_module.remove(cid); } catch (...) {} }
        st["held"].erase(cid); m_mirrorFetching.erase(cid);
    }
    std::vector<std::string> oldLists;
    for (auto& [L, v] : st["lists"].items()) if (!listsInUse.count(L)) oldLists.push_back(L);
    for (auto& L : oldLists) st["lists"].erase(L);

    // status for the view: per repo files held / named and bytes; the node's announced addresses
    std::map<std::string, long long> sizes;
    try {
        json ms = resJson(modules().storage_module.manifests());
        if (ms.is_array()) for (auto& m : ms) if (m.is_object()) sizes[m.value("cid", std::string())] = m.value("datasetSize", 0LL);
    } catch (...) {}
    for (auto& [cid, keys] : want) {
        bool held = st["held"].contains(cid) && !st["held"][cid].value("pending", false);
        for (auto& k : keys) {
            json& r = perRepo[k];
            if (!r.is_object()) r = json::object();
            r["files"] = r.value("files", 0) + 1;
            if (held) { r["held"] = r.value("held", 0) + 1; r["bytes"] = r.value("bytes", 0LL) + (sizes.count(cid) ? sizes[cid] : 0LL); }
        }
    }
    json addrs = json::array();
    try { json d = resJson(modules().storage_module.debug()); if (d.is_object() && d.contains("addrs")) addrs = d["addrs"]; } catch (...) {}
    for (auto& [L, cids] : m_mirrorNewLists) st["lists"][L] = cids;   // arrived during this tick
    m_mirrorNewLists.clear();
    try { bp::writeFileAtomic(dir + "/mirror.json", st.dump(1), 0600); } catch (...) {}
    fprintf(stderr, "Backpack mirror: %zu wanted, %zu held, %zu fetching, %zu lists, dropped %zu, %zu addrs\n",
            want.size(), (size_t)st["held"].size(), m_mirrorFetching.size(), (size_t)st["lists"].size(), drop.size(), (size_t)addrs.size());
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_mirrorStatus = json{{"repos", perRepo}, {"addrs", addrs}, {"checkedAt", bp::nowIso()}}.dump();
}
