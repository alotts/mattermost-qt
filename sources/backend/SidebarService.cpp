/**
 * @file SidebarService.cpp
 * @brief Mattermost sidebar categories and per-channel notification state.
 */

#include "SidebarService.h"

#include <algorithm>
#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"

namespace Mattermost {

namespace {

const QString channelOpenTimeCategory = QStringLiteral("channel_open_time");
const QString channelApproximateViewTimeCategory = QStringLiteral("channel_approximate_view_time");
const QString displaySettingsCategory = QStringLiteral("display_settings");
const QString collapsedReplyThreadsName = QStringLiteral("collapsed_reply_threads");
const QString sidebarSettingsCategory = QStringLiteral("sidebar_settings");
const QString visibleDirectMessagesName = QStringLiteral("limit_visible_dms_gms");
const QString directChannelShowCategory = QStringLiteral("direct_channel_show");
const QString groupChannelShowCategory = QStringLiteral("group_channel_show");
constexpr int channelMembershipsPerPage = 200;
constexpr int defaultVisibleDirectMessages = 40;

} // namespace

SidebarCategory SidebarCategory::fromJson(const QJsonObject& object)
{
    SidebarCategory category;
    category.id = object.value(QStringLiteral("id")).toString();
    category.userId = object.value(QStringLiteral("user_id")).toString();
    category.teamId = object.value(QStringLiteral("team_id")).toString();
    category.sortOrder = object.value(QStringLiteral("sort_order")).toVariant().toLongLong();
    category.sorting = object.value(QStringLiteral("sorting")).toString();
    category.type = object.value(QStringLiteral("type")).toString();
    category.displayName = object.value(QStringLiteral("display_name")).toString();
    category.muted = object.value(QStringLiteral("muted")).toBool();
    category.collapsed = object.value(QStringLiteral("collapsed")).toBool();

    for (const auto& channelId : object.value(QStringLiteral("channel_ids")).toArray()) {
        category.channelIds.push_back(channelId.toString());
    }

    return category;
}

QJsonObject SidebarCategory::toJson() const
{
    QJsonArray channels;
    for (const auto& channelId : channelIds) {
        channels.push_back(channelId);
    }

    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("user_id"), userId},
        {QStringLiteral("team_id"), teamId},
        {QStringLiteral("sort_order"), static_cast<double>(sortOrder)},
        {QStringLiteral("sorting"), sorting},
        {QStringLiteral("type"), type},
        {QStringLiteral("display_name"), displayName},
        {QStringLiteral("muted"), muted},
        {QStringLiteral("collapsed"), collapsed},
        {QStringLiteral("channel_ids"), channels},
    };
}

SidebarCategory* SidebarTeamState::category(const QString& categoryId)
{
    auto it = categories.find(categoryId);
    return it == categories.end() ? nullptr : &it.value();
}

const SidebarCategory* SidebarTeamState::category(const QString& categoryId) const
{
    auto it = categories.constFind(categoryId);
    return it == categories.cend() ? nullptr : &it.value();
}

SidebarCategory* SidebarTeamState::categoryByType(const QString& type)
{
    for (auto it = categories.begin(); it != categories.end(); ++it) {
        if (it->type == type) {
            return &it.value();
        }
    }
    return nullptr;
}

const SidebarCategory* SidebarTeamState::categoryByType(const QString& type) const
{
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        if (it->type == type) {
            return &it.value();
        }
    }
    return nullptr;
}

SidebarService& SidebarService::instance(Backend& backend)
{
    // SidebarService is a QObject child of Backend and is destroyed together
    // with it (e.g. during application shutdown). Use a self-nulling QPointer so
    // a stale entry never returns a dangling reference for a destroyed Backend;
    // the next call transparently constructs a fresh instance.
    static QHash<Backend*, QPointer<SidebarService>> instances;
    QPointer<SidebarService>& service = instances[&backend];
    if (!service) {
        service = new SidebarService(backend);
    }
    return *service;
}

SidebarService::SidebarService(Backend& backend)
    : QObject(&backend)
    , backend(backend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);

    connect(&backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        recordChannelPost(channel, post);
    });
    connect(&backend, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        recordChannelViewed(channel);
    });
    connect(&backend, &Backend::onAllTeamChannelsPopulated,
            this, &SidebarService::synchronizeChannelActivity);

    QTimer::singleShot(0, this, [this] {
        retrieveClientConfig();
        retrieveChannelPreferences();
    });
}

void SidebarService::clear()
{
    httpConnector.reset();
    mutedChannelIds.clear();
    sidebarByTeam.clear();
    activityTracker.clear();
    directChannelVisibility.clear();
    groupChannelVisibility.clear();
    visibleDirectMessagesLimit = defaultVisibleDirectMessages;
    membershipWaiters.clear();
    preferenceWaiters.clear();
    membershipsLoading = false;
    membershipsLoaded = false;
    preferencesLoading = false;
    preferencesLoaded = false;
    collapsedThreadsConfig.clear();
    hasCollapsedThreadsPreference = false;
    collapsedThreadsPreference = false;
    collapsedThreadsEnabled = false;
    emit channelActivityReset();
}

QString SidebarService::currentUserId() const
{
    return backend.getLoginUser().id;
}

QString SidebarService::categoriesPath(const QString& teamId) const
{
    return QStringLiteral("users/") + currentUserId() + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/channels/categories");
}

bool SidebarService::isChannelMuted(const BackendChannel& channel) const
{
    return isChannelMuted(channel.id);
}

bool SidebarService::isChannelMuted(const QString& channelId) const
{
    return mutedChannelIds.contains(channelId);
}

bool SidebarService::hasUnreadMention(const QString& channelId) const
{
    return activityTracker.hasMention(channelId);
}

void SidebarService::setChannelMentioned(const QString& channelId, bool mentioned)
{
    if (channelId.isEmpty()) {
        return;
    }

    const bool wasMentioned = activityTracker.hasMention(channelId);
    activityTracker.setMentioned(channelId, mentioned);
    const bool isMentioned = activityTracker.hasMention(channelId);
    if (wasMentioned == isMentioned) {
        return;
    }

    emit channelMentionedChanged(channelId, isMentioned);
    emit channelActivityChanged(channelId);
}

bool SidebarService::isChannelTracked(const QString& channelId) const
{
    if (!activityTracker.isTracked(channelId)) {
        return false;
    }

    for (auto teamIt = sidebarByTeam.cbegin(); teamIt != sidebarByTeam.cend(); ++teamIt) {
        const SidebarTeamState& state = teamIt.value();
        for (auto categoryIt = state.categories.cbegin(); categoryIt != state.categories.cend(); ++categoryIt) {
            if (visibleChannelIds(*categoryIt).contains(channelId)) {
                return true;
            }
        }
    }
    return false;
}

bool SidebarService::isChannelUnread(const BackendChannel& channel) const
{
    return activityTracker.isUnread(channel.id);
}

uint64_t SidebarService::channelActivityTime(const BackendChannel& channel) const
{
    return std::max(activityTracker.activityTime(channel.id), channel.last_post_at);
}

uint64_t SidebarService::channelRecentTime(const BackendChannel& channel) const
{
    return activityTracker.recentTime(channel.id);
}

bool SidebarService::isDirectChannelManuallyHidden(const BackendChannel& channel) const
{
    if (channel.type == BackendChannel::directChannel) {
        const auto it = directChannelVisibility.constFind(channel.name);
        return it != directChannelVisibility.cend() && !it.value();
    }
    if (channel.type == BackendChannel::groupChannel) {
        const auto it = groupChannelVisibility.constFind(channel.id);
        return it != groupChannelVisibility.cend() && !it.value();
    }
    return false;
}

QStringList SidebarService::visibleChannelIds(const SidebarCategory& category) const
{
    struct Candidate {
        QString id;
        BackendChannel* channel = nullptr;
        bool unread = false;
    };

    QVector<Candidate> candidates;
    candidates.reserve(category.channelIds.size());
    BackendChannel* currentChannel = backend.getCurrentChannel();

    for (const QString& channelId : category.channelIds) {
        BackendChannel* channel = backend.getStorage().getChannelById(channelId);
        if (!channel) {
            continue;
        }
        if (channel->delete_at != 0 && channel != currentChannel) {
            continue;
        }

        const bool unread = isChannelUnread(*channel);
        if (category.type == QStringLiteral("direct_messages")
            && isDirectChannelManuallyHidden(*channel)
            && !unread && channel != currentChannel) {
            continue;
        }
        candidates.push_back(Candidate {channelId, channel, unread});
    }

    if (category.type == QStringLiteral("direct_messages")) {
        int unreadCount = 0;
        for (const Candidate& candidate : candidates) {
            unreadCount += candidate.unread ? 1 : 0;
        }

        std::stable_sort(candidates.begin(), candidates.end(),
                         [this, currentChannel](const Candidate& a, const Candidate& b) {
            if (a.channel == currentChannel && b.channel != currentChannel) {
                return true;
            }
            if (b.channel == currentChannel && a.channel != currentChannel) {
                return false;
            }
            if (a.unread != b.unread) {
                return a.unread;
            }

            const uint64_t aRecent = std::max({channelRecentTime(*a.channel),
                                                a.channel->last_post_at,
                                                a.channel->create_at});
            const uint64_t bRecent = std::max({channelRecentTime(*b.channel),
                                                b.channel->last_post_at,
                                                b.channel->create_at});
            return aRecent > bRecent;
        });

        const int keep = std::max(visibleDirectMessagesLimit, unreadCount);
        if (candidates.size() > keep) {
            candidates.resize(keep);
        }

        QSet<QString> selected;
        for (const Candidate& candidate : candidates) {
            selected.insert(candidate.id);
        }

        QVector<Candidate> inServerOrder;
        inServerOrder.reserve(candidates.size());
        for (const QString& channelId : category.channelIds) {
            if (!selected.contains(channelId)) {
                continue;
            }
            for (const Candidate& candidate : candidates) {
                if (candidate.id == channelId) {
                    inServerOrder.push_back(candidate);
                    break;
                }
            }
        }
        candidates = std::move(inServerOrder);
    }

    if (category.sorting == QStringLiteral("recent")) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            const uint64_t aTime = std::max(a.channel->last_post_at, a.channel->create_at);
            const uint64_t bTime = std::max(b.channel->last_post_at, b.channel->create_at);
            return aTime > bTime;
        });
    } else if (category.sorting == QStringLiteral("alpha")) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return QString::compare(a.channel->display_name, b.channel->display_name,
                                    Qt::CaseInsensitive) < 0;
        });
    }

    QStringList result;
    result.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        result.push_back(candidate.id);
    }
    return result;
}

void SidebarService::markChannelViewedLocally(const BackendChannel& channel)
{
    recordChannelViewed(channel);
}

void SidebarService::synchronizeChannelActivity()
{
    const auto& channels = backend.getStorage().channels;
    for (auto it = channels.cbegin(); it != channels.cend(); ++it) {
        const BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }
        activityTracker.synchronizeChannel(
            channel->id,
            channel->last_post_at,
            static_cast<uint64_t>(std::max(0, channel->total_msg_count)),
            static_cast<uint64_t>(std::max(0, channel->total_msg_count_root)),
            channel->has_total_msg_count_root,
            collapsedThreadsEnabled);
    }
    emit channelActivityReset();
}

void SidebarService::recordChannelPost(BackendChannel& channel, const BackendPost& post)
{
    activityTracker.recordPost(channel.id, post.create_at, post.isOwnPost(),
                               !post.root_id.isEmpty(), post.currentUserMentioned);
    emit channelActivityChanged(channel.id);
}

void SidebarService::recordChannelViewed(const BackendChannel& channel)
{
    const bool wasMentioned = activityTracker.hasMention(channel.id);

    activityTracker.recordViewed(
        channel.id,
        channel.last_post_at,
        static_cast<uint64_t>(std::max(0, channel.total_msg_count)),
        static_cast<uint64_t>(std::max(0, channel.total_msg_count_root)),
        channel.has_total_msg_count_root);

    if (wasMentioned) {
        emit channelMentionedChanged(channel.id, false);
    }
    emit channelActivityChanged(channel.id);
}

void SidebarService::finishMembershipLoad()
{
    membershipsLoading = false;
    membershipsLoaded = true;
    const auto callbacks = std::move(membershipWaiters);
    membershipWaiters.clear();
    for (const auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

void SidebarService::retrieveChannelMemberships(std::function<void()> callback)
{
    if (membershipsLoaded) {
        if (callback) {
            callback();
        }
        return;
    }
    if (callback) {
        membershipWaiters.push_back(std::move(callback));
    }
    if (membershipsLoading) {
        return;
    }
    membershipsLoading = true;

    if (currentUserId().isEmpty()) {
        finishMembershipLoad();
        return;
    }

    const auto memberships = std::make_shared<QJsonArray>();
    const auto fetchPage = std::make_shared<std::function<void(int)>>();

    *fetchPage = [this, memberships, fetchPage](int page) {
        NetworkRequest request(
            QStringLiteral("users/") + currentUserId()
            + QStringLiteral("/channel_members?page=") + QString::number(page)
            + QStringLiteral("&per_page=") + QString::number(channelMembershipsPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [this, memberships, fetchPage, page](const QJsonDocument& doc) {
                const QJsonArray pageMemberships = doc.array();
                for (const auto& value : pageMemberships) {
                    memberships->push_back(value);
                }

                if (pageMemberships.size() == channelMembershipsPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                mutedChannelIds.clear();
                for (const auto& value : *memberships) {
                    const auto object = value.toObject();
                    const QString channelId = object.value(QStringLiteral("channel_id")).toString();
                    const auto notifyProps = object.value(QStringLiteral("notify_props")).toObject();
                    const bool muted = notifyProps.value(QStringLiteral("mark_unread")).toString()
                        == QStringLiteral("mention");
                    const uint64_t readMessageCount = object.value(QStringLiteral("msg_count"))
                        .toVariant().toULongLong();
                    const bool hasReadRootMessageCount = object.contains(QStringLiteral("msg_count_root"));
                    const uint64_t readRootMessageCount = hasReadRootMessageCount
                        ? object.value(QStringLiteral("msg_count_root")).toVariant().toULongLong()
                        : 0;
                    const uint64_t mentionCount = object.value(QStringLiteral("mention_count"))
                        .toVariant().toULongLong();
                    const bool hasRootMentionCount = object.contains(QStringLiteral("mention_count_root"));
                    const uint64_t rootMentionCount = hasRootMentionCount
                        ? object.value(QStringLiteral("mention_count_root")).toVariant().toULongLong()
                        : 0;

                    if (muted) {
                        mutedChannelIds.insert(channelId);
                    }

                    activityTracker.setMembership(
                        channelId,
                        object.value(QStringLiteral("last_viewed_at")).toVariant().toULongLong(),
                        readMessageCount,
                        readRootMessageCount,
                        hasReadRootMessageCount,
                        mentionCount,
                        rootMentionCount,
                        hasRootMentionCount,
                        muted);
                }

                synchronizeChannelActivity();
                finishMembershipLoad();
            }));
    };

    (*fetchPage)(0);
}

void SidebarService::finishPreferenceLoad()
{
    preferencesLoading = false;
    preferencesLoaded = true;
    const auto callbacks = std::move(preferenceWaiters);
    preferenceWaiters.clear();
    for (const auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

void SidebarService::retrieveChannelPreferences(std::function<void()> callback)
{
    if (preferencesLoaded) {
        if (callback) {
            callback();
        }
        return;
    }
    if (callback) {
        preferenceWaiters.push_back(std::move(callback));
    }
    if (preferencesLoading) {
        return;
    }
    preferencesLoading = true;

    if (currentUserId().isEmpty()) {
        finishPreferenceLoad();
        return;
    }

    NetworkRequest request(QStringLiteral("users/") + currentUserId() + QStringLiteral("/preferences"));
    httpConnector.get(request, HttpResponseCallback([this](const QJsonDocument& doc) {
        QMap<QString, uint64_t> approximateViewTimes;
        QMap<QString, uint64_t> openTimes;
        directChannelVisibility.clear();
        groupChannelVisibility.clear();
        visibleDirectMessagesLimit = defaultVisibleDirectMessages;
        hasCollapsedThreadsPreference = false;

        for (const auto& value : doc.array()) {
            const QJsonObject object = value.toObject();
            const QString category = object.value(QStringLiteral("category")).toString();
            const QString name = object.value(QStringLiteral("name")).toString();
            const QString preferenceValue = object.value(QStringLiteral("value")).toString();

            if (category == channelApproximateViewTimeCategory) {
                approximateViewTimes.insert(name, preferenceValue.toULongLong());
            } else if (category == channelOpenTimeCategory) {
                openTimes.insert(name, preferenceValue.toULongLong());
            } else if (category == displaySettingsCategory && name == collapsedReplyThreadsName) {
                hasCollapsedThreadsPreference = true;
                collapsedThreadsPreference = preferenceValue == QStringLiteral("on");
            } else if (category == sidebarSettingsCategory && name == visibleDirectMessagesName) {
                bool ok = false;
                const int limit = preferenceValue.toInt(&ok);
                if (ok && limit > 0) {
                    visibleDirectMessagesLimit = std::min(limit, defaultVisibleDirectMessages);
                }
            } else if (category == directChannelShowCategory) {
                directChannelVisibility.insert(name, preferenceValue != QStringLiteral("false"));
            } else if (category == groupChannelShowCategory) {
                groupChannelVisibility.insert(name, preferenceValue != QStringLiteral("false"));
            }
        }

        QSet<QString> channelIds;
        for (auto it = approximateViewTimes.cbegin(); it != approximateViewTimes.cend(); ++it) {
            channelIds.insert(it.key());
        }
        for (auto it = openTimes.cbegin(); it != openTimes.cend(); ++it) {
            channelIds.insert(it.key());
        }
        for (const QString& channelId : channelIds) {
            activityTracker.setRecencyTimes(
                channelId,
                approximateViewTimes.value(channelId, 0),
                openTimes.value(channelId, 0));
        }

        updateCollapsedThreadsMode();
        emit channelActivityReset();
        finishPreferenceLoad();
    }));
}

void SidebarService::retrieveClientConfig(std::function<void()> callback)
{
    NetworkRequest request(QStringLiteral("config/client?format=old"));
    httpConnector.get(request, HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        collapsedThreadsConfig = doc.object().value(QStringLiteral("CollapsedThreads")).toString();
        updateCollapsedThreadsMode();
        if (callback) {
            callback();
        }
    }));
}

void SidebarService::updateCollapsedThreadsMode()
{
    bool enabled = false;
    if (collapsedThreadsConfig == QStringLiteral("always_on")) {
        enabled = true;
    } else if (collapsedThreadsConfig == QStringLiteral("disabled")) {
        enabled = false;
    } else if (hasCollapsedThreadsPreference) {
        enabled = collapsedThreadsPreference;
    } else {
        enabled = collapsedThreadsConfig == QStringLiteral("default_on");
    }

    if (collapsedThreadsEnabled == enabled) {
        return;
    }

    collapsedThreadsEnabled = enabled;
    synchronizeChannelActivity();
}

void SidebarService::setChannelMuted(BackendChannel& channel, bool muted,
                                     std::function<void(bool)> callback)
{
    if (currentUserId().isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(QStringLiteral("channels/") + channel.id + QStringLiteral("/members/")
                           + currentUserId() + QStringLiteral("/notify_props"));
    const QJsonObject props {
        {QStringLiteral("mark_unread"), muted ? QStringLiteral("mention") : QStringLiteral("all")},
    };

    httpConnector.put(request, QByteArrayCreator(props),
                      HttpResponseCallback([this, channelId = channel.id, muted, callback](const QJsonDocument&) {
        if (muted) {
            mutedChannelIds.insert(channelId);
        } else {
            mutedChannelIds.remove(channelId);
        }
        activityTracker.setMuted(channelId, muted);
        emit channelMutedChanged(channelId, muted);
        emit channelActivityChanged(channelId);
        if (callback) {
            callback(true);
        }
    }));
}

void SidebarService::storeCategories(QString teamId, SidebarTeamState state,
                                     std::function<void(const SidebarTeamState&)> callback)
{
    QStringList directUserIds;
    for (auto it = state.categories.cbegin(); it != state.categories.cend(); ++it) {
        for (const QString& channelId : visibleChannelIds(*it)) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
            if (channel && channel->type == BackendChannel::directChannel && !channel->name.isEmpty()) {
                directUserIds.push_back(channel->name);
            }
        }
    }

    const auto statePtr = std::make_shared<SidebarTeamState>(std::move(state));
    UserProfileService::instance(backend).ensureUsers(directUserIds,
        [this, teamId = std::move(teamId), statePtr, directUserIds, callback] {
            UserProfileService::instance(backend).ensureStatuses(directUserIds,
                [this, teamId, statePtr, callback] {
                    sidebarByTeam.insert(teamId, std::move(*statePtr));
                    emit categoriesChanged(teamId);
                    if (callback) {
                        callback(sidebarByTeam[teamId]);
                    }
                });
        });
}

void SidebarService::retrieveCategories(BackendTeam& team,
                                        std::function<void(const SidebarTeamState&)> callback)
{
    const QString teamId = team.id;
    retrieveChannelMemberships([this, teamId, callback] {
        retrieveChannelPreferences([this, teamId, callback] {
            NetworkRequest request(categoriesPath(teamId));
            httpConnector.get(request, HttpResponseCallback(
                [this, teamId, callback](const QJsonDocument& doc) {
                    SidebarTeamState state;
                    const auto object = doc.object();

                    for (const auto& categoryValue : object.value(QStringLiteral("categories")).toArray()) {
                        SidebarCategory category = SidebarCategory::fromJson(categoryValue.toObject());
                        state.categories.insert(category.id, std::move(category));
                    }
                    for (const auto& categoryId : object.value(QStringLiteral("order")).toArray()) {
                        state.order.push_back(categoryId.toString());
                    }

                    for (auto it = state.categories.cbegin(); it != state.categories.cend(); ++it) {
                        if (!state.order.contains(it.key())) {
                            state.order.push_back(it.key());
                        }
                    }

                    storeCategories(teamId, std::move(state), callback);
                }));
        });
    });
}

const SidebarTeamState* SidebarService::teamState(const QString& teamId) const
{
    auto it = sidebarByTeam.constFind(teamId);
    return it == sidebarByTeam.cend() ? nullptr : &it.value();
}

SidebarTeamState* SidebarService::teamState(const QString& teamId)
{
    auto it = sidebarByTeam.find(teamId);
    return it == sidebarByTeam.end() ? nullptr : &it.value();
}

void SidebarService::updateCategory(const SidebarCategory& category,
                                    std::function<void(const SidebarCategory&)> callback)
{
    NetworkRequest request(categoriesPath(category.teamId) + QLatin1Char('/') + category.id);
    httpConnector.put(request, QByteArrayCreator(category.toJson()),
                      HttpResponseCallback([this, teamId = category.teamId, callback](const QJsonDocument& doc) {
        SidebarCategory updated = SidebarCategory::fromJson(doc.object());
        sidebarByTeam[teamId].categories.insert(updated.id, updated);
        emit categoriesChanged(teamId);
        if (callback) {
            callback(sidebarByTeam[teamId].categories[updated.id]);
        }
    }));
}

void SidebarService::updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,
                                      std::function<void(const SidebarTeamState&)> callback)
{
    QJsonArray payload;
    for (const auto& category : categories) {
        payload.push_back(category.toJson());
    }

    NetworkRequest request(categoriesPath(teamId));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
        for (const auto& value : doc.array()) {
            SidebarCategory updated = SidebarCategory::fromJson(value.toObject());
            sidebarByTeam[teamId].categories.insert(updated.id, std::move(updated));
        }
        emit categoriesChanged(teamId);
        if (callback) {
            callback(sidebarByTeam[teamId]);
        }
    }));
}

void SidebarService::updateCategoryOrder(const QString& teamId, const QStringList& order,
                                         std::function<void()> callback)
{
    QJsonArray payload;
    for (const auto& categoryId : order) {
        payload.push_back(categoryId);
    }

    NetworkRequest request(categoriesPath(teamId) + QStringLiteral("/order"));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
        QStringList updatedOrder;
        for (const auto& value : doc.array()) {
            updatedOrder.push_back(value.toString());
        }
        sidebarByTeam[teamId].order = updatedOrder;
        emit categoriesChanged(teamId);
        if (callback) {
            callback();
        }
    }));
}

} // namespace Mattermost
