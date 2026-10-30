// Backpack - a Basecamp view over backpack_core (ADR 0011): repos you follow (and pay for) and repos you publish.
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
    // two sides: "subs" = repos you follow (and pay for), "repos" = repos you publish
    property string mode: ""
    property string selSub: ""
    readonly property var curSub: { var l = root.st.subs || []; for (var i = 0; i < l.length; i++) if (l[i].key === root.selSub) return l[i]; return null }
    readonly property var subJobs: { var o = []; for (var i = 0; i < st.jobs.length; i++) if (st.jobs[i].repo === "sub_" + root.selSub) o.push(st.jobs[i]); return o }
    readonly property var curJobs: { var o = []; for (var i = 0; i < st.jobs.length; i++) if (st.jobs[i].repo === root.sel) o.push(st.jobs[i]); return o }

    function refresh() {
        if (root.busy) { root.again = true; return }
        root.busy = true
        root.core("snapshot", [], function (raw) {
            var s = root.parse(raw)
            if (s && s.ok !== false) {
                root.st = s; root.coreSeen = true; root.missed = 0
                if (!root.sel && s.repos.length) root.sel = s.repos[0].slug
                var subs = s.subs || []
                if (root.mode === "") root.mode = (s.repos.length && !subs.length) ? "repos" : "subs"
                if (!root.selSub && subs.length) root.selSub = subs[0].key
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
    // A members repo hands out its key only through the update note (the link carries no key). The update note goes out over Delivery (loam_core); web addresses are an optional extra
    readonly property var curListed: { var l = root.st.repos || []; for (var i = 0; i < l.length; i++) if (l[i].slug === root.sel) return l[i]; return null }
    function noteLine() {
        if (!root.cur) return ""
        var d = root.st.delivery || "off", sent = root.curListed ? root.curListed.noteSent : null
        var t = "Update note: over Delivery"
        if (sent && String(sent).indexOf("failed") === 0) t += " (" + sent + ")"
        else if (sent) t += " (sent " + root.when(Date.parse(sent)) + ")"
        else if (d === "Connected") t += (root.cur.seq ? " (sent at the next publish)" : "")
        else if (d.indexOf("error") === 0) t += " (" + d + ")"
        else if (root.cur.seq) t += " (the node is " + d + ")"
        if (root.cur.pointersGet.length) t += " · also on the web: " + root.cur.pointersGet.join(", ")
        if (root.cur.pointersPut.length) t += " · sent via " + root.cur.pointersPut.join(", ")
        return t
    }
    function noteBad() { var d = root.st.delivery || ""; return d.indexOf("error") === 0 && !(root.cur && root.cur.pointersPut.length) }
    // devices that can read a members repo: added by hand + those of paid-up memberships
    function deviceCount(r) {
        if (!r) return 0
        var n = r.memberDevices ? r.memberDevices.length : 0
        if (r.lez && r.lez.members) for (var i = 0; i < r.lez.members.length; i++) if (r.lez.members[i].paidThroughMs > Date.now()) n += r.lez.members[i].devices.length
        return n
    }
    function lezActive(list) { var n = 0; for (var i = 0; i < list.length; i++) if (list[i].paidThroughMs > Date.now()) n++; return n }
    // the membership program this build knows (backpack_access, one config per repo; test chain)
    readonly property string lezProgram: "01f0369ec40456b4be54ebb09ac8af8cee704af51fc28be08b7fbcae218ea567"
    readonly property string lezSequencer: "https://testnet.lez.logos.co/"
    // a billing period in words
    function period(ms) {
        if (!ms) return "period"
        var h = ms / 3600000
        if (h < 1) return Math.round(ms / 60000) + " min"
        if (h < 24) return h === 1 ? "hour" : h + " hours"
        var d = h / 24
        if (d >= 28 && d <= 31) return "month"
        return d === 1 ? "day" : (d % 7 === 0 ? (d / 7 === 1 ? "week" : d / 7 + " weeks") : d + " days")
    }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm") : "" }
    function terms(x) { var t = x && (x.terms || (x.access && x.access.price !== undefined ? { price: x.access.price, periodMs: x.access.periodMs } : null)); return t }
    function deviceName(x, pk) { return (x && x.deviceNames && x.deviceNames[pk]) ? x.deviceNames[pk] : "a phone" }
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

    component ActivityPanel: Rectangle {
        property var jobs: []
        Layout.preferredWidth: 380; Layout.fillHeight: true; radius: 10; color: root.cSurface
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 12; spacing: 6
            RowLayout { T { text: "Activity"; font.bold: true; Layout.fillWidth: true } Text { text: "clear"; color: root.cFaint; font.pixelSize: 12; MouseArea { anchors.fill: parent; onClicked: root.act("clearJobs", [], "") } } }
            Sub { visible: jobs.length === 0; text: "Publishing, imports and checks show up here." }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8
                model: jobs
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
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    visible: root.st.repos.length > 0 || root.mode === "repos"
                    Repeater {
                        model: [{ m: "subs", t: "Following" }, { m: "repos", t: "My repos" }]
                        Rectangle {
                            Layout.fillWidth: true; height: 30; radius: 6
                            color: root.mode === modelData.m ? root.cRaised : "transparent"; border.color: root.mode === modelData.m ? root.cAccent : root.cLine
                            T { anchors.centerIn: parent; text: modelData.t; font.pixelSize: 13; font.bold: root.mode === modelData.m }
                            MouseArea { anchors.fill: parent; onClicked: root.mode = modelData.m }
                        }
                    }
                }
                Sub { text: root.mode === "repos" ? "Repos you publish on Logos Storage" : "Repos you follow: join paid ones and add your phones"; Layout.fillWidth: true }
                Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
                // followed repos
                ListView {
                    visible: root.mode === "subs"
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 4
                    model: root.st.subs || []
                    delegate: Rectangle {
                        width: ListView.view.width; height: 58; radius: 8
                        color: modelData.key === root.selSub ? root.cRaised : (sma.containsMouse ? Qt.darker(root.cRaised, 1.2) : "transparent")
                        MouseArea { id: sma; anchors.fill: parent; hoverEnabled: true; onClicked: root.selSub = modelData.key }
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 2
                            RowLayout {
                                T { text: (modelData.access ? "💳 " : modelData.members ? "👥 " : "") + modelData.name; font.bold: true; elide: Text.ElideRight; wrapMode: Text.NoWrap; Layout.fillWidth: true }
                                Rectangle { visible: !!modelData.busy; width: 8; height: 8; radius: 4; color: root.cWarn }
                            }
                            Sub {
                                Layout.fillWidth: true; elide: Text.ElideRight; wrapMode: Text.NoWrap
                                text: modelData.error ? modelData.error
                                    : !modelData.access ? (modelData.members ? "members · added by the publisher" : "free")
                                    : (modelData.membership && modelData.membership.found ? (modelData.membership.paidThroughMs > Date.now() ? "member until " + root.when(modelData.membership.paidThroughMs) : "expired " + root.when(modelData.membership.paidThroughMs)) : "paid · not a member")
                            }
                        }
                    }
                }
                Sub { visible: root.mode === "subs" && !(root.st.subs || []).length; Layout.fillWidth: true; text: "Nothing yet: follow a repo with the link its publisher shared." }
                LogosButton { visible: root.mode === "subs"; text: "+ Follow a repo"; Layout.fillWidth: true; onClicked: followDialog.open() }
                LogosButton { visible: root.mode === "subs"; text: "Your LEZ account"; Layout.fillWidth: true; onClicked: accountDialog.show() }
                LogosButton { visible: root.mode === "subs" && root.st.repos.length === 0; text: "Publish your own repo"; Layout.fillWidth: true; onClicked: { root.mode = "repos"; newDialog.open() } }
                // repos you publish
                ListView {
                    visible: root.mode === "repos"
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
                LogosButton { visible: root.mode === "repos"; text: "+ New repo"; Layout.fillWidth: true; onClicked: newDialog.open() }
                LogosButton { visible: root.mode === "repos"; text: "Signing keys"; Layout.fillWidth: true; onClicked: keysDialog.show() }
                Sub { text: root.coreSeen ? ("Repos in " + root.st.dataDir + " · core " + root.st.version) : (root.missed > 2 ? "Can't reach backpack_core" : "Connecting…"); Layout.fillWidth: true; color: !root.coreSeen && root.missed > 2 ? root.cBad : root.cFaint }
            }
        }

        // ── repo page (a repo you publish)
        Item {
            visible: root.mode === "repos"
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
                            Badge { visible: !!(root.cur && root.cur.members); text: "👥 members · " + root.deviceCount(root.cur) + " devices"; tone: root.cAccent }
                            Badge { visible: !!(root.cur && !root.cur.private); text: "public"; tone: root.cOk }
                            Badge { visible: !!(root.cur && root.cur.busy); text: "working…"; tone: root.cAccent }
                        }
                        Sub { text: root.cur ? ("#" + root.cur.seq + " · publisher key " + root.keyShort(root.cur.key) + " · Storage node " + (root.cur.node || "not set")) : ""; Layout.fillWidth: true }
                        Sub { visible: !!(root.cur && root.cur.nodeWarning); color: root.cWarn; Layout.fillWidth: true
                              text: root.cur ? (root.cur.node ? "The Storage node is " + root.cur.nodeWarning + ": phones outside that network can't fetch this repo." : "Set a Storage node in Settings before publishing.") : "" }
                        Sub { text: root.noteLine(); color: root.noteBad() ? root.cBad : root.cSub; Layout.fillWidth: true }
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
                    ActivityPanel { jobs: root.curJobs }
                }
            }
        }

        // ── a repo you follow
        Item {
            visible: root.mode === "subs"
            Layout.fillWidth: true; Layout.fillHeight: true
            T { visible: !root.curSub; anchors.centerIn: parent; color: root.cSub; width: 460; horizontalAlignment: Text.AlignHCenter
                text: (root.st.subs || []).length ? "Pick a repo" : "Follow a repo with the link its publisher shared (backpack://repo?…). A paid repo is joined here: you pay with your LEZ wallet and add your phones; they get in at the publisher's next update." }
            ColumnLayout {
                id: subPage
                visible: !!root.curSub
                anchors.fill: parent; anchors.margins: 18; spacing: 12
                readonly property var sx: root.curSub
                readonly property var sm: sx && sx.membership ? sx.membership : null
                readonly property bool sPaid: !!(sx && sx.access)
                readonly property bool sActive: !!(sm && sm.found && sm.paidThroughMs > Date.now())
                RowLayout {
                    T { text: subPage.sx ? subPage.sx.name : ""; font.pixelSize: 22; font.bold: true }
                    Badge { visible: subPage.sPaid; text: "💳 paid"; tone: root.cAccent }
                    Badge { visible: !subPage.sPaid && !!(subPage.sx && subPage.sx.members); text: "👥 members"; tone: root.cAccent }
                    Badge { visible: !subPage.sPaid && !(subPage.sx && subPage.sx.members); text: "free"; tone: root.cOk }
                    Badge { visible: !!(subPage.sx && subPage.sx.busy); text: "working…"; tone: root.cAccent }
                }
                Sub { Layout.fillWidth: true; text: subPage.sx ? ("publisher key " + root.keyShort(subPage.sx.key) + (subPage.sx.seq !== undefined ? " · update #" + subPage.sx.seq : "") + (subPage.sx.checkedAt ? " · checked " + subPage.sx.checkedAt.slice(0, 16).replace("T", " ") : "")) : "" }
                Sub { visible: !!(subPage.sx && subPage.sx.error); color: root.cBad; Layout.fillWidth: true; text: subPage.sx && subPage.sx.error ? subPage.sx.error : "" }
                // ADR 0015: this Basecamp holds the repo's files and serves them to others
                Sub {
                    Layout.fillWidth: true
                    property var m: root.st.mirror && root.st.mirror.repos && subPage.sx ? root.st.mirror.repos[subPage.sx.key] : null
                    property bool reachable: !!(root.st.mirror && root.st.mirror.addrs && root.st.mirror.addrs.length)
                    color: subPage.sx && subPage.sx.mirrorOff ? root.cSub : (reachable ? root.cOk : root.cWarn)
                    text: !subPage.sx ? "" : subPage.sx.mirrorOff ? "Not hosting this repo's files for others."
                        : !m ? "Hosting for others: waiting for the repo's file list…"
                        : "Hosting for others: " + (m.held || 0) + " of " + m.files + " files" + (m.bytes ? ", " + root.mb(m.bytes) : "")
                          + (reachable ? " · your Storage node is reachable at " + root.st.mirror.addrs.length + " address(es)" : " · your Storage node announces no address, so others can't fetch from it")
                }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    LogosButton { text: "Check now"; enabled: !!subPage.sx && !subPage.sx.busy; onClicked: root.act("refreshSub", [root.selSub], "Checking…") }
                    LogosButton { text: subPage.sx && subPage.sx.mirrorOff ? "Host for others" : "Stop hosting"; enabled: !!subPage.sx
                        onClicked: root.act("setMirror", [root.selSub, subPage.sx.mirrorOff ? "1" : "0"], subPage.sx.mirrorOff ? "Hosting this repo's files" : "Stopped hosting: its files are removed") }
                    LogosButton { text: "Copy link"; enabled: !!subPage.sx; onClicked: root.copyText(subPage.sx.link) }
                    LogosButton { text: "Unfollow"; enabled: !!subPage.sx && !subPage.sx.busy
                        onClicked: confirmDialog.ask("Stop following " + subPage.sx.name + "? A membership you paid for stays yours on the chain; follow the link again to manage it.", function () { root.act("unfollow", [root.selSub], "Unfollowed", function () { root.selSub = "" }) }) }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 10; color: root.cSurface
                        Flickable {
                            anchors.fill: parent; anchors.margins: 14; clip: true; contentHeight: subCol.implicitHeight
                            ColumnLayout {
                                id: subCol; width: parent.width; spacing: 10
                                // not paid: how to get in
                                Sub { visible: !subPage.sPaid; Layout.fillWidth: true; font.pixelSize: 13
                                    text: subPage.sx && subPage.sx.members
                                        ? "The publisher adds this repo's members by hand: send them your phone's member code (Backpack on the phone → Settings → This phone's member code → Copy link). Then open this repo's link on the phone."
                                        : "Anyone with the link can read this repo: open it in Backpack on your phone (Copy link, or scan its QR from the publisher)." }
                                // paid: terms + membership
                                T { visible: subPage.sPaid; text: "Membership"; font.bold: true; font.pixelSize: 16 }
                                Sub { visible: subPage.sPaid; Layout.fillWidth: true; font.pixelSize: 13
                                    text: { var t = root.terms(subPage.sx); return t ? ("Costs " + t.price + " per " + root.period(t.periodMs) + ", paid from your LEZ wallet. Your phones read the repo while it's paid; when it runs out they miss the updates after that, and keep what they have.") : "" } }
                                Sub { visible: subPage.sPaid && !(root.st.me && root.st.me.account); color: root.cWarn; Layout.fillWidth: true; text: "Set your LEZ wallet and account first (Your LEZ account), then Check now." }
                                Sub { visible: subPage.sPaid && !!(subPage.sx && subPage.sx.membershipError); color: root.cBad; Layout.fillWidth: true; text: subPage.sx && subPage.sx.membershipError ? "Couldn't read your membership: " + subPage.sx.membershipError : "" }
                                T { visible: subPage.sPaid && !!subPage.sm; font.pixelSize: 15
                                    color: subPage.sActive ? root.cOk : root.cWarn
                                    text: !subPage.sm ? "" : !subPage.sm.found ? "You're not a member yet." : subPage.sActive ? "Member until " + root.when(subPage.sm.paidThroughMs) : "Expired " + root.when(subPage.sm.paidThroughMs) + ": renew to get new updates again." }
                                LogosButton { visible: subPage.sPaid && !!(root.st.me && root.st.me.account); enabled: !!subPage.sx && !subPage.sx.busy
                                    text: subPage.sm && subPage.sm.found ? "Renew" : "Join"; onClicked: payDialog.show(subPage.sm && subPage.sm.found) }
                                // devices
                                T { visible: subPage.sPaid && !!(subPage.sm && subPage.sm.found); text: "Your devices"; font.bold: true; font.pixelSize: 16; Layout.topMargin: 8 }
                                Sub { visible: subPage.sPaid && !!(subPage.sm && subPage.sm.found); Layout.fillWidth: true
                                    text: "The phones that read this repo with your membership. A new one gets in at the publisher's next update (it publishes by itself when someone joins, usually within minutes); one you take off is shut out the same way." }
                                Repeater {
                                    model: subPage.sPaid && subPage.sm && subPage.sm.found ? subPage.sm.devices : []
                                    RowLayout {
                                        Layout.fillWidth: true
                                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                                            T { text: root.deviceName(subPage.sx, modelData); font.bold: true }
                                            Sub { text: "key " + root.keyShort(modelData) } }
                                        LogosButton { Layout.preferredWidth: 110; text: "Take off"; enabled: !!subPage.sx && !subPage.sx.busy
                                            onClicked: { var pk = modelData; confirmDialog.ask("Take " + root.deviceName(subPage.sx, pk) + " off your membership? It can't read updates after the publisher's next one.", function () { root.act("removeMyDevice", [root.selSub, pk], "Taking it off…") }) } }
                                    }
                                }
                                Sub { visible: subPage.sPaid && !!(subPage.sm && subPage.sm.found) && subPage.sm.devices.length === 0; text: "No devices: add a phone below." }
                                RowLayout {
                                    visible: subPage.sPaid && !!(subPage.sm && subPage.sm.found); Layout.fillWidth: true
                                    DField { id: addPhone; placeholderText: "a phone's member code: backpack://member?pk=…" }
                                    LogosButton { Layout.preferredWidth: 110; text: "Add"; enabled: addPhone.text.indexOf("backpack://member?") === 0 && !!subPage.sx && !subPage.sx.busy
                                        onClicked: root.act("addMyDevice", [root.selSub, addPhone.text.trim()], "Adding the phone…", function () { addPhone.text = "" }) }
                                }
                                Sub { visible: subPage.sPaid; Layout.fillWidth: true; color: root.cFaint
                                    text: "A phone's member code: Backpack on the phone → Settings → This phone's member code → Copy link. On the phone, open this repo's link too (Copy link above)." }
                            }
                        }
                    }
                    ActivityPanel { jobs: root.subJobs }
                }
            }
        }
    }

    // ── follow a repo
    Popup {
        id: followDialog; modal: true; anchors.centerIn: parent; width: 600; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        onOpened: fLink.text = ""
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Follow a repo"; font.bold: true; font.pixelSize: 17 }
            Sub { Layout.fillWidth: true; text: "Paste the repo's link (backpack://repo?…) as its publisher shared it." }
            DField { id: fLink; placeholderText: "backpack://repo?…" }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: followDialog.close() }
                LogosButton { text: "Follow"; enabled: fLink.text.trim().indexOf("backpack://repo?") === 0
                    onClicked: root.act("follow", [fLink.text.trim()], "Reading the repo's update note…", function () { followDialog.close() }) }
            }
        }
    }

    // ── the user's LEZ wallet and paying account
    Popup {
        id: accountDialog; modal: true; anchors.centerIn: parent; width: 620; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        function show() { aWallet.text = root.st.me && root.st.me.wallet ? root.st.me.wallet : ""; aAcct.text = root.st.me && root.st.me.account ? root.st.me.account : ""; open() }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: "Your LEZ account"; font.bold: true; font.pixelSize: 17 }
            Sub { Layout.fillWidth: true; text: "Memberships are paid from this account. Backpack signs with the wallet in this folder; it never shows or copies its keys. The wallet's own settings decide which sequencer it talks to." }
            Sub { text: "Wallet folder" }
            DField { id: aWallet; placeholderText: "/home/you/.lez-wallet" }
            Sub { text: "Account id (as the wallet shows it)" }
            DField { id: aAcct; placeholderText: "base58, as the LEZ Wallet app shows it" }
            Sub { Layout.fillWidth: true; color: root.cWarn; text: "Memberships are public on the chain: anyone can see that this account pays for a repo and how many devices it has." }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: accountDialog.close() }
                LogosButton { text: "Save"; enabled: aWallet.text.trim().length > 0 && aAcct.text.trim().length > 30
                    onClicked: root.act("setLezAccount", [JSON.stringify({ wallet: aWallet.text.trim(), account: aAcct.text.trim() })], "Saved", function () { accountDialog.close(); if (root.selSub) root.act("refreshSub", [root.selSub], "") }) }
            }
        }
    }

    // ── join or renew a paid repo
    Popup {
        id: payDialog; modal: true; anchors.centerIn: parent; width: 600; padding: 18
        background: Rectangle { radius: 10; color: root.cSurface; border.color: root.cLine }
        property bool renewing: false
        readonly property var t: root.terms(root.curSub)
        function show(renew) { renewing = !!renew; pPeriods.value = 1; pPhone.text = ""; open() }
        ColumnLayout {
            width: parent.width; spacing: 10
            T { text: (payDialog.renewing ? "Renew " : "Join ") + (root.curSub ? root.curSub.name : ""); font.bold: true; font.pixelSize: 17 }
            Sub { Layout.fillWidth: true; text: payDialog.t ? (payDialog.t.price + " per " + root.period(payDialog.t.periodMs) + (payDialog.renewing ? ". Renewing adds to what's left (or starts now if it ran out)." : ".")) : "" }
            RowLayout {
                Sub { text: "How many " + (payDialog.t ? root.period(payDialog.t.periodMs) : "period") + "s" }
                SpinBox { id: pPeriods; from: 1; to: 1200; value: 1; editable: true }
            }
            T { text: payDialog.t ? ("Total: " + (Number(payDialog.t.price) * pPeriods.value) + " (plus the network fee)") : ""; font.bold: true }
            Sub { visible: !payDialog.renewing; Layout.fillWidth: true; text: "Your first phone: its member code (Backpack on the phone → Settings → This phone's member code → Copy link). Add more phones later." }
            DField { id: pPhone; visible: !payDialog.renewing; placeholderText: "backpack://member?pk=…" }
            RowLayout {
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: payDialog.close() }
                LogosButton { text: "Pay " + (payDialog.t ? Number(payDialog.t.price) * pPeriods.value : "")
                    enabled: !!payDialog.t && (payDialog.renewing || pPhone.text.trim().indexOf("backpack://member?") === 0)
                    onClicked: root.act("payMembership", [root.selSub, String(pPeriods.value), payDialog.renewing ? "" : pPhone.text.trim(), String(payDialog.t.price)], "Paying… (a few seconds)", function () { payDialog.close() }) }
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
                            { k: "2", t: "Members: only member devices: ones you add by their member code, or (with paid memberships on) people who pay. The link carries no key" }]
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
            Sub { Layout.fillWidth: true; text: "Each device here gets the read key, wrapped for it alone, at the next publish. The key changes at every publish and only the devices that are members then get it, so a removed or lapsed one can't read new versions (what it already has stays its own)." }
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
                  text: root.cur && root.cur.lez ? ((root.cur.lez.terms && root.cur.lez.terms.price ? root.cur.lez.terms.price + " per " + root.period(root.cur.lez.terms.periodMs) + " · " : "") + root.cur.lez.members.length + " membership(s) · " + root.lezActive(root.cur.lez.members) + " paid up · last read " + (root.cur.lez.syncedAt ? root.cur.lez.syncedAt.slice(0, 16).replace("T", " ") : "never") + " · read again at every publish") : "" }
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
                readonly property var periods: [{ t: "month (30 days)", ms: 2592000000 }, { t: "week", ms: 604800000 }, { t: "day", ms: 86400000 }, { t: "hour (testing)", ms: 3600000 }]
                onVisibleChanged: if (visible) {
                    var z = root.cur && root.cur.lez ? root.cur.lez : null
                    lzWallet.text = z ? z.wallet : ""; lzSeq.text = z && z.sequencer ? z.sequencer : root.lezSequencer; lzProg.text = z && z.program ? z.program : root.lezProgram
                    lzAdmin.text = z && z.admin ? z.admin : ""; lzPrice.text = z && z.price ? z.price : ""
                    var pi = 0; for (var k = 0; k < periods.length; k++) if (z && z.periodMs === periods[k].ms) pi = k; lzPeriod.currentIndex = pi
                }
                Sub { Layout.fillWidth: true; text: "Saving creates this repo's membership terms on the chain (or changes them: a new price applies to later payments). People join from Backpack on Basecamp; their phones get in at the next publish, which happens by itself." }
                Sub { text: "Your LEZ wallet folder (signs the terms; pays only the network fee)" }
                DField { id: lzWallet; placeholderText: "/home/you/.lez-wallet" }
                Sub { text: "Your account in it: it receives the payments and alone can change the terms" }
                DField { id: lzAdmin; placeholderText: "base58, as the LEZ Wallet app shows it" }
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout { Layout.fillWidth: true; Sub { text: "Price per period" } DField { id: lzPrice; placeholderText: "e.g. 100"; validator: RegularExpressionValidator { regularExpression: /[0-9]{1,30}/ } } }
                    ColumnLayout { Sub { text: "Period" } ComboBox { id: lzPeriod; Layout.preferredWidth: 200; model: lezBox.periods.map(function (p) { return p.t }) } }
                }
                Sub { text: "Sequencer" }
                DField { id: lzSeq; placeholderText: root.lezSequencer }
                Sub { text: "Membership program id (64 hex)" }
                DField { id: lzProg }
                RowLayout {
                    Item { Layout.fillWidth: true }
                    LogosButton { text: "Turn off"; visible: !!(root.cur && root.cur.lez); onClicked: root.act("setLez", [root.sel, "{}"], "LEZ memberships off", function () { lezBox.visible = false }) }
                    LogosButton { text: "Save"; enabled: lzWallet.text.length > 0 && lzSeq.text.length > 8 && lzProg.text.trim().length === 64 && lzAdmin.text.trim().length > 30 && lzPrice.text.length > 0
                        onClicked: root.act("setLez", [root.sel, JSON.stringify({ wallet: lzWallet.text.trim(), sequencer: lzSeq.text.trim(), program: lzProg.text.trim(), admin: lzAdmin.text.trim(), price: lzPrice.text, periodMs: lezBox.periods[lzPeriod.currentIndex].ms })], "Saving the terms on the chain…", function () { lezBox.visible = false }) }
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
            Sub { text: "The update note goes out over Logos Delivery at every publish, and again twice a day while Basecamp runs. Optionally, web addresses phones may also read it from (https, one per line):"; Layout.fillWidth: true }
            TextArea { id: sGet; Layout.fillWidth: true; Layout.preferredHeight: 70; color: root.cText; font.pixelSize: 13; background: Rectangle { radius: 6; color: root.cBg; border.color: root.cLine } }
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
