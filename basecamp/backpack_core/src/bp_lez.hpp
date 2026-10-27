#pragma once
// bp_lez.hpp - LEZ memberships (backpack-access, ADR 0010) as a source of member devices (ADR 0013).
// The backpack_access_ffi library is loaded at run time (dlopen), so backpack_core works without it;
// only a repo set to use LEZ needs it. Read-only here: list the registry, read each membership.
#include "bp_crypto.hpp"
#include <dlfcn.h>
#include <cstdlib>

namespace bp {
struct LezFfi {
    void* h = nullptr;
    void* (*open)(const char*) = nullptr;
    void (*close)(void*) = nullptr;
    char* (*list)(const void*, const char*) = nullptr;
    char* (*getMany)(const void*, const char*) = nullptr;
    char* (*getConfig)(const void*) = nullptr;
    void (*freeStr)(char*) = nullptr;
    // lib: an explicit path, else $BACKPACK_LEZ_LIB, else the loader's search path
    static LezFfi& get(const std::string& lib) {
        static LezFfi f; static std::string loaded;
        std::string p = !lib.empty() ? lib : std::getenv("BACKPACK_LEZ_LIB") ? std::getenv("BACKPACK_LEZ_LIB") : "libbackpack_access_ffi.so";
        if (f.h && loaded == p) return f;
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { const char* e = dlerror(); throw Err(std::string("can't load the LEZ membership library (") + p + "): " + (e ? e : "?")); }
        LezFfi n; n.h = h;
        n.open = (void* (*)(const char*))dlsym(h, "backpack_access_config_open");
        n.close = (void (*)(void*))dlsym(h, "backpack_access_config_close");
        n.list = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_list");
        n.getMany = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_get_many");
        n.getConfig = (char* (*)(const void*))dlsym(h, "backpack_access_get_config");
        n.freeStr = (void (*)(char*))dlsym(h, "backpack_access_free_string");
        if (!n.open || !n.close || !n.list || !n.getMany || !n.getConfig || !n.freeStr) { dlclose(h); throw Err("the LEZ membership library lacks the expected functions"); }
        if (f.h) dlclose(f.h);
        f = n; loaded = p; return f;
    }
    json take(char* s) const {
        if (!s) throw Err("the LEZ library returned nothing");
        std::string t(s); freeStr(s);
        json j = json::parse(t, nullptr, false);
        if (j.is_discarded() || !j.is_object()) throw Err("the LEZ library returned something unreadable");
        if (j.contains("ok") && j["ok"] == false) throw Err("LEZ: " + j.value("error", std::string("failed")));
        if (j.contains("success") && j["success"] == false) throw Err("LEZ: " + j.value("error", std::string("failed")));
        return j;
    }
};

// A device key as the chain stores it (hex, maybe "Public/"-prefixed) -> b64url 32 bytes, or "" if malformed.
inline std::string lezDeviceToB64(std::string d) {
    if (d.rfind("Public/", 0) == 0) d = d.substr(7);
    if (d.size() == 43) { try { if (unb64url(d).size() == 32) return d; } catch (...) {} return ""; }
    if (d.size() != 64) return "";
    for (char c : d) if (!isxdigit((unsigned char)c)) return "";
    Bytes b; for (size_t i = 0; i < 64; i += 2) b.push_back((uint8_t)std::stoi(d.substr(i, 2), nullptr, 16));
    return b64url(b);
}

struct LezMember { std::string owner; std::vector<std::string> devices; int64_t paidThroughMs = 0; };
struct LezSnapshot { std::vector<LezMember> members; json config; };

// Every membership in the registry, with its devices and paid-through time.
inline LezSnapshot lezRead(const json& cfg) {
    std::string wallet = cfg.value("wallet", ""), seq = cfg.value("sequencer", ""), prog = cfg.value("program", "");
    if (wallet.empty() || seq.empty() || prog.size() != 64) throw Err("LEZ isn't set up for this repo (wallet, sequencer, program id)");
    LezFfi& f = LezFfi::get(cfg.value("lib", ""));
    json o = {{"wallet_path", wallet}, {"sequencer_url", seq}, {"program_id_hex", prog}};
    void* h = f.open(o.dump().c_str());
    if (!h) throw Err("LEZ: couldn't open the config (check the wallet folder and program id)");
    struct Closer { LezFfi& f; void* h; ~Closer() { f.close(h); } } closer{f, h};
    LezSnapshot snap; snap.config = f.take(f.getConfig(h));
    std::vector<std::string> pdas; uint64_t from = 0;
    for (int guard = 0; guard < 200; guard++) {
        json page = f.take(f.list(h, json{{"from_index", from}}.dump().c_str()));
        for (auto& e : page.value("entries", json::array())) if (e.contains("membership_pda_hex") && e["membership_pda_hex"].is_string()) pdas.push_back(e["membership_pda_hex"]);
        uint64_t next = page.value("next_cursor", from), upto = page.value("next_member_index", next);
        if (next == from || next >= upto) break;
        from = next;
    }
    if (pdas.size() > 5000) throw Err("LEZ: too many memberships");
    for (size_t i = 0; i < pdas.size(); i += 100) {
        std::vector<std::string> chunk(pdas.begin() + i, pdas.begin() + std::min(pdas.size(), i + 100));
        json r = f.take(f.getMany(h, json{{"membership_pdas_hex", chunk}}.dump().c_str()));
        for (auto& m : r.value("members", json::array())) {
            if (!m.value("found", false)) continue;
            LezMember lm; lm.owner = m.value("owner", std::string());
            const json& pt = m.contains("paid_through_ms") ? m["paid_through_ms"] : json();
            lm.paidThroughMs = pt.is_number() ? pt.get<int64_t>() : pt.is_string() ? std::atoll(pt.get<std::string>().c_str()) : 0;
            for (auto& d : m.value("devices", json::array())) if (d.is_string()) { std::string b = lezDeviceToB64(d); if (!b.empty()) lm.devices.push_back(b); }
            snap.members.push_back(lm);
        }
    }
    return snap;
}
}  // namespace bp
