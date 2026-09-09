/**
 * @file UserProfileService.cpp
 * @brief Lazy, batched Mattermost user profile loading.
 */

#include "UserProfileService.h"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendTeamMember.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {

namespace {

constexpr int MaxProfilesPerRequest = 100;
constexpr int MaxStatusesPerRequest = 200;
constexpr int MembersPerPage = 200;
constexpr int MissingProfilesWaitMs = 100;
constexpr qint64 ProfileReconnectClockSkewAllowanceMs = 5 * 60 * 1000;

QStringList uniqueNonEmptyIds(const QStringList& userIds)
{
    QSet<QString> unique;
    for (const QString& userId : userIds) {
        if (!userId.isEmpty()) {
            unique.insert(userId);
        }
    }
    return unique.values();
}

} // namespace

UserProfileService& UserProfileService::instance(Backend& backend)
{
    // UserProfileService is a QObject child of Backend and is destroyed together
    // with it (e.g. during application shutdown). Use a self-nulling QPointer so
    // a stale entry never returns a dangling reference for a destroyed Backend;
    // the next call transparently constructs a fresh instance.
    static QHash<Backend*, QPointer<UserProfileService>> instances;
    QPointer<UserProfileService>& service = instances[&backend];
    if (!service) {
        service = new UserProfileService(backend);
    }
    return *service;
}

UserProfileService::UserProfileService(Backend& backend)
    : QObject(&backend)
    , backend(backend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);

    connect(&backend, &Backend::onWebSocketDisconnect, this, [this] {
        if (disconnectedAt == 0) {
            disconnectedAt = QDateTime::currentMSecsSinceEpoch();
        }
    });
    connect(&backend, &Backend::onWebSocketConnect, this, [this] {
        if (disconnectedAt == 0) {
            return;
        }

        const qint64 since = disconnectedAt;
        disconnectedAt = 0;
        refreshKnownUsersSince(since);
    });
}

void UserProfileService::clear()
{
    httpConnector.reset();
    queuedUserIds.clear();
    inFlightUserIds.clear();
    inFlightAvatarKeys.clear();
    waiters.clear();
    disconnectedAt = 0;
    flushScheduled = false;

    // The legacy startup path happened to load the login user's avatar at the
    // end of retrieveAllUsers(). With the global directory preload gone, make
    // the two pieces of login-user state explicit and defer them one event-loop
    // turn so MainWindow has installed its avatar/status signal handlers first.
    QTimer::singleShot(0, this, [this] {
        const QString loginUserId = backend.getLoginUser().id;
        if (loginUserId.isEmpty()) {
            return;
        }

        ensureStatuses(QStringList {loginUserId});
        if (const BackendUser* loginUser = backend.getStorage().getUserById(loginUserId)) {
            ensureAvatar(*loginUser);
        }
    });
}

void UserProfileService::ensureUser(const QString& userId,
                                    std::function<void(const BackendUser*)> callback)
{
    if (userId.isEmpty()) {
        if (callback) {
            callback(nullptr);
        }
        return;
    }

    if (BackendUser* user = backend.getStorage().getUserById(userId)) {
        if (callback) {
            callback(user);
        }
        return;
    }

    if (callback) {
        waiters[userId].push_back(std::move(callback));
    }

    if (!queuedUserIds.contains(userId) && !inFlightUserIds.contains(userId)) {
        queuedUserIds.insert(userId);
        scheduleFlush();
    }
}

void UserProfileService::ensureUsers(const QStringList& userIds,
                                     std::function<void()> callback)
{
    const QStringList ids = uniqueNonEmptyIds(userIds);
    if (ids.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    const auto remaining = std::make_shared<int>(static_cast<int>(ids.size()));
    const auto completed = std::make_shared<bool>(false);

    for (const QString& userId : ids) {
        ensureUser(userId, [remaining, completed, callback](const BackendUser*) {
            --*remaining;
            if (*remaining == 0 && !*completed) {
                *completed = true;
                if (callback) {
                    callback();
                }
            }
        });
    }
}

void UserProfileService::ensureAvatar(const BackendUser& user)
{
    if (user.id.isEmpty()) {
        return;
    }

    BackendUser* storedUser = backend.getStorage().getUserById(user.id);
    if (!storedUser) {
        return;
    }

    const uint64_t pictureVersion = storedUser->last_picture_update;
    if (!storedUser->avatar.isNull()
        && storedUser->avatar_picture_update == pictureVersion) {
        return;
    }

    const QString pictureVersionString = QString::number(
        static_cast<qulonglong>(pictureVersion));
    const QString requestKey = storedUser->id + QLatin1Char(':') + pictureVersionString;
    if (inFlightAvatarKeys.contains(requestKey)) {
        return;
    }
    inFlightAvatarKeys.insert(requestKey);

    // Mattermost itself uses last_picture_update both as the profile image
    // ETag and as a cache-busting URL parameter in the web client. Keep the
    // same URL identity so an unchanged avatar can be served directly by
    // QNetworkDiskCache while a changed avatar necessarily gets a new key.
    NetworkRequest request(
        QStringLiteral("users/") + storedUser->id + QStringLiteral("/image?_=")
            + pictureVersionString,
        true);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    const QString userId = storedUser->id;
    httpConnector.get(request, HttpResponseCallback(
        [this, requestKey, userId, pictureVersion](QVariant, QByteArray data) {
            inFlightAvatarKeys.remove(requestKey);

            BackendUser* currentUser = backend.getStorage().getUserById(userId);
            if (!currentUser || currentUser->last_picture_update != pictureVersion) {
                return;
            }

            QPixmap pixmap;
            if (!pixmap.loadFromData(data)) {
                return;
            }

            currentUser->avatar = pixmap.scaled(
                48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            currentUser->avatar_picture_update = pictureVersion;
            emit currentUser->onAvatarChanged();
        }));
}

void UserProfileService::ensureStatuses(const QStringList& userIds,
                                        std::function<void()> callback)
{
    QStringList ids;
    for (const QString& userId : uniqueNonEmptyIds(userIds)) {
        if (backend.getStorage().getUserById(userId)) {
            ids.push_back(userId);
        }
    }

    if (ids.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    const auto pending = std::make_shared<QStringList>(std::move(ids));
    const auto fetchNext = std::make_shared<std::function<void()>>();

    *fetchNext = [this, callback, pending, fetchNext] {
        if (pending->isEmpty()) {
            if (callback) {
                callback();
            }
            return;
        }

        QJsonArray payload;
        QStringList batch;
        const int batchSize = std::min(
            MaxStatusesPerRequest, static_cast<int>(pending->size()));
        batch.reserve(batchSize);
        for (int i = 0; i < batchSize; ++i) {
            const QString userId = pending->takeFirst();
            batch.push_back(userId);
            payload.push_back(userId);
        }

        NetworkRequest request(QStringLiteral("users/status/ids"));
        httpConnector.post(request, QByteArrayCreator(payload),
                           HttpResponseCallback([this, fetchNext](const QJsonDocument& doc) {
            for (const auto& value : doc.array()) {
                const QJsonObject object = value.toObject();
                BackendUser* user = backend.getStorage().getUserById(
                    object.value(QStringLiteral("user_id")).toString());
                if (!user) {
                    continue;
                }

                const QString status = object.value(QStringLiteral("status")).toString();
                const uint64_t lastActivity = object.value(QStringLiteral("last_activity_at"))
                    .toVariant().toULongLong();
                const bool statusChanged = user->status != status;
                user->status = status;
                user->lastActivity = lastActivity;
                if (statusChanged) {
                    emit user->onStatusChanged();
                }
            }
            (*fetchNext)();
        }));
    };

    (*fetchNext)();
}

void UserProfileService::searchUsers(
    const UserSearchOptions& options,
    std::function<void(QVector<const BackendUser*>)> callback)
{
    QJsonObject payload {
        {QStringLiteral("term"), options.term},
        {QStringLiteral("allow_inactive"), options.allowInactive},
        {QStringLiteral("limit"), std::max(1, options.limit)},
    };

    if (!options.teamId.isEmpty()) {
        payload.insert(QStringLiteral("team_id"), options.teamId);
    }
    if (!options.notInTeamId.isEmpty()) {
        payload.insert(QStringLiteral("not_in_team_id"), options.notInTeamId);
    }
    if (!options.inChannelId.isEmpty()) {
        payload.insert(QStringLiteral("in_channel_id"), options.inChannelId);
    }
    if (!options.notInChannelId.isEmpty()) {
        payload.insert(QStringLiteral("not_in_channel_id"), options.notInChannelId);
    }

    NetworkRequest request(QStringLiteral("users/search"));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        QVector<const BackendUser*> users;
        users.reserve(doc.array().size());

        for (const auto& value : doc.array()) {
            BackendUser* user = backend.getStorage().addUser(value.toObject());
            if (!user) {
                continue;
            }
            resolveReferences(*user);
            users.push_back(user);
        }

        if (callback) {
            callback(std::move(users));
        }
    }));
}

void UserProfileService::ensureTeamMembers(BackendTeam& team,
                                           std::function<void()> callback)
{
    const auto userIds = std::make_shared<QStringList>();
    const auto fetchPage = std::make_shared<std::function<void(int)>>();

    *fetchPage = [this, &team, callback, userIds, fetchPage](int page) {
        NetworkRequest request(
            QStringLiteral("teams/") + team.id + QStringLiteral("/members?page=")
            + QString::number(page) + QStringLiteral("&per_page=")
            + QString::number(MembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [this, &team, callback, userIds, fetchPage, page](const QJsonDocument& doc) {
                const QJsonArray members = doc.array();
                for (const auto& value : members) {
                    const QJsonObject object = value.toObject();
                    team.addMember(backend.getStorage(), object);
                    userIds->push_back(object.value(QStringLiteral("user_id")).toString());
                }

                if (members.size() == MembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                finishMemberProfiles(*userIds, callback);
            }));
    };

    (*fetchPage)(0);
}

void UserProfileService::ensureChannelMembers(BackendChannel& channel,
                                              std::function<void()> callback)
{
    const auto userIds = std::make_shared<QStringList>();
    const auto fetchPage = std::make_shared<std::function<void(int)>>();

    *fetchPage = [this, &channel, callback, userIds, fetchPage](int page) {
        NetworkRequest request(
            QStringLiteral("channels/") + channel.id + QStringLiteral("/members?page=")
            + QString::number(page) + QStringLiteral("&per_page=")
            + QString::number(MembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [this, &channel, callback, userIds, fetchPage, page](const QJsonDocument& doc) {
                const QJsonArray members = doc.array();
                for (const auto& value : members) {
                    const QJsonObject object = value.toObject();
                    channel.addMember(backend.getStorage(), object);
                    userIds->push_back(object.value(QStringLiteral("user_id")).toString());
                }

                if (members.size() == MembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                finishMemberProfiles(*userIds, callback);
            }));
    };

    (*fetchPage)(0);
}

void UserProfileService::finishMemberProfiles(const QStringList& userIds,
                                              std::function<void()> callback)
{
    const QStringList ids = uniqueNonEmptyIds(userIds);
    ensureUsers(ids, [this, ids, callback] {
        ensureStatuses(ids, callback);
    });
}

void UserProfileService::scheduleFlush()
{
    if (flushScheduled) {
        return;
    }

    flushScheduled = true;
    // Match Mattermost's missing-profile loader: wait briefly so dozens of
    // widgets/posts created in the same burst collapse into one /users/ids
    // request instead of several tiny requests.
    QTimer::singleShot(MissingProfilesWaitMs, this, [this] {
        flushScheduled = false;
        flushProfiles();
    });
}

void UserProfileService::flushProfiles()
{
    if (queuedUserIds.isEmpty()) {
        return;
    }

    QStringList batch;
    batch.reserve(std::min(
        MaxProfilesPerRequest, static_cast<int>(queuedUserIds.size())));
    auto it = queuedUserIds.begin();
    while (it != queuedUserIds.end() && static_cast<int>(batch.size()) < MaxProfilesPerRequest) {
        const QString userId = *it;
        it = queuedUserIds.erase(it);
        inFlightUserIds.insert(userId);
        batch.push_back(userId);
    }

    QJsonArray payload;
    for (const QString& userId : batch) {
        payload.push_back(userId);
    }

    NetworkRequest request(QStringLiteral("users/ids"));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback([this, batch](const QJsonDocument& doc) {
        QSet<QString> returnedIds;
        for (const auto& value : doc.array()) {
            BackendUser* user = backend.getStorage().addUser(value.toObject());
            if (!user) {
                continue;
            }
            returnedIds.insert(user->id);
            resolveReferences(*user);
            finishProfile(user->id, user);
        }

        for (const QString& userId : batch) {
            if (!returnedIds.contains(userId)) {
                finishProfile(userId, nullptr);
            }
        }

        if (!queuedUserIds.isEmpty()) {
            scheduleFlush();
        }
    }));
}

void UserProfileService::refreshKnownUsersSince(qint64 since)
{
    QStringList userIds;
    const auto& users = backend.getStorage().getAllUsers();
    userIds.reserve(static_cast<int>(users.size()));
    for (const auto& entry : users) {
        userIds.push_back(entry.first);
    }

    if (userIds.isEmpty()) {
        return;
    }

    // `since` is compared to server-side user update timestamps. The client
    // clock may not be perfectly synchronized with the server, so overlap the
    // window slightly. The endpoint still only considers our already-loaded
    // IDs and returns only changed profiles, making the overlap inexpensive.
    const qint64 safeSince = qMax<qint64>(
        0, since - ProfileReconnectClockSkewAllowanceMs);
    const QString sinceQuery = QString::number(static_cast<qlonglong>(safeSince));

    const auto pending = std::make_shared<QStringList>(std::move(userIds));
    const auto fetchNext = std::make_shared<std::function<void()>>();
    *fetchNext = [this, pending, fetchNext, sinceQuery] {
        if (pending->isEmpty()) {
            return;
        }

        QJsonArray payload;
        const int batchSize = std::min(
            MaxProfilesPerRequest, static_cast<int>(pending->size()));
        for (int i = 0; i < batchSize; ++i) {
            payload.push_back(pending->takeFirst());
        }

        NetworkRequest request(
            QStringLiteral("users/ids?since=") + sinceQuery);
        httpConnector.post(request, QByteArrayCreator(payload),
                           HttpResponseCallback([this, fetchNext](const QJsonDocument& doc) {
            for (const auto& value : doc.array()) {
                BackendUser* user = backend.getStorage().addUser(value.toObject());
                if (user) {
                    resolveReferences(*user);
                }
            }
            (*fetchNext)();
        }));
    };

    (*fetchNext)();
}

void UserProfileService::finishProfile(const QString& userId, const BackendUser* user)
{
    inFlightUserIds.remove(userId);
    const auto callbacks = waiters.take(userId);
    for (const auto& callback : callbacks) {
        if (callback) {
            callback(user);
        }
    }
}

void UserProfileService::resolveReferences(BackendUser& user)
{
    Storage& storage = backend.getStorage();

    // Keep an already-visible avatar fresh when a profile refresh tells us its
    // version changed. Profiles that have never needed an avatar remain lazy.
    if (!user.avatar.isNull() && user.avatar_picture_update != user.last_picture_update) {
        ensureAvatar(user);
    }

    if (BackendChannel* directChannel = storage.getDirectChannelByUserId(user.id)) {
        directChannel->display_name = user.getDisplayName();
        if (!storage.directChannels.members.contains(&user)) {
            storage.directChannels.members.push_back(&user);
        }
        emit directChannel->onUpdated();
    }

    for (auto teamIt = storage.teams.begin(); teamIt != storage.teams.end(); ++teamIt) {
        auto memberIt = teamIt->second.members.find(user.id);
        if (memberIt != teamIt->second.members.end()) {
            memberIt->user = &user;
        }
    }

    for (auto channelIt = storage.channels.begin(); channelIt != storage.channels.end(); ++channelIt) {
        BackendChannel* channel = channelIt.value();
        if (!channel) {
            continue;
        }

        auto memberIt = channel->members.find(user.id);
        if (memberIt != channel->members.end()) {
            memberIt->user = &user;
        }

        for (BackendPost& post : channel->posts) {
            if (!post.author && post.user_id == user.id) {
                post.author = &user;
            }
        }
        for (BackendPost& post : channel->pinnedPosts) {
            if (!post.author && post.user_id == user.id) {
                post.author = &user;
            }
        }
    }
}

} // namespace Mattermost
