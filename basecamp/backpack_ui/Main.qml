// Backpack - a Basecamp view over backpack_core (ADR 0011): the Backpack repos this computer publishes.
//
// Pure QML: every action runs in backpack_core (callVia = logos.callModuleAsync, never a blocking call).
// The view renders backpack_core.snapshot(), polled (fast while a job runs), and repo(slug) for the
// selected repo. Peer data is shown as plain text.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import Logos.Theme
import Logos.Controls

Item {
    id: root
    width: 1100
    height: 720

    // ── colours: design-system tokens with fallbacks (a missing token is undefined, not fatal)
    function tok(v, fb) { return (v === undefined || v === null) ? fb : v }
    readonly property var pal: (typeof Theme !== "undefined" && Theme.palette) ? Theme.palette : ({})
    readonly property color cBg: tok(pal.background, "#141416")
    readonly property color cSurface: tok(pal.surface, "#1c1c20")
    readonly property color cRaised: tok(pal.surfaceRaised, "#26262c")
    readonly property color cText: tok(pal.text, "#ececf1")
    readonly property color cSub: tok(pal.textSecondary, "#a8a8b3")
    readonly property color cFaint: tok(pal.textTertiary, "#74747f")
    readonly property color cLine: tok(pal.borderHairline, "#2e2e36")
    readonly property color cAccent: tok(pal.primary, "#e0a050")
    readonly property color cOk: tok(pal.success, "#5fbf7f")
    readonly property color cBad: tok(pal.error, "#ef6f6c")
    readonly property color cWarn: tok(pal.warning, "#e8b84a")

    // ── core bridge
    readonly property string coreName: "backpack_core"
    readonly property int callTimeoutMs: 30000
    function _deliver(cb, raw) { if (!cb) return; try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn("backpack view: " + e) } }
    function callVia(module, method, args, cb) {
        var a = args || []
        if (typeof logos === "undefined" || logos === null) { Qt.callLater(function () { root._deliver(cb, "") }); return }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync(module, method, a, function (res) { root._deliver(cb, res) }, root.callTimeoutMs) }
            catch (e) { Qt.callLater(function () { root._deliver(cb, "") }) }
            return
        }
        Qt.callLater(function () { var raw = ""; try { raw = logos.callModule(module, method, a) } catch (e) {} root._deliver(cb, raw) })
    }
    function core(method, args, cb) { root.callVia(root.coreName, method, args, cb) }
    function parse(raw) {   // JSON, or JSON quoted once or twice
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) { var s = v.trim(); if (s === "") return null; try { v = JSON.parse(s) } catch (e) { return null } }
        return (v && typeof v === "object") ? v : null
    }
    // An action: toast its outcome, refresh, optional callback with the result.
    function act(method, args, okMsg, then) {
        root.core(method, args, function (raw) {
            var r = root.parse(raw)
            if (!r) { root.toast("No answer from backpack_core - is it installed and loaded?", true); return }
            if (r.ok === false) { root.toast(r.error || "Failed", true); return }
            if (okMsg) root.toast(okMsg, false)
            if (then) then(r)
            root.refresh()
        })
    }

    // ── state
    property var st: ({ repos: [], jobs: [], dataDir: "", version: "" })
    property var cur: null              // repo(slug) of the selected repo
    property string sel: ""
    property bool coreSeen: false
    property int missed: 0
    property bool busy: false
    property bool again: false
    readonly property bool jobRunning: { for (var i = 0; i < st.jobs.length; i++) if (st.jobs[i].state === "running") return true; return false }
    readonly property var curJobs: { var o = []; for (var i = 0; i < st.jobs.length; i++) if (st.jobs[i].repo === root.sel) o.push(st.jobs[i]); return o }

    function refresh() {
        if (root.busy) { root.again = true; return }
        root.busy = true
        root.core("snapshot", [], function (raw) {
            var s = root.parse(raw)
            if (s && s.ok !== false) {
                root.st = s; root.coreSeen = true; root.missed = 0
                if (!root.sel && s.repos.length) root.sel = s.repos[0].slug
                root.loadRepo()
            } else root.missed++
            root.busy = false
            if (root.again) { root.again = false; root.refresh() }
        })
    }
    function loadRepo() {
        if (!root.sel) { root.cur = null; return }
        var want = root.sel
        root.core("repo", [want], function (raw) { var r = root.parse(raw); if (r && r.ok && root.sel === want) root.cur = r.repo })
    }
    Timer { interval: root.jobRunning ? 1000 : 3000; running: true; repeat: true; onTriggered: root.refresh() }
    Component.onCompleted: root.refresh()

    // ── helpers
    // A members repo hands out its key only through the update note: with no web address phones read, or
    // nowhere publish sends it, no phone can ever get in (the link carries no key)
    function noteMissing() { return !!(root.cur && root.cur.members && (!root.cur.pointersGet.length || !root.cur.pointersPut.length)) }
    readonly property string noteMissingText: "No phone can read this repo yet: phones get their key from the update note, and " + (root.cur && !root.cur.pointersGet.length ? "it has no web address (Settings)." : "publish sends it nowhere (backpack config --pointer-put).")
    function lezActive(list) { var n = 0; for (var i = 0; i < list.length; i++) if (list[i].paidThroughMs > Date.now()) n++; return n }
    function keyShort(k) { return k ? k.slice(0, 6) + "…" + k.slice(-4) : "" }
    function mb(n) { return n >= 1e6 ? (n / 1e6).toFixed(1) + " MB" : Math.round(n / 1e3) + " kB" }
    // Test a Storage node and show the outcome in `label`
    function testNode(url, label) {
        label.color = root.cSub; label.text = "Asking " + url + "…"
        root.core("testNode", [url], function (raw) {
            var r = root.parse(raw)
            if (!r) { label.color = root.cBad; label.text = "No answer from backpack_core"; return }
            if (!r.ok) { label.color = root.cBad; label.text = r.error; return }
            label.color = r.warning ? root.cWarn : root.cOk
            label.text = "Answers (peer " + String(r.peerId).slice(-8) + ", " + (r.addrs || []).length + " address(es))" + (r.warning ? ". But it's " + r.warning + ": phones outside that network can't reach it." : ".")
        })
    }
    function urlToPath(u) { var s = String(u); return s.indexOf("file://") === 0 ? decodeURIComponent(s.slice(7)) : s }
    TextEdit { id: clip; visible: false }
    function copyText(t) { clip.text = t; clip.selectAll(); clip.copy(); clip.deselect(); toast("Copied", false) }
    property string toastText: ""
    property bool toastBad: false
    function toast(t, bad) { root.toastText = t; root.toastBad = !!bad; toastTimer.restart() }
    Timer { id: toastTimer; interval: 4500; onTriggered: root.toastText = "" }

    component DField: TextField {
        Layout.fillWidth: true; Layout.preferredWidth: 1; color: root.cText; placeholderTextColor: root.cFaint; font.pixelSize: 14; selectByMouse: true
        background: Rectangle { radius: 6; color: root.cBg; border.width: 1; border.color: parent.activeFocus ? root.cAccent : root.cLine }
    }
    component T: Text { color: root.cText; font.pixelSize: 14; textFormat: Text.PlainText; wrapMode: Text.Wrap }
    component Sub: Text { color: root.cSub; font.pixelSize: 12; textFormat: Text.PlainText; wrapMode: Text.Wrap }
    component Badge: Rectangle {
        property alias text: bt.text; property color tone: root.cAccent
        implicitWidth: bt.implicitWidth + 12; implicitHeight: 20; radius: 10; color: "transparent"; border.color: tone; border.width: 1
        Text { id: bt; anchors.centerIn: parent; color: parent.tone; font.pixelSize: 11; textFormat: Text.PlainText }
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    RowLayout {
        anchors.fill: parent; spacing: 0

        // ── sidebar: repos
        Rectangle {
            Layout.preferredWidth: 250; Layout.fillHeight: true; color: root.cSurface
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                RowLayout {
                    Text { text: "🎒"; font.pixelSize: 22 }
                    T { text: "Backpack"; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true }
                }
                Sub { text: "Your app repos on Logos Storage"; Layout.fillWidth: true }
                Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                    model: root.st.repos
                    delegate: Rectangle {
                        width: ListView.view.width; height: 58; radius: 8
                        color: modelData.slug === root.sel ? root.cRaised : (ma.containsMouse ? Qt.darker(root.cRaised, 1.2) : "transparent")
                        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; onClicked: { root.sel = modelData.slug; root.cur = null; root.loadRepo() } }
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 2
                            RowLayout {
                                T { text: (modelData.members ? "👥 " : modelData.private ? "🔒 " : "") + (modelData.name || modelData.slug); font.bold: true; elide: Text.ElideRight; wrapMode: Text.NoWrap; Layout.fillWidth: true }
                                Rectangle { visible: !!modelData.busy; width: 8; height: 8; radius: 4; color: root.cWarn }
                            }
                            Sub { text: modelData.error ? modelData.error : ("#" + modelData.seq + " · " + modelData.appCount + " apps · " + modelData.slug); elide: Text.ElideRight; wrapMode: Text.NoWrap; Layout.fillWidth: true }
                        }
                    }
                }
                LogosButton { text: "+ New repo"; Layout.fillWidth: true; onClicked: newDialog.open() }
                LogosButton { text: "Signing keys"; Layout.fillWidth: true; onClicked: keysDialog.show() }
                Sub { text: root.coreSeen ? ("Repos in " + root.st.dataDir + " · core " + root.st.version) : (root.missed > 2 ? "Can't reach backpack_core" : "Connecting…"); Layout.fillWidth: true; color: !root.coreSeen && root.missed > 2 ? root.cBad : root.cFaint }
            }
        }

        // ── repo page
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            T { visible: !root.cur; anchors.centerIn: parent; color: root.cSub; text: root.st.repos.length ? "Pick a repo" : "No repos yet. Make one with + New repo, or point Backpack at the CLI's folder." }
            ColumnLayout {
                visible: !!root.cur
                anchors.fill: parent; anchors.margins: 18; spacing: 12

                // header
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4
                        RowLayout {
                            T { text: root.cur ? root.cur.name : ""; font.pixelSize: 22; font.bold: true }
                            Badge { visible: !!(root.cur && root.cur.private && !root.cur.members); text: "🔒 private"; tone: root.cWarn }
                            Badge { visible: !!(root.cur && root.cur.members); text: "👥 members · " + (root.cur && root.cur.memberDevices ? root.cur.memberDevices.length : 0) + " devices"; tone: root.cAccent }
                            Badge { visible: !!(root.cur && !root.cur.private); text: "public"; tone: root.cOk }
                            Badge { visible: !!(root.cur && root.cur.busy); text: "working…"; tone: root.cAccent }
                        }
                        Sub { text: root.cur ? ("#" + root.cur.seq + " · publisher key " + root.keyShort(root.cur.key) + " · Storage node " + (root.cur.node || "not set")) : ""; Layout.fillWidth: true }
                        Sub { visible: !!(root.cur && root.cur.nodeWarning); color: root.cWarn; Layout.fillWidth: true
                              text: root.cur ? (root.cur.node ? "The Storage node is " + root.cur.nodeWarning + ": phones outside that network can't fetch this repo." : "Set a Storage node in Settings before publishing.") : "" }
                        Sub { visible: root.noteMissing(); color: root.cBad; Layout.fillWidth: true; text: root.noteMissingText }
                        Sub { text: root.cur ? ("Update note: " + (root.cur.pointersGet.length ? root.cur.pointersGet.join(", ") : "no web address") + (root.cur.pointersPut.length ? " · sent via " + root.cur.pointersPut.join(", ") : " · not sent anywhere")) : ""; Layout.fillWidth: true }
                    }
                }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    LogosButton { text: "Add APKs"; enabled: !!root.cur && !root.cur.busy; onClicked: apkDialog.open() }
                    LogosButton { text: "Import F-Droid"; enabled: !!root.cur && !root.cur.busy; onClicked: importDialog.open() }
                    LogosButton { text: "Publish"; enabled: !!root.cur && !root.cur.busy; onClicked: root.act("publish", [root.sel, "0"], "Publishing…") }
                    LogosButton { text: "Verify"; enabled: !!root.cur && !root.cur.busy; onClicked: root.act("verify", [root.sel, "1"], "Checking every APK the way a phone does…") }
                    LogosButton { text: "Invite link"; enabled: !!root.cur && root.cur.hasLink; onClicked: linkDialog.show() }
                    LogosButton { text: "Members"; visible: !!(root.cur && root.cur.members); enabled: !!root.cur && !root.cur.busy; onClicked: membersDialog.open() }
                    LogosButton {
                        visible: !(root.cur && root.cur.members)
                        text: root.cur && root.cur.private ? "Rotate read key" : "Make private"
                        enabled: !!root.cur && !root.cur.busy
                        onClicked: confirmDialog.ask(root.cur.private
                            ? "Rotate the read key? Everyone needs the NEW link after the next publish; what they could already read stays readable to them."
                            : "Make this repo private? The next publish encrypts every file again under new keys and moves the update note. Phones need the new link. What was published before stays public.",
                            function () { root.act(root.cur.private ? "rotateReadKey" : "makePrivate", [root.sel], "Done: publish to apply it") })
                    }
                    LogosButton { text: "Settings"; enabled: !!root.cur && !root.cur.busy; onClicked: settingsDialog.show() }
                }

                // apps + jobs
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 10; color: root.cSurface
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 12; spacing: 6
                            T { text: "Apps (" + (root.cur ? root.cur.apps.length : 0) + ")"; font.bold: true }
                            Sub { visible: !!root.cur && root.cur.apps.length === 0; text: "No apps yet: add APK files or import an F-Droid repo, then Publish." }
                            ListView {
                                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6
                                model: root.cur ? root.cur.apps : []
                                delegate: Rectangle {
                                    width: ListView.view.width; height: appCol.implicitHeight + 14; radius: 8; color: root.cRaised
                                    ColumnLayout {
                                        id: appCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; spacing: 3
                                        T { text: modelData.label; font.bold: true }
                                        Sub { text: modelData.id + (modelData.summary ? " · " + modelData.summary : "") }
                                        Repeater {
                                            model: modelData.versions
                                            RowLayout {
                                                property var v: modelData
                                                Sub { text: v.versionName + " (" + v.versionCode + ") · " + root.mb(v.size) + (v.encrypted ? " · encrypted" : "") + (v.added ? " · added " + v.added.slice(0, 10) : ""); Layout.fillWidth: true }
                                                Text {
                                                    text: "remove"; color: root.cBad; font.pixelSize: 12; textFormat: Text.PlainText
                                                    MouseArea { anchors.fill: parent; enabled: !!root.cur && !root.cur.busy
                                                        onClicked: { root.act("removeVersion", [root.sel, appCol.appId, String(v.versionCode)], "Removed: publish to make it visible") } }
                                                }
                                            }
                                        }
                                        property string appId: modelData.id
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        Layout.preferredWidth: 380; Layout.fillHeight: true; radius: 10; color: root.cSurface
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 12; spacing: 6
                            RowLayout { T { text: "Activity"; font.bold: true; Layout.fillWidth: true } Text { text: "clear"; color: root.cFaint; font.pixelSize: 12; MouseArea { anchors.fill: parent; onClicked: root.act("clearJobs", [], "") } } }
                            Sub { visible: root.curJobs.length === 0; text: "Publishing, imports and checks show up here." }
                            ListView {
                                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8
                                model: root.curJobs
                                delegate: Rectangle {
                                    width: ListView.view.width; height: jc.implicitHeight + 12; radius: 8; color: root.cRaised
                                    ColumnLayout {
                                        id: jc; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; spacing: 3
                                        RowLayout {
                                            T { text: modelData.kind; font.bold: true; Layout.fillWidth: true }
                                            Badge { text: modelData.state; tone: modelData.state === "done" ? root.cOk : modelData.state === "failed" ? root.cBad : root.cAccent }
                                        }
                                        ProgressBar {
                                            Layout.fillWidth: true
                                            visible: modelData.state === "running" && modelData.progress.total > 0
                                            from: 0; to: Math.max(1, modelData.progress.total); value: modelData.progress.done
                                        }
                                        Sub { visible: modelData.state === "running" && modelData.progress.total > 0; text: modelData.progress.what + " · " + root.mb(modelData.progress.done) + " / " + root.mb(modelData.progress.total) }
                                        Sub { text: modelData.log.slice(-8).join("\n"); Layout.fillWidth: true; font.family: "monospace"; font.pixelSize: 11 }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── toast
    Rectangle {
        visible: root.toastText !== ""; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: 18
        width: Math.min(parent.width - 40, tt.implicitWidth + 28); height: tt.implicitHeight + 16; radius: 8
        color: root.toastBad ? Qt.darker(root.cBad, 1.8) : root.cRaised; border.color: root.toastBad ? root.cBad : root.cLine
        Text { id: tt; anchors.centerIn: parent; width: parent.width - 28; text: root.toastText; color: root.cText; font.pixelSize: 13; wrapMode: Text.Wrap; textFormat: Text.PlainText }
    }

    // ── dialogs
    FileDialog {
        id: apkDialog; title: "APK files to add"; fileMode: FileDialog.OpenFiles; nameFilters: ["Android apps (*.apk)"]
        onAccepted: { for (var i = 0; i < selectedFiles.length; i++) { var p = root.urlToPath(selectedFiles[i]); root.queueAdd(p) } }
    }
    // one add job at a time per repo: queue the files
    property var addQueue: []
    function queueAdd(p) { var q = root.addQueue.slice(); q.push(p); root.addQueue = q; root.pumpAdd() }
    function pumpAdd() {
        if (!root.addQueue.length || (root.cur && root.cur.busy) || root.jobRunning) return
        var q = root.addQueue.slice(); var p = q.shift(); root.addQueue = q
        root.act("addApk", [root.sel, p], "Adding " + p.split("/").pop() + "…")
    }
    Timer { interval: 1500; running: root.addQueue.length > 0; repeat: true; onTriggered: root.pumpAdd() }

    Popup {
        id: newDialog; modal: true; anchors.centerIn: parent; width: 440; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        onOpened: { nName.text = ""; nSlug.text = ""; nSlug.edited = false; nKind.kind = "0"; nNode.text = root.st.lastNode || ""; nNodeMsg.text = ""; nName.forceActiveFocus() }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "New repo"; font.bold: true; font.pixelSize: 17 }
            Sub { text: "Name (phones show it)" }
            DField { id: nName; placeholderText: "vpavlin's apps"; onTextEdited: if (!nSlug.edited) nSlug.text = text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 40) }
            Sub { text: "Folder id (letters, digits, - and _)" }
            DField { id: nSlug; property bool edited: false; onTextEdited: edited = true }
            Sub { text: "Storage node that hosts the files (its REST address). Phones download from it, so it must be reachable from where they are."; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                DField { id: nNode; placeholderText: "http://storage.example.org:8080" }
                LogosButton { Layout.preferredWidth: 120; text: "Test"; enabled: nNode.text.length > 8; onClicked: root.testNode(nNode.text.trim(), nNodeMsg) }
            }
            Sub { id: nNodeMsg; Layout.fillWidth: true; visible: text !== "" }
            Sub { text: "Who can read it" }
            ColumnLayout {
                id: nKind; property string kind: "0"; spacing: 2
                Repeater {
                    model: [{ k: "0", t: "Public: anyone with the link" },
                            { k: "1", t: "Private: only people with the invite link (the link carries the read key)" },
                            { k: "2", t: "Members: only devices you add (the link carries no key; you add each phone's member code)" }]
                    RadioButton {
                        checked: nKind.kind === modelData.k; onClicked: nKind.kind = modelData.k; text: modelData.t
                        contentItem: Text { leftPadding: 28; text: parent.text; color: root.cText; font.pixelSize: 13; textFormat: Text.PlainText; wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter }
                        Layout.fillWidth: true
                    }
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: newDialog.close() }
                LogosButton { text: "Create"; enabled: nName.text.length > 0 && nSlug.text.length > 0
                    onClicked: { var slug = nSlug.text; root.act("createRepo", [slug, nName.text, nKind.kind, nNode.text.trim()], "Repo created", function () { root.sel = slug; newDialog.close() }) } }
            }
        }
    }

    Popup {
        id: importDialog; modal: true; anchors.centerIn: parent; width: 520; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Import an F-Droid repo"; font.bold: true; font.pixelSize: 17 }
            Sub { text: "Backpack imports only through the repo's signed index: its certificate must match the fingerprint you give, and every APK must match its SHA-256 and signer."; Layout.fillWidth: true }
            Sub { text: "Repo URL" }
            DField { id: iUrl; placeholderText: "https://example.org/fdroid/repo" }
            Sub { text: "Certificate fingerprint (SHA-256)" }
            DField { id: iFp; placeholderText: "2373710A76AC…" }
            Sub { text: "Versions per app (1-5)" }
            DField { id: iVer; text: "2"; inputMethodHints: Qt.ImhDigitsOnly }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: importDialog.close() }
                LogosButton { text: "Import"; enabled: iUrl.text.length > 8 && iFp.text.length >= 64
                    onClicked: root.act("importFdroid", [root.sel, iUrl.text.trim(), iFp.text.trim(), iVer.text], "Importing… this downloads every APK", function () { importDialog.close() }) }
            }
        }
    }

    Popup {
        id: linkDialog; modal: true; anchors.centerIn: parent; width: 520; padding: 18
        property string link: ""; property bool priv: false; property var qr: null
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        function show() {
            root.core("link", [root.sel], function (raw) {
                var r = root.parse(raw)
                if (!r || r.ok === false) { root.toast(r ? r.error : "No answer", true); return }
                linkDialog.link = r.link; linkDialog.priv = r.private && !(root.cur && root.cur.members); linkDialog.qr = r.qr; qrCanvas.requestPaint(); linkDialog.open()
            })
        }
        onClosed: { link = ""; qr = null }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Invite link"; font.bold: true; font.pixelSize: 17 }
            Sub { visible: linkDialog.priv; color: root.cWarn; Layout.fillWidth: true; text: "🔒 This link carries the READ KEY: whoever has it can read the repo. Share it only with people you mean to, and not in public chats." }
            Sub { visible: !!(root.cur && root.cur.members); Layout.fillWidth: true; text: "👥 Members repo: this link carries no key. It works only on devices you added under Members (after a publish); others see \"not a member\"." }
            Sub { visible: !linkDialog.priv; Layout.fillWidth: true; text: "Open it on the phone (or scan the code in Backpack: + Repo) to add the repo." }
            Canvas {
                id: qrCanvas; Layout.alignment: Qt.AlignHCenter; width: 260; height: 260
                onPaint: {
                    var c = getContext("2d"); c.fillStyle = "#ffffff"; c.fillRect(0, 0, width, height)
                    var q = linkDialog.qr; if (!q) return
                    var n = q.n, m = 4, s = Math.floor(width / (n + 2 * m)), off = Math.floor((width - s * n) / 2)
                    c.fillStyle = "#000000"
                    for (var y = 0; y < n; y++) for (var x = 0; x < n; x++) if (q.cells[y * n + x]) c.fillRect(off + x * s, off + y * s, s, s)
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Copy link"; onClicked: root.copyText(linkDialog.link) }
                LogosButton { text: "Close"; onClicked: linkDialog.close() }
            }
        }
    }

    // ── members (ADR 0013): the devices that get this repo's read key, wrapped for each at publish
    Popup {
        id: membersDialog; modal: true; anchors.centerIn: parent; width: 700; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        onOpened: { mCode.text = "" }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Members"; font.bold: true; font.pixelSize: 17 }
            Sub { Layout.fillWidth: true; text: "Each device here gets the read key, wrapped for it alone, at the next publish. The key changes every hour and whenever you remove a device, so a removed device can't read new versions (what it already has stays its own)." }
            Sub { visible: root.noteMissing(); color: root.cBad; Layout.fillWidth: true; text: root.noteMissingText }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(contentHeight, 280); clip: true; spacing: 6
                model: root.cur && root.cur.memberDevices ? root.cur.memberDevices : []
                delegate: RowLayout {
                    width: ListView.view.width
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.preferredWidth: 1; spacing: 1
                        T { text: modelData.name; font.bold: true }
                        Sub { text: "key " + root.keyShort(modelData.pk) + (modelData.added ? " · added " + modelData.added.slice(0, 10) : "") }
                    }
                    LogosButton { Layout.preferredWidth: 110; text: "Remove"; enabled: !!root.cur && !root.cur.busy
                        onClicked: { var pk = modelData.pk, nm = modelData.name
                            confirmDialog.ask("Remove " + nm + "? After the next publish it can't read new versions.", function () { root.act("removeMember", [root.sel, pk], "Removed: publish to apply it") }) } }
                }
            }
            Sub { visible: !(root.cur && root.cur.memberDevices && root.cur.memberDevices.length); text: "No devices yet." }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
            RowLayout {
                Layout.fillWidth: true
                T { text: "Paid memberships (LEZ)"; font.bold: true; Layout.fillWidth: true }
                Text { text: lezBox.visible ? "hide settings" : "settings"; color: root.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                       MouseArea { anchors.fill: parent; onClicked: lezBox.visible = !lezBox.visible } }
            }
            Sub { Layout.fillWidth: true; visible: !(root.cur && root.cur.lez); text: "Off. With it on, everyone who pays for a membership on the Logos Execution Zone gets their devices added here automatically, until it runs out." }
            Sub { Layout.fillWidth: true; visible: !!(root.cur && root.cur.lez)
                  text: root.cur && root.cur.lez ? (root.cur.lez.members.length + " membership(s) · " + root.lezActive(root.cur.lez.members) + " paid up · last read " + (root.cur.lez.syncedAt ? root.cur.lez.syncedAt.slice(0, 16).replace("T", " ") : "never") + " · read again at every publish") : "" }
            ListView {
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(contentHeight, 140); clip: true; spacing: 4
                model: root.cur && root.cur.lez ? root.cur.lez.members : []
                delegate: Sub {
                    width: ListView.view.width
                    property bool active: modelData.paidThroughMs > Date.now()
                    color: active ? root.cText : root.cFaint
                    text: root.keyShort(modelData.owner) + " · " + modelData.devices.length + " device(s) · " + (active ? "paid through " : "expired ") + new Date(modelData.paidThroughMs).toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm")
                }
            }
            ColumnLayout {
                id: lezBox; visible: false; Layout.fillWidth: true; spacing: 6
                onVisibleChanged: if (visible) { lzWallet.text = root.cur && root.cur.lez ? root.cur.lez.wallet : ""; lzSeq.text = root.cur && root.cur.lez ? root.cur.lez.sequencer : ""; lzProg.text = root.cur && root.cur.lez ? root.cur.lez.program : "" }
                Sub { text: "Wallet folder (reads only; it pays nothing here)" }
                DField { id: lzWallet; placeholderText: "/home/me/.backpack-lez/ops-wallet" }
                Sub { text: "Sequencer" }
                DField { id: lzSeq; placeholderText: "http://sequencer.example:3040/" }
                Sub { text: "backpack-access program id (64 hex)" }
                DField { id: lzProg; placeholderText: "cb23…" }
                RowLayout {
                    Item { Layout.fillWidth: true }
                    LogosButton { text: "Turn off"; visible: !!(root.cur && root.cur.lez); onClicked: root.act("setLez", [root.sel, "{}"], "LEZ memberships off", function () { lezBox.visible = false }) }
                    LogosButton { text: "Save"; enabled: lzWallet.text.length > 0 && lzSeq.text.length > 8 && lzProg.text.trim().length === 64
                        onClicked: root.act("setLez", [root.sel, JSON.stringify({ wallet: lzWallet.text.trim(), sequencer: lzSeq.text.trim(), program: lzProg.text.trim() })], "Saved", function () { lezBox.visible = false }) }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
            Sub { Layout.fillWidth: true; text: "Add a device: in Backpack on the phone, Settings → This phone's member code → Copy link, and paste it here (backpack://member?…). It isn't secret." }
            RowLayout {
                Layout.fillWidth: true
                DField { id: mCode; placeholderText: "backpack://member?pk=…&n=…" }
                LogosButton { Layout.preferredWidth: 110; text: "Add"; enabled: mCode.text.indexOf("backpack://member?") >= 0
                    onClicked: root.act("addMember", [root.sel, mCode.text.trim()], "Added: publish to give it access", function () { mCode.text = "" }) }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Read memberships"; visible: !!(root.cur && root.cur.lez); enabled: !!root.cur && !root.cur.busy; onClicked: root.act("syncLez", [root.sel], "Reading memberships from LEZ…") }
                LogosButton { text: "Publish now"; enabled: !!root.cur && !root.cur.busy; onClicked: { root.act("publish", [root.sel, "0"], "Publishing…"); membersDialog.close() } }
                LogosButton { text: "Close"; onClicked: membersDialog.close() }
            }
        }
    }

    Popup {
        id: settingsDialog; modal: true; anchors.centerIn: parent; width: 560; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        function show() {
            if (!root.cur) return
            sName.text = root.cur.name; sNode.text = root.cur.node; sNodeMsg.text = ""; sGet.text = root.cur.pointersGet.join("\n"); open()
        }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Repo settings"; font.bold: true; font.pixelSize: 17 }
            Sub { text: "Name" }
            DField { id: sName }
            Sub { text: "Storage node (its REST address). Phones must be able to fetch from it: use a public node, not this computer behind a router."; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                DField { id: sNode }
                LogosButton { Layout.preferredWidth: 120; text: "Test"; enabled: sNode.text.length > 8; onClicked: root.testNode(sNode.text.trim(), sNodeMsg) }
            }
            Sub { id: sNodeMsg; Layout.fillWidth: true; visible: text !== "" }
            Sub { text: "Web addresses of the update note phones may read (https, one per line)" }
            TextArea { id: sGet; Layout.fillWidth: true; Layout.preferredHeight: 70; color: root.cText; font.pixelSize: 13; background: Rectangle { radius: 6; color: root.cBg; border.color: root.cLine } }
            Sub { text: "Where publish sends the note is set with the CLI for now (backpack config --pointer-put)."; Layout.fillWidth: true }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: settingsDialog.close() }
                LogosButton { text: "Save"
                    onClicked: {
                        var gets = sGet.text.split("\n").map(function (s) { return s.trim() }).filter(function (s) { return s.length > 0 })
                        root.act("configure", [root.sel, JSON.stringify({ name: sName.text, node: sNode.text.trim(), pointersGet: gets })], "Saved: publish to apply", function () { settingsDialog.close() })
                    } }
            }
        }
    }

    // ── signing keys (one per app; the key is the app's identity)
    Popup {
        id: keysDialog; modal: true; anchors.centerIn: parent; width: 600; padding: 18
        property var list: []
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        function show() { reload(); open() }
        function reload() { root.core("keys", [], function (raw) { var r = root.parse(raw); keysDialog.list = (r && r.ok) ? r.keys : [] }) }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Signing keys"; font.bold: true; font.pixelSize: 17 }
            Sub { Layout.fillWidth: true; text: "Add an UNSIGNED APK and Backpack signs it with its app's key. A key is the app's identity for good: phones only accept updates signed with the same key, so export a backup and keep it safe." }
            Repeater {
                model: keysDialog.list
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 1
                        T { text: modelData.package; font.bold: true }
                        Sub { text: modelData.name + " · " + modelData.type + " · certificate " + modelData.cert.slice(0, 16) + "…" }
                    }
                    LogosButton { Layout.preferredWidth: 120; text: "Export"; onClicked: { exportDialog.pkg = modelData.package; exportDialog.open() } }
                }
            }
            Sub { visible: keysDialog.list.length === 0; text: "No keys yet." }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
            Sub { text: "App package name (from its build, e.g. org.example.app)" }
            DField { id: kPkg; placeholderText: "org.example.app" }
            RowLayout {
                Layout.fillWidth: true
                DField { id: kName; placeholderText: "Name in the certificate (optional)" }
                LogosButton { Layout.preferredWidth: 120; text: "Create key"; enabled: kPkg.text.length > 3
                    onClicked: root.act("createKey", [kPkg.text.trim(), kName.text.trim()], "Key created: export a backup now", function () { keysDialog.reload() }) }
            }
            RowLayout {
                Layout.fillWidth: true
                DField { id: kPass; placeholderText: "Keystore password (to import a .p12)"; echoMode: TextInput.Password }
                LogosButton { Layout.preferredWidth: 120; text: "Import .p12…"; enabled: kPkg.text.length > 3; onClicked: importKeyDialog.open() }
            }
            RowLayout { Item { Layout.fillWidth: true } LogosButton { text: "Close"; onClicked: keysDialog.close() } }
        }
    }
    FileDialog {
        id: importKeyDialog; title: "PKCS#12 keystore to import"; fileMode: FileDialog.OpenFile; nameFilters: ["Keystores (*.p12 *.pfx *.keystore *.jks)", "All files (*)"]
        onAccepted: { root.act("importKey", [kPkg.text.trim(), root.urlToPath(selectedFile), kPass.text], "Key imported", function () { kPass.text = ""; keysDialog.reload() }) }
    }
    Popup {
        id: exportDialog; modal: true; anchors.centerIn: parent; width: 460; padding: 18
        property string pkg: ""
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        onOpened: ePass.text = ""
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Export the " + exportDialog.pkg + " key"; font.bold: true; Layout.fillWidth: true }
            Sub { Layout.fillWidth: true; text: "A password-protected PKCS#12 file (Gradle, apksigner and keytool read it). Keep it somewhere safe and offline." }
            DField { id: ePass; placeholderText: "Password (8+ characters)"; echoMode: TextInput.Password }
            RowLayout { Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: exportDialog.close() }
                LogosButton { text: "Save as…"; enabled: ePass.text.length >= 8; onClicked: exportSave.open() } }
        }
    }
    FileDialog {
        id: exportSave; title: "Save the key backup"; fileMode: FileDialog.SaveFile; nameFilters: ["PKCS#12 (*.p12)"]
        onAccepted: { root.act("exportKey", [exportDialog.pkg, root.urlToPath(selectedFile), ePass.text], "Backup saved", function () { exportDialog.close() }) }
    }

    Popup {
        id: confirmDialog; modal: true; anchors.centerIn: parent; width: 460; padding: 18
        property string msg: ""; property var onYes: null
        function ask(m, f) { msg = m; onYes = f; open() }
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        ColumnLayout {
            width: parent.width; spacing: 12
            T { text: confirmDialog.msg; Layout.fillWidth: true }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: confirmDialog.close() }
                LogosButton { text: "Yes"; onClicked: { confirmDialog.close(); if (confirmDialog.onYes) confirmDialog.onYes() } }
            }
        }
    }
}
