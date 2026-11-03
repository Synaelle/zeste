// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <chrono>
#include <optional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include "citra_qt/zeste_update.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"

namespace Zeste::Update {

namespace {

// The latest published full release: GitHub leaves drafts and pre-releases (vX.Y.Z-preN) out.
constexpr char ReleasesApi[] = "https://api.github.com/repos/Synaelle/zeste/releases/latest";
constexpr char ReleasesPage[] = "https://github.com/Synaelle/zeste/releases";
constexpr std::int64_t CacheSeconds = 6 * 60 * 60;

/// "0.4.1", "v0.4.1" or "0.4.1-3-g3159690" -> {0, 4, 1}; nullopt if it does not start so.
std::optional<std::array<int, 3>> ParseVersion(const QString& text) {
    static const QRegularExpression pattern{QStringLiteral("^v?(\\d+)\\.(\\d+)(?:\\.(\\d+))?")};
    const auto match = pattern.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return std::array<int, 3>{match.captured(1).toInt(), match.captured(2).toInt(),
                              match.captured(3).isEmpty() ? 0 : match.captured(3).toInt()};
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string CachePath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "zeste_update.txt";
}

struct Cache {
    std::int64_t checked = 0;
    QString latest;
    QString url;
};

Cache ReadCache() {
    Cache cache;
    std::string text;
    if (FileUtil::ReadFileToString(true, CachePath(), text) == 0) {
        return cache;
    }
    for (const QString& line : QString::fromStdString(text).split(QLatin1Char('\n'))) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == QStringLiteral("checked")) {
            cache.checked = value.toLongLong();
        } else if (key == QStringLiteral("latest")) {
            cache.latest = value;
        } else if (key == QStringLiteral("url")) {
            cache.url = value;
        }
    }
    return cache;
}

void WriteCache(const Cache& cache) {
    const std::string text = QStringLiteral("checked=%1\nlatest=%2\nurl=%3\n")
                                 .arg(cache.checked)
                                 .arg(cache.latest, cache.url)
                                 .toStdString();
    FileUtil::WriteStringToFile(true, CachePath(), text);
}

Result Compare(const QString& latest_tag, const QString& url) {
    Result result;
    result.latest = latest_tag.startsWith(QLatin1Char('v')) ? latest_tag.mid(1) : latest_tag;
    result.url = url.isEmpty() ? QString::fromLatin1(ReleasesPage) : url;
    const auto current = ParseVersion(QString::fromUtf8(Common::g_zeste_version));
    const auto latest = ParseVersion(latest_tag);
    result.newer = current && latest && *latest > *current;
    return result;
}

} // namespace

void Check(QObject* parent, bool force, std::function<void(const Result&)> done) {
    if (Common::g_zeste_prerelease) {
        // Pre-releases are for testing the Old 3DS side: they never offer updates.
        LOG_INFO(Frontend, "Zeste: no update check in a pre-release build ({})",
                 Common::g_zeste_version);
        if (force) {
            done(Result{.error = QStringLiteral("pre-release builds don't check for updates")});
        }
        return;
    }
    if (!ParseVersion(QString::fromUtf8(Common::g_zeste_version))) {
        LOG_INFO(Frontend, "Zeste: no update check, this build's version is unknown ({})",
                 Common::g_zeste_version);
        if (force) {
            done(Result{.error = QStringLiteral("this build has no version (built without the "
                                                "release tags)")});
        }
        return;
    }

    const Cache cache = ReadCache();
    if (!force && !cache.latest.isEmpty() && Now() - cache.checked < CacheSeconds) {
        done(Compare(cache.latest, cache.url));
        return;
    }

    auto* manager = new QNetworkAccessManager(parent);
    QNetworkRequest request{QUrl(QString::fromLatin1(ReleasesApi))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Zeste/%1").arg(QString::fromUtf8(Common::g_zeste_version)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(10000);
    QNetworkReply* reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, manager, [manager, reply, done] {
        reply->deleteLater();
        manager->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOG_WARNING(Frontend, "Zeste: update check failed: {}",
                        reply->errorString().toStdString());
            done(Result{.error = reply->errorString()});
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = json.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty() || tag.contains(QStringLiteral("-pre"))) {
            done(Result{.error = QStringLiteral("unexpected answer from GitHub")});
            return;
        }
        const QString page = json.value(QStringLiteral("html_url")).toString();
        const Cache fresh{Now(), tag, page};
        WriteCache(fresh);
        const Result result = Compare(fresh.latest, fresh.url);
        LOG_INFO(Frontend, "Zeste: latest release {}, this build {}{}", tag.toStdString(),
                 Common::g_zeste_version, result.newer ? " (update available)" : "");
        done(result);
    });
}

} // namespace Zeste::Update
