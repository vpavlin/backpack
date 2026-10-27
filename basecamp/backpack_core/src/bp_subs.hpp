#pragma once
// bp_subs.hpp - the subscriber side: repos this computer follows (not publishes), and paying for a
// membership of a paid members repo (ADR 0013 + backpack-access on LEZ). The membership's devices are
// the user's phones: their member codes go into it, and the publisher's next publish lets them in.
// Nothing here is a secret: the link of a members repo carries no key, and memberships are public on chain.
// State: <data dir>/subscriptions.json  {me:{wallet, account}, subs:{<repo key>:{...}}}
#include "bp_http.hpp"
#include "bp_lez.hpp"
#include "bp_members.hpp"

namespace bp {
// Membership programs this build trusts to take payments. A pointer naming any other program is refused:
// the wallet signs whatever the program asks, so the publisher must not be able to pick it.
inline bool knownLezProgram(const std::string& hex) {
    static const char* known[] = {
        "cb23358fd7150fdd38504b132eebe18abe2c4183c7a31133fdc94e43b3aa254c",   // backpack_access v3 (test chain)
    };
    for (auto k : known) if (hex == k) return true;
    return false;
}

struct RepoLink { std::string key, name, index; std::vector<std::string> pointers; bool members = false; };
inline RepoLink parseRepoLink(const std::string& link) {
    std::string t = link; while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back(); while (!t.empty() && isspace((unsigned char)t.front())) t.erase(0, 1);
    if (t.rfind("backpack://repo?", 0) != 0) throw Err("not a Backpack repo link (backpack://repo?...)");
    if (t.size() > 8192) throw Err("the link is too long");
    auto dec = [](const std::string& v) { std::string o; for (size_t i = 0; i < v.size(); i++) { if (v[i] == '%' && i + 2 < v.size()) { o += (char)std::stoi(v.substr(i + 1, 2), nullptr, 16); i += 2; } else if (v[i] == '+') o += ' '; else o += v[i]; } return o; };
    RepoLink r; std::string q = t.substr(16); size_t p = 0;
    while (p <= q.size()) {
        size_t e = q.find('&', p); std::string kv = q.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = dec(kv.substr(eq + 1));
            if (k == "k") r.key = v; else if (k == "n") r.name = v.substr(0, 100); else if (k == "i") r.index = v;
            else if (k == "m") r.members = v == "1";
            else if (k == "u" && v.rfind("https://", 0) == 0 && r.pointers.size() < 8) r.pointers.push_back(v);
        }
        if (e == std::string::npos) break; p = e + 1;
    }
    if (r.key.size() != 43 || unb64url(r.key).size() != 32) throw Err("the link's publisher key is malformed");
    return r;
}

struct Subs {
    std::string file;
    json s;
    std::function<void(const std::string&)> log = [](const std::string&) {};
    static Subs load(const std::string& dataDir) {
        Subs x; x.file = dataDir + "/subscriptions.json";
        try { x.s = json::parse(readFile(x.file)); } catch (...) { x.s = json::object(); }
        if (!x.s.is_object()) x.s = json::object();
        if (!x.s.contains("subs") || !x.s["subs"].is_object()) x.s["subs"] = json::object();
        if (!x.s.contains("me") || !x.s["me"].is_object()) x.s["me"] = json::object();
        return x;
    }
    void save() { std::error_code ec; fs::create_directories(fs::path(file).parent_path(), ec); writeFileAtomic(file, s.dump(2), 0600); }
    json& sub(const std::string& key) { if (!s["subs"].contains(key)) throw Err("you don't follow that repo"); return s["subs"][key]; }

    void setMe(const json& me) {
        std::string w = me.value("wallet", ""), a = me.value("account", "");
        if (w.empty() || !fs::is_directory(w)) throw Err("the LEZ wallet folder doesn't exist");
        accountToHex(a);   // throws if malformed
        s["me"] = json{{"wallet", w}, {"account", a}}; save();
    }

    // Follow a repo from its link: read its newest signed pointer to learn its name and, for a paid repo, its terms.
    json follow(const std::string& link) {
        RepoLink r = parseRepoLink(link);
        json& x = s["subs"][r.key];
        x["key"] = r.key; x["link"] = link; x["members"] = r.members; x["pointers"] = r.pointers;
        if (!x.contains("name") || !r.name.empty()) x["name"] = r.name.empty() ? "a Backpack repo" : r.name;
        x["followed"] = x.value("followed", nowIso());
        save();
        return refresh(r.key);
    }
    void unfollow(const std::string& key) { s["subs"].erase(key); save(); }

    json refresh(const std::string& key) {
        json& x = sub(key);
        json best; int64_t bestSeq = -1; std::string lastErr;
        for (auto& u : x.value("pointers", json::array())) {
            try {
                auto r = Http::get(u.get<std::string>(), 8192, 30000);
                if (!r.ok()) throw Err(r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
                json b = open(json::parse(r.body), key, "pointer");
                if (b.value("seq", 0) > bestSeq) { bestSeq = b.value("seq", 0); best = b; }
            } catch (std::exception& e) { lastErr = e.what(); }
        }
        if (bestSeq < 0) {
            x["error"] = x.value("pointers", json::array()).empty() ? "the link names no web address for the repo's update note" : "couldn't read the update note: " + lastErr;
            save(); throw Err(x["error"].get<std::string>());
        }
        x.erase("error"); x["seq"] = bestSeq; x["checkedAt"] = nowIso();
        if (best.contains("access") && best["access"].is_object()) x["access"] = best["access"]; else x.erase("access");
        if (paid(x)) readMembership(x);
        save();
        return x;
    }

    static bool paid(const json& x) { return x.contains("access") && x["access"].value("via", "") == "lez"; }
    json lezCfg(const json& x) const {
        if (!paid(x)) throw Err("this repo isn't a paid one");
        const json& a = x["access"];
        std::string prog = a.value("program", ""), seq = a.value("sequencer", "");
        if (!knownLezProgram(prog)) throw Err("this repo asks for payment through a membership program this Backpack doesn't know (" + prog.substr(0, 12) + "...): refusing");
        if (seq.rfind("http://", 0) != 0 && seq.rfind("https://", 0) != 0) throw Err("the repo's sequencer address is malformed");
        if (!s["me"].contains("wallet")) throw Err("set your LEZ wallet and account first");
        return json{{"wallet", s["me"]["wallet"]}, {"sequencer", seq}, {"program", prog}};
    }
    // My membership of this repo, as the chain has it, and the current price.
    void readMembership(json& x) {
        if (!s["me"].contains("account")) return;
        try {
            json cfg = lezCfg(x);
            json m = lezMemberOp(cfg, "get", json{{"account", s["me"]["account"]}});
            x["membership"] = json{{"found", m.value("found", false)}, {"paidThroughMs", m.contains("paid_through_ms") ? (m["paid_through_ms"].is_string() ? std::atoll(m["paid_through_ms"].get<std::string>().c_str()) : m["paid_through_ms"].get<int64_t>()) : 0},
                                   {"devices", m.value("devices", json::array())}, {"readAt", nowIso()}};
            json c = lezConfigOf(cfg);
            x["terms"] = json{{"price", c.contains("price_per_epoch") ? (c["price_per_epoch"].is_string() ? c["price_per_epoch"].get<std::string>() : c["price_per_epoch"].dump()) : ""}, {"periodMs", c.value("epoch_len_ms", (int64_t)0)}};
        } catch (std::exception& e) { x["membershipError"] = e.what(); return; }
        x.erase("membershipError");
    }

    // Pay for a membership (or more of it), add or take off a phone. expectPrice: the price the user saw;
    // refused if the chain's price differs (the publisher changed it meanwhile).
    json pay(const std::string& key, uint64_t periods, const std::string& phoneLink, const std::string& expectPrice) {
        json& x = sub(key); json cfg = lezCfg(x);
        if (!s["me"].contains("account")) throw Err("set your LEZ account first");
        json c = lezConfigOf(cfg);
        std::string price = c.contains("price_per_epoch") ? (c["price_per_epoch"].is_string() ? c["price_per_epoch"].get<std::string>() : c["price_per_epoch"].dump()) : "";
        if (!expectPrice.empty() && price != expectPrice) throw Err("the price changed to " + price + " per period: look again before paying");
        readMembership(x);
        bool member = x.contains("membership") && x["membership"].value("found", false);
        json r;
        if (member) { log("renewing for " + std::to_string(periods) + " period(s)"); r = lezMemberOp(cfg, "renew", json{{"account", s["me"]["account"]}, {"epochs", periods}}); }
        else {
            MemberLink m = parseMemberLink(phoneLink);
            log("joining for " + std::to_string(periods) + " period(s), first device " + m.name);
            r = lezMemberOp(cfg, "register", json{{"account", s["me"]["account"]}, {"devicePk", m.pk}, {"epochs", periods}});
            nameDevice(x, m);
        }
        log("paid: transaction " + r.value("tx_hash", std::string("?")).substr(0, 16) + "...");
        save(); return r;
    }
    json addDevice(const std::string& key, const std::string& phoneLink) {
        json& x = sub(key); json cfg = lezCfg(x); MemberLink m = parseMemberLink(phoneLink);
        log("adding " + m.name + " to your membership");
        json r = lezMemberOp(cfg, "addDevice", json{{"account", s["me"]["account"]}, {"devicePk", m.pk}});
        nameDevice(x, m); save(); return r;
    }
    json removeDevice(const std::string& key, const std::string& pk) {
        json& x = sub(key); json cfg = lezCfg(x);
        log("taking a device off your membership");
        json r = lezMemberOp(cfg, "removeDevice", json{{"account", s["me"]["account"]}, {"devicePk", pk}});
        save(); return r;
    }
    // device names are local (the chain has keys only)
    void nameDevice(json& x, const MemberLink& m) { if (!x.contains("deviceNames") || !x["deviceNames"].is_object()) x["deviceNames"] = json::object(); x["deviceNames"][m.pk] = m.name.empty() ? "a phone" : m.name; }
};
}  // namespace bp
