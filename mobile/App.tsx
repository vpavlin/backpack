// Backpack — install Android apps from repositories that live in Logos Storage.
import { useCallback, useEffect, useState } from "react";
import { ActivityIndicator, Alert, AppState, Image, Pressable, RefreshControl, ScrollView, StyleSheet, Text, TextInput, View } from "react-native";
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
  const [icons, setIcons] = useState<Record<string, string>>({});
  const [showDiag, setShowDiag] = useState(false);
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
    if (!R.installer.available()) return;
    const out: Record<string, number> = {};
    for (const r of rs) for (const a of r.index?.apps ?? []) out[a.id] = await R.installer.installedVersion(a.id);
    setInstalled(out);
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
    await R.saveRepos(out); setRepos(out); setBusy(null);
    scanInstalled(out); loadIcons(out);
  }, [scanInstalled, loadIcons]);

  // first load + backpack:// links (opened from a message, a QR scan, a browser)
  useEffect(() => {
    storage.lastCrash().then((c) => c && Alert.alert("Backpack stopped last time", `It crashed inside the Logos Storage call: ${c}. Please tell the developer.`));
    R.loadRepos().then((rs) => { setRepos(rs); scanInstalled(rs); loadIcons(rs); });
    Linking.getInitialURL().then((u) => { if (u && u.startsWith("backpack://repo")) add(u); });
    const sub = Linking.addEventListener("url", ({ url }) => { if (url.startsWith("backpack://repo")) add(url); });
    const st = AppState.addEventListener("change", (s) => { if (s === "active") R.loadRepos().then(scanInstalled); });
    return () => { sub.remove(); st.remove(); };
  }, [add, scanInstalled, loadIcons]);

  const install = async (repo: R.Repo, app: R.App) => {
    const v = app.versions[0];
    if (!(await R.installer.canInstall())) {
      Alert.alert("Allow installs", "Android needs your permission for Backpack to install apps. Turn on \"Allow from this source\", then come back and tap Install again.",
        [{ text: "Cancel", style: "cancel" }, { text: "Open settings", onPress: () => R.installer.openSettings() }]);
      return;
    }
    const set = (s: string | null) => setStage((m) => { const n = { ...m }; if (s) n[app.id] = s; else delete n[app.id]; return n; });
    try {
      set("starting");
      const res = await R.install(repo, app, v, set);
      if (res === "cancelled") Alert.alert("Not installed", "The install was cancelled.");
    } catch (e: any) { Alert.alert(`Couldn't install ${app.label}`, String(e?.message ?? e)); }
    finally { set(null); scanInstalled(repos); }
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
              <Text style={s.repoMeta}>{r.index ? `${r.index.apps.length} apps · ${new Date(r.index.created).toLocaleDateString()}` : "not fetched"}</Text>
            </Pressable>
            {r.error && <Text style={[s.hint, { color: C.err }]}>{r.error}</Text>}
            {(r.index?.apps ?? []).map((a) => {
              const v = a.versions[0]; const have = installed[a.id] ?? -1; const st = stage[a.id];
              const action = have < 0 ? "Install" : have < v.versionCode ? "Update" : "Open";
              return (
                <View key={a.id} style={s.app}>
                  {icons[a.id] ? <Image source={{ uri: icons[a.id] }} style={s.icon} /> : <View style={[s.icon, s.iconPh]}><Text style={{ color: C.dim }}>{a.label.slice(0, 1)}</Text></View>}
                  <View style={{ flex: 1 }}>
                    <Text style={s.appName} numberOfLines={1}>{a.label}</Text>
                    <Text style={s.appMeta} numberOfLines={1}>{v.versionName} · {mb(v.size)}{have >= 0 ? ` · installed ${have === v.versionCode ? "" : `(${have})`}` : ""}</Text>
                    {st && <Text style={[s.appMeta, { color: C.accent }]}>{st}…</Text>}
                  </View>
                  <Pressable disabled={!!st} style={[action === "Open" ? s.btnGhost : s.btn, st && { opacity: 0.4 }]}
                    onPress={() => (action === "Open" ? R.installer.launch(a.id) : install(r, a))}>
                    {st ? <ActivityIndicator color={C.bg} size="small" /> : <Text style={action === "Open" ? s.btnGhostT : s.btnT}>{action}</Text>}
                  </Pressable>
                </View>
              );
            })}
          </View>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
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
  repoHead: { flexDirection: "row", alignItems: "baseline", paddingVertical: 6 },
  repoName: { flex: 1, color: C.accent, fontSize: 15, fontWeight: "700" },
  repoMeta: { color: C.dim, fontSize: 12 },
  app: { flexDirection: "row", alignItems: "center", gap: 12, padding: 10, backgroundColor: C.card, borderRadius: 12, marginTop: 8 },
  icon: { width: 44, height: 44, borderRadius: 10 },
  iconPh: { backgroundColor: C.line, alignItems: "center", justifyContent: "center" },
  appName: { color: C.text, fontSize: 16, fontWeight: "600" },
  appMeta: { color: C.dim, fontSize: 12, marginTop: 2 },
});
