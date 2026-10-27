#pragma once
// bp_http.hpp - blocking HTTP for a worker thread (Qt Network with a local event loop). The Storage node's
// REST API (upload/download/debug info) and pointer hosts. Never call it on the module's own thread.
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <functional>
#include <string>

namespace bp {
struct HttpResp { int status = 0; std::string body; std::string error; bool ok() const { return error.empty() && status >= 200 && status < 300; } };

class Http {
public:
    // max: refuse bodies larger than this many bytes (0 = no limit); progress(sent, total) for uploads
    static HttpResp get(const std::string& url, uint64_t max = 0, int timeoutMs = 120000) {
        QNetworkAccessManager nam; QNetworkRequest rq(QUrl(QString::fromStdString(url)));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setRawHeader("Cache-Control", "no-cache");
        QNetworkReply* r = nam.get(rq); HttpResp out; uint64_t got = 0; bool tooBig = false;
        QObject::connect(r, &QNetworkReply::readyRead, [&]() {
            QByteArray b = r->readAll(); got += b.size();
            if (max && got > max) { tooBig = true; r->abort(); return; }
            out.body.append(b.constData(), b.size());
        });
        wait(r, timeoutMs);
        if (tooBig) { out.error = "over " + std::to_string(max) + " bytes"; }
        finish(r, out); return out;
    }
    static HttpResp postFile(const std::string& url, const std::string& path, const std::string& mime, const std::string& filename,
                             std::function<void(int64_t, int64_t)> progress = {}, int timeoutMs = 3600000) {
        QNetworkAccessManager nam; QNetworkRequest rq(QUrl(QString::fromStdString(url)));
        rq.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromStdString(mime));
        rq.setRawHeader("Content-Disposition", QByteArray("attachment; filename=\"") + QByteArray::fromStdString(filename) + "\"");
        QFile f(QString::fromStdString(path)); HttpResp out;
        if (!f.open(QIODevice::ReadOnly)) { out.error = "can't read " + path; return out; }
        rq.setHeader(QNetworkRequest::ContentLengthHeader, f.size());
        QNetworkReply* r = nam.post(rq, &f);
        if (progress) QObject::connect(r, &QNetworkReply::uploadProgress, [&](qint64 s, qint64 t) { progress(s, t); });
        QObject::connect(r, &QNetworkReply::readyRead, [&]() { QByteArray b = r->readAll(); out.body.append(b.constData(), b.size()); });
        wait(r, timeoutMs); finish(r, out); return out;
    }
    static HttpResp put(const std::string& url, const std::string& body, int timeoutMs = 60000) {
        QNetworkAccessManager nam; QNetworkRequest rq(QUrl(QString::fromStdString(url)));
        rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* r = nam.put(rq, QByteArray::fromStdString(body)); HttpResp out;
        QObject::connect(r, &QNetworkReply::readyRead, [&]() { QByteArray b = r->readAll(); out.body.append(b.constData(), b.size()); });
        wait(r, timeoutMs); finish(r, out); return out;
    }
private:
    static void wait(QNetworkReply* r, int timeoutMs) {
        if (r->isFinished()) return;
        QEventLoop loop; QTimer t; t.setSingleShot(true);
        QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&t, &QTimer::timeout, [&]() { r->abort(); });
        t.start(timeoutMs); loop.exec();
    }
    static void finish(QNetworkReply* r, HttpResp& out) {
        QByteArray rest = r->readAll(); out.body.append(rest.constData(), rest.size());
        out.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (out.error.empty() && r->error() != QNetworkReply::NoError && !(out.status >= 400)) out.error = r->errorString().toStdString();
        r->deleteLater();
    }
};
}  // namespace bp
