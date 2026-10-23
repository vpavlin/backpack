// Backpack — install Android apps from repositories that live in Logos Storage.
import { useCallback, useEffect, useRef, useState } from "react";
import { ActivityIndicator, Alert, AppState, Image, Linking as RNLinking, Modal, Pressable, RefreshControl, ScrollView, StyleSheet, Text, TextInput, View } from "react-native";
import { SafeAreaProvider, SafeAreaView } from "react-native-safe-area-context";
import { StatusBar } from "expo-status-bar";
import * as Clipboard from "expo-clipboard";
import * as Linking from "expo-linking";
import * as R from "./src/repo";
import * as storage from "./src/storage";

const C = { bg: "#14161a", card: "#1d2026", line: "#2a2e36", text: "#e8e6e3", dim: "#9aa0a8", accent: "#e0a458", ok: "#7fbf7f", err: "#e07a6a" };
const mb = (n: number) => `${(n / 1e6).toFixed(n < 1e7 ? 1 : 0)} MB`;

export default function App() {
  return (<SafeAreaProvider><StatusBar style="light" /><Main /></SafeAreaProvider>);
}

function Main() {
  const [repos, setRepos] = useState<R.Repo[]>([]);
  const [busy, setBusy] = useState<string | null>(null);   // repo-level activity line
  const [adding, setAdding] = useState(false);
  const [link, setLink] = useState("");
  const [installed, setInstalled] = useState<Record<string, number>>({});
  const [stage, setStage] = useState<Record<string, string>>({}); // app id → install stage
  const [prog, setProg] = useState<Record<string, storage.Progress>>({}); // app id → download progress
  const [icons, setIcons] = useState<Record<string, string>>({});
  const [showDiag, setShowDiag] = useState(false);
  const [detail, setDetail] = useState<{ repo: R.Repo; app: R.App } | null>(null);
  const [updating, setUpdating] = useState<string | null>(null);  // "Updating Kith (1 of 2)…"
  const updatingRef = useRef(false);
  const lastRefresh = useRef(0);
  const [diagTick, setDiagTick] = useState(0);
  const [nodeLine, setNodeLine] = useState("");
  useEffect(() => storage.onDiag(() => setDiagTick((t) => t + 1)), []);
  useEffect(() => {
    if (!showDiag) return;
    let live = true;
    const poll = async () => { const l = await storage.nodeSummary(); if (live) setNodeLine(l); };
    poll(); const iv = setInterval(poll, 5000);
    return () => { live = false; clearInterval(iv); };
  }, [showDiag]);

  const scanInstalled = useCallback(async (rs: R.Repo[]) => {
    const out: Record<string, number> = {};
    if (!R.installer.available()) return out;
    for (const r of rs) for (const a of r.index?.apps ?? []) out[a.id] = await R.installer.installedVersion(a.id);
    setInstalled(out);
    return out;
  }, []);

  const loadIcons = useCallback(async (rs: R.Repo[]) => {
    for (const r of rs) for (const a of r.index?.apps ?? []) {
      if (!a.icon) continue;
      R.iconPath(r, a).then((p) => p && setIcons((m) => ({ ...m, [a.id]: `file://${p}` }))).catch(() => {});
    }
  }, []);

  const add = useCallback(async (l: string) => {
    setBusy("Adding repository…");
    try {
      const rs = await R.addFromLink(l, await R.loadRepos(), (s) => setBusy(`Index: ${s}…`));
      setRepos(rs); setAdding(false); setLink("");
      scanInstalled(rs); loadIcons(rs);
    } catch (e: any) { Alert.alert("Couldn't add the repository", String(e?.message ?? e)); }
    finally { setBusy(null); }
  }, [scanInstalled, loadIcons]);

  const refreshAll = useCallback(async () => {
    setBusy("Refreshing…");
    const out: R.Repo[] = [];
    for (const r of await R.loadRepos()) {
      try { out.push(await R.refresh(r, (s) => setBusy(`${r.name}: ${s}…`))); }
      catch (e: any) { out.push({ ...r, error: String(e?.message ?? e) }); }
    }
    await R.saveRepos(out); setRepos(out); setBusy(null); lastRefresh.current = Date.now();
    loadIcons(out);
    autoUpdate(out, await scanInstalled(out));
  }, [scanInstalled, loadIcons]);   // eslint-disable-line react-hooks/exhaustive-deps

  // first load + backpack:// links (opened from a message, a QR scan, a browser)
  useEffect(() => {
    storage.lastCrash().then((c) => c && Alert.alert("Backpack stopped last time", `It crashed inside the Logos Storage call: ${c}. Please tell the developer.`));
    // on start: show what we have, then ask each repo's pointers for anything newer (ADR 0003)
    // (opened by a link: adding it refreshes that repo — don't race it with a refresh of all)
    Promise.all([R.loadRepos(), Linking.getInitialURL()]).then(([rs, u]) => {
      setRepos(rs); scanInstalled(rs); loadIcons(rs);
      if (u && u.startsWith("backpack://repo")) add(u);
      else if (rs.length) refreshAll();
    });
    const sub = Linking.addEventListener("url", ({ url }) => { if (url.startsWith("backpack://repo")) add(url); });
    // back in the foreground: rescan (an install or uninstall may have finished), and look for
    // updates again if the last check is more than 15 minutes old
    const st = AppState.addEventListener("change", (s) => {
      if (s !== "active") return;
      if (Date.now() - lastRefresh.current > 15 * 60_000 && !updatingRef.current) refreshAll();
      else R.loadRepos().then(scanInstalled);
    });
    return () => { sub.remove(); st.remove(); };
  }, [add, refreshAll, scanInstalled, loadIcons]);

  const install = async (repo: R.Repo, app: R.App, quiet = false): Promise<string> => {
    const v = app.versions[0];
    const set = (s: string | null) => setStage((m) => { const n = { ...m }; if (s) n[app.id] = s; else delete n[app.id]; return n; });
    let res = "failed";
    try {
      set("starting");
      res = await R.install(repo, app, v, set, (p) => setProg((m) => ({ ...m, [app.id]: p })));
      if (res === "cancelled" && !quiet) Alert.alert("Not installed", "The install was cancelled.");
      if (res === "needs-permission") Alert.alert("Allow installs", `${app.label} is downloaded and checked. Android needs your permission for Backpack to install apps: turn on "Allow from this source", then come back and tap Install again (it won't download again).`,
        [{ text: "Later", style: "cancel" }, { text: "Open settings", onPress: () => R.installer.openSettings() }]);
    } catch (e: any) { Alert.alert(`Couldn't install ${app.label}`, String(e?.message ?? e)); }
    finally { set(null); setProg((m) => { const n = { ...m }; delete n[app.id]; return n; }); scanInstalled(await R.loadRepos()); }
    return res;
  };

  // After every refresh: download and install updates for installed apps, one at a time. Android asks
  // to confirm each one, except for apps Backpack itself installed (Android 12+ updates those silently).
  const autoUpdate = async (rs: R.Repo[], inst: Record<string, number>) => {
    if (updatingRef.current) return;
    const ups = await R.pendingUpdates(rs, inst);
    if (!ups.length) return;
    updatingRef.current = true;
    try {
      for (const [i, u] of ups.entries()) {
        setUpdating(`Updating ${u.app.label} to ${u.v.versionName}${ups.length > 1 ? ` (${i + 1} of ${ups.length})` : ""}…`);
        const res = await install(u.repo, u.app, true);
        if (res === "needs-permission") break;   // install() already explained; the rest would ask again
      }
    } finally { updatingRef.current = false; setUpdating(null); }
  };

  const remove = (r: R.Repo) => Alert.alert(`Remove "${r.name}"?`, "Apps you installed from it stay installed.", [
    { text: "Cancel", style: "cancel" },
    { text: "Remove", style: "destructive", onPress: async () => { const rs = repos.filter((x) => x.key !== r.key); await R.saveRepos(rs); setRepos(rs); } },
  ]);

  return (
    <SafeAreaView style={s.root} edges={["top", "bottom"]}>
      <View style={s.head}>
        <Text style={s.title}>🎒 Backpack</Text>
        <Pressable onPress={() => setShowDiag((d) => !d)} style={[s.headBtn, { marginRight: 8 }]}><Text style={s.headBtnT}>{showDiag ? "Hide log" : "Log"}</Text></Pressable>
        <Pressable onPress={() => setAdding((a) => !a)} style={s.headBtn}><Text style={s.headBtnT}>{adding ? "Close" : "+ Repo"}</Text></Pressable>
      </View>
      {!storage.available() && <Text style={[s.banner, { color: C.err }]}>Logos Storage isn't in this build — Backpack needs an arm64 phone.</Text>}
      {showDiag && (
        <View style={[s.card, { maxHeight: 320 }]}>
          <View style={s.row}>
            <Text style={[s.label, { flex: 1 }]}>Storage log</Text>
            <Pressable style={s.btnGhost} onPress={async () => { await Clipboard.setStringAsync(`${storage.diagText()}\n\n${await storage.nodeDebug()}`); Alert.alert("Copied", "The log and the node's state are on the clipboard."); }}><Text style={s.btnGhostT}>Copy</Text></Pressable>
          </View>
          <Text style={[s.hint, { fontFamily: "monospace", fontSize: 11 }]} numberOfLines={3}>{nodeLine || "node: …"}</Text>
          <ScrollView key={diagTick}>
            <Text style={[s.hint, { fontFamily: "monospace", fontSize: 11 }]}>{storage.diagText() || "nothing yet"}</Text>
          </ScrollView>
        </View>
      )}
      {busy && <View style={s.busy}><ActivityIndicator color={C.accent} size="small" /><Text style={s.busyT}>{busy}</Text></View>}
      {updating && !busy && <View style={s.busy}><ActivityIndicator color={C.accent} size="small" /><Text style={s.busyT}>{updating}</Text></View>}

      {adding && (
        <View style={s.card}>
          <Text style={s.label}>Repository link</Text>
          <TextInput value={link} onChangeText={setLink} placeholder="backpack://repo?i=…" placeholderTextColor={C.dim} style={s.input} autoCapitalize="none" autoCorrect={false} multiline />
          <View style={s.row}>
            <Pressable style={s.btnGhost} onPress={async () => setLink(await Clipboard.getStringAsync())}><Text style={s.btnGhostT}>Paste</Text></Pressable>
            <Pressable style={[s.btn, !link && { opacity: 0.4 }]} disabled={!link || !!busy} onPress={() => add(link)}><Text style={s.btnT}>Add</Text></Pressable>
          </View>
          <Text style={s.hint}>Or just open a backpack:// link on this phone.</Text>
        </View>
      )}

      <ScrollView contentContainerStyle={{ padding: 12, paddingBottom: 40 }} refreshControl={<RefreshControl refreshing={false} onRefresh={refreshAll} tintColor={C.accent} />}>
        {repos.length === 0 && !adding && (
          <View style={s.empty}>
            <Text style={s.emptyT}>Your backpack is empty.</Text>
            <Text style={s.hint}>Add a repository with "+ Repo". Everything it holds — the list, the icons, the APKs — comes from Logos Storage.</Text>
          </View>
        )}
        {repos.map((r) => (
          <View key={r.key} style={{ marginBottom: 18 }}>
            <Pressable onLongPress={() => remove(r)} style={s.repoHead}>
              <Text style={s.repoName}>{r.name}</Text>
              <Text style={s.repoMeta}>{r.index ? `${r.index.apps.length} apps · #${r.seq ?? 0} · ${new Date(r.index.created).toLocaleDateString()}${r.checkedAt ? ` · checked ${new Date(r.checkedAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}` : ""}` : "not fetched"}</Text>
              <Text style={s.repoKey}>publisher key {keyShort(r.key)}</Text>
            </Pressable>
            {r.error && <Text style={[s.hint, { color: C.err }]}>{r.error}</Text>}
            {(r.index?.apps ?? []).map((a) => {
              const v = a.versions[0]; const have = installed[a.id] ?? -1; const st = stage[a.id];
              const action = have < 0 ? "Install" : have < v.versionCode ? "Update" : "Open";
              return (
                <Pressable key={a.id} style={s.app} onPress={() => setDetail({ repo: r, app: a })}>
                  {icons[a.id] ? <Image source={{ uri: icons[a.id] }} style={s.icon} /> : <View style={[s.icon, s.iconPh]}><Text style={{ color: C.dim }}>{a.label.slice(0, 1)}</Text></View>}
                  <View style={{ flex: 1 }}>
                    <Text style={s.appName} numberOfLines={1}>{a.label}</Text>
                    <Text style={s.appMeta} numberOfLines={1}>{v.versionName} · {mb(v.size)}{have >= 0 ? ` · installed ${have === v.versionCode ? "" : `(${have})`}` : ""}</Text>
                    {st && !prog[a.id] && <Text style={[s.appMeta, { color: C.accent }]}>{st}…</Text>}
                    {st && prog[a.id] && <ProgressBar p={prog[a.id]} stage={st} />}
                  </View>
                  <Pressable disabled={!!st} style={[action === "Open" ? s.btnGhost : s.btn, st && { opacity: 0.4 }]}
                    onPress={() => (action === "Open" ? R.installer.launch(a.id) : install(r, a))}>
                    {st ? <ActivityIndicator color={C.bg} size="small" /> : <Text style={action === "Open" ? s.btnGhostT : s.btnT}>{action}</Text>}
                  </Pressable>
                </Pressable>
              );
            })}
          </View>
        ))}
      </ScrollView>
      {/* rendered only while open: a hidden RN Modal still renders its body */}
      {detail && (
        <Modal visible animationType="slide" onRequestClose={() => setDetail(null)}>
          <AppDetail repo={detail.repo} app={detail.app} repos={repos} icon={icons[detail.app.id]} have={installed[detail.app.id] ?? -1}
            stage={stage[detail.app.id]} prog={prog[detail.app.id]} onClose={() => setDetail(null)}
            onInstall={() => install(detail.repo, detail.app)} />
        </Modal>
      )}
    </SafeAreaView>
  );
}

const keyShort = (k: string) => `${k.slice(0, 6)}…${k.slice(-4)}`;
const certShort = (h?: string) => (h ? `${h.slice(0, 8)}…${h.slice(-8)}` : "not pinned");

function AppDetail({ repo, app, repos, icon, have, stage, prog, onClose, onInstall }: {
  repo: R.Repo; app: R.App; repos: R.Repo[]; icon?: string; have: number; stage?: string; prog?: storage.Progress;
  onClose: () => void; onInstall: () => void;
}) {
  const v = app.versions[0];
  const [installedBy, setInstalledBy] = useState<string>("");
  const [installedSigners, setInstalledSigners] = useState<string[] | null>(null);
  useEffect(() => {
    if (have < 0) { setInstalledSigners([]); setInstalledBy(""); return; }
    R.installer.installedSigners(app.id).then(setInstalledSigners).catch(() => setInstalledSigners([]));
    R.installer.installerOf(app.id).then(setInstalledBy).catch(() => {});
  }, [app.id, have]);
  const sameKey = installedSigners && v.signer ? installedSigners.includes(v.signer) : null;
  const others = R.publishersOf(repos, app.id).filter((p) => p.repo.key !== repo.key);
  const action = have < 0 ? "Install" : have < v.versionCode ? "Update" : null;
  return (
    <SafeAreaView style={s.root} edges={["top", "bottom"]}>
      <View style={s.head}>
        <Pressable onPress={onClose} style={s.headBtn}><Text style={s.headBtnT}>‹ Back</Text></Pressable>
      </View>
      <ScrollView contentContainerStyle={{ padding: 16, paddingBottom: 40 }}>
        <View style={[s.row, { alignItems: "center", marginBottom: 14 }]}>
          {icon ? <Image source={{ uri: icon }} style={s.iconBig} /> : <View style={[s.iconBig, s.iconPh]}><Text style={{ color: C.dim, fontSize: 24 }}>{app.label.slice(0, 1)}</Text></View>}
          <View style={{ flex: 1, marginLeft: 14 }}>
            <Text style={s.detailTitle}>{app.label}</Text>
            <Text style={s.appMeta}>{app.id}</Text>
            <Text style={s.appMeta}>{v.versionName} · {mb(v.size)}{have >= 0 ? ` · installed ${have === v.versionCode ? "(latest)" : `vc ${have}`}` : ""}</Text>
          </View>
        </View>
        {stage && prog && <ProgressBar p={prog} stage={stage} />}
        {stage && !prog && <Text style={[s.appMeta, { color: C.accent }]}>{stage}…</Text>}
        <View style={[s.row, { marginVertical: 12 }]}>
          {action && <Pressable disabled={!!stage} style={[s.btn, { flex: 1, marginRight: 8 }, stage && { opacity: 0.4 }]} onPress={onInstall}><Text style={s.btnT}>{action}</Text></Pressable>}
          {have >= 0 && <Pressable style={[s.btnGhost, { flex: 1, marginRight: 8 }]} onPress={() => R.installer.launch(app.id)}><Text style={s.btnGhostT}>Open</Text></Pressable>}
          {have >= 0 && <Pressable style={[s.btnGhost, { flex: 1 }]} onPress={() => R.installer.uninstall(app.id)}><Text style={[s.btnGhostT, { color: C.err }]}>Uninstall</Text></Pressable>}
        </View>
        {!!app.summary && <Text style={s.summary}>{app.summary}</Text>}
        {!!app.description && <Text style={s.desc}>{app.description}</Text>}

        <Text style={s.section}>Publisher</Text>
        <Text style={s.kv}>Repository  <Text style={s.kvV}>{repo.name}</Text></Text>
        <Text style={s.kv}>Repository key  <Text style={s.kvV}>{keyShort(repo.key)}</Text></Text>
        <Text style={s.kv}>APK signed by  <Text style={s.kvV}>{certShort(v.signer)}</Text></Text>
        {have >= 0 && sameKey !== null && (
          <Text style={[s.kv, { color: sameKey ? C.ok : C.err }]}>
            {sameKey ? "✓ Same signing key as the app installed on this phone" : "✗ The installed app is signed by a different key: Android won't update it from this repository"}
          </Text>
        )}
        {have >= 0 && !!installedBy && <Text style={s.kv}>Installed by  <Text style={s.kvV}>{installedBy === "xyz.vpavlin.backpack" ? "Backpack" : installedBy}</Text></Text>}
        {others.map((o) => (
          <Text key={o.repo.key} style={[s.kv, { color: o.signer && v.signer && o.signer !== v.signer ? C.err : C.dim }]}>
            {o.signer && v.signer && o.signer !== v.signer
              ? `⚠ "${o.repo.name}" (key ${keyShort(o.repo.key)}) also lists ${app.label}, signed by a different key (${certShort(o.signer)}). Only one of them is the real publisher.`
              : `Also in "${o.repo.name}" (${o.versionName}), same signing key.`}
          </Text>
        ))}

        <Text style={s.section}>Details</Text>
        {!!app.license && app.license !== "Unknown" && <Text style={s.kv}>Licence  <Text style={s.kvV}>{app.license}</Text></Text>}
        {!!app.source && <Text style={s.kv} onPress={() => app.source && RNLinking.openURL(app.source)}>Source  <Text style={[s.kvV, { color: C.accent }]}>{app.source}</Text></Text>}
        <Text style={s.kv}>Needs Android  <Text style={s.kvV}>{sdkName(v.minSdk)}+</Text></Text>
        {!!v.abis?.length && <Text style={s.kv}>Built for  <Text style={s.kvV}>{v.abis.join(", ")}</Text></Text>}
        <Text style={s.kv}>SHA-256  <Text style={s.kvV}>{certShort(v.sha256)}</Text></Text>

        <Text style={s.section}>Versions in this repository</Text>
        {app.versions.map((x) => (
          <Text key={x.versionCode} style={s.kv}>{x.versionName}  <Text style={s.kvV}>vc {x.versionCode} · {mb(x.size)}{x.added ? ` · ${new Date(x.added).toLocaleDateString()}` : ""}</Text></Text>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}

const sdkName = (n: number) => ({ 21: "5.0", 22: "5.1", 23: "6", 24: "7", 25: "7.1", 26: "8", 27: "8.1", 28: "9", 29: "10", 30: "11", 31: "12", 32: "12L", 33: "13", 34: "14", 35: "15", 36: "16" } as Record<number, string>)[n] ?? `API ${n}`;

function ProgressBar({ p, stage }: { p: storage.Progress; stage: string }) {
  const frac = p.total ? Math.min(1, p.bytes / p.total) : 0;
  const left = p.rate > 512 && p.total ? Math.max(0, (p.total - p.bytes) / p.rate) : -1;
  const eta = left < 0 ? "" : left < 90 ? ` · ${Math.ceil(left)} s left` : ` · ${Math.ceil(left / 60)} min left`;
  // fetch returns once the manifest is in and the blocks mostly arrive during the write (download_stream),
  // so both phases are "downloading" to the user; the bytes are measured where they land in each phase.
  const busy = stage.startsWith("fetching") || stage.startsWith("writing");
  const retry = / \(try \d+\)/.exec(stage)?.[0] ?? "";
  const label = busy
    ? `downloading${retry} ${mb(p.bytes)}${p.total ? ` / ${mb(p.total)} · ${Math.floor(frac * 100)}%` : ""} · ${(p.rate / 1024).toFixed(0)} KB/s${eta}`
    : `${stage}…`;
  return (
    <View style={{ marginTop: 4 }}>
      <View style={s.barBg}><View style={[s.barFg, { width: `${Math.round(frac * 100)}%` }]} /></View>
      <Text style={[s.appMeta, { color: C.accent }]} numberOfLines={2}>{label}</Text>
    </View>
  );
}

const s = StyleSheet.create({
  repoKey: { color: C.dim, fontSize: 11, fontFamily: "monospace", width: "100%" },
  iconBig: { width: 64, height: 64, borderRadius: 14 },
  detailTitle: { color: C.text, fontSize: 22, fontWeight: "700" },
  summary: { color: C.text, fontSize: 16, fontWeight: "600", marginBottom: 6 },
  desc: { color: C.text, fontSize: 14, lineHeight: 20, marginBottom: 6 },
  section: { color: C.accent, fontSize: 12, fontWeight: "700", letterSpacing: 1, textTransform: "uppercase", marginTop: 18, marginBottom: 6 },
  kv: { color: C.dim, fontSize: 13, lineHeight: 20, marginBottom: 2 },
  kvV: { color: C.text, fontFamily: "monospace", fontSize: 12 },
  barBg: { height: 6, borderRadius: 3, backgroundColor: C.line, overflow: "hidden", marginBottom: 3 },
  barFg: { height: 6, borderRadius: 3, backgroundColor: C.accent },
  root: { flex: 1, backgroundColor: C.bg },
  head: { flexDirection: "row", alignItems: "center", paddingHorizontal: 16, paddingVertical: 12, borderBottomWidth: 1, borderBottomColor: C.line },
  title: { flex: 1, color: C.text, fontSize: 22, fontWeight: "700" },
  headBtn: { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 8, borderWidth: 1, borderColor: C.accent },
  headBtnT: { color: C.accent, fontWeight: "600" },
  banner: { padding: 12 },
  busy: { flexDirection: "row", alignItems: "center", gap: 8, paddingHorizontal: 16, paddingVertical: 8, backgroundColor: C.card },
  busyT: { color: C.dim },
  card: { margin: 12, padding: 12, backgroundColor: C.card, borderRadius: 12, gap: 8 },
  label: { color: C.dim, fontSize: 12, textTransform: "uppercase", letterSpacing: 1 },
  input: { color: C.text, backgroundColor: C.bg, borderRadius: 8, padding: 10, minHeight: 60, borderWidth: 1, borderColor: C.line, fontFamily: "monospace", fontSize: 12 },
  row: { flexDirection: "row", gap: 8, justifyContent: "flex-end" },
  btn: { backgroundColor: C.accent, paddingHorizontal: 14, paddingVertical: 8, borderRadius: 8, minWidth: 76, alignItems: "center" },
  btnT: { color: C.bg, fontWeight: "700" },
  btnGhost: { borderWidth: 1, borderColor: C.line, paddingHorizontal: 14, paddingVertical: 8, borderRadius: 8, minWidth: 76, alignItems: "center" },
  btnGhostT: { color: C.text, fontWeight: "600" },
  hint: { color: C.dim, fontSize: 13, lineHeight: 18 },
  empty: { padding: 24, gap: 8, alignItems: "center" },
  emptyT: { color: C.text, fontSize: 17 },
  repoHead: { flexDirection: "row", flexWrap: "wrap", alignItems: "baseline", paddingVertical: 6 },
  repoName: { flex: 1, color: C.accent, fontSize: 15, fontWeight: "700" },
  repoMeta: { color: C.dim, fontSize: 12 },
  app: { flexDirection: "row", alignItems: "center", gap: 12, padding: 10, backgroundColor: C.card, borderRadius: 12, marginTop: 8 },
  icon: { width: 44, height: 44, borderRadius: 10 },
  iconPh: { backgroundColor: C.line, alignItems: "center", justifyContent: "center" },
  appName: { color: C.text, fontSize: 16, fontWeight: "600" },
  appMeta: { color: C.dim, fontSize: 12, marginTop: 2 },
});
