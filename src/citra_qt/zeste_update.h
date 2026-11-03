// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste update check: compares this build's version (Common::g_zeste_version, from the latest
// release tag) with the latest release of github.com/Synaelle/zeste. Headless test loops start
// Zeste many times and the server limits unauthenticated API calls, so the answer is cached in
// the user folder (zeste_update.txt) and GitHub is asked at most every 6 hours.

#pragma once

#include <functional>
#include <QString>

class QObject;

namespace Zeste::Update {

struct Result {
    bool newer = false; ///< `latest` is newer than this build
    QString latest;     ///< latest release ("0.4.2"), empty when unknown
    QString url;        ///< its release page
    QString error;      ///< why the check failed, empty when it worked
};

/// Asynchronous: `done` runs on the GUI thread. `force` skips the cache (Help > Check for
/// Updates). Nothing is checked when this build's version is unknown (built without git tags).
void Check(QObject* parent, bool force, std::function<void(const Result&)> done);

} // namespace Zeste::Update
