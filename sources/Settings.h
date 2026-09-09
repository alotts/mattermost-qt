/**
 * @file Settings.h
 * @brief Settings keys definition
 * @author Lyubomir Filipov
 * @date Dec 8, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QSettings>

static constexpr const char* DOWNLOAD_LOCATION = "config/downloadLocation";
static constexpr const char* DOWNLOAD_ASK = "config/downloadAsk";
static constexpr const char* DOWNLOAD_IMAGE_MAX_WIDTH = "config/imageMaxWidth";
static constexpr const char* DOWNLOAD_IMAGE_MAX_HEIGHT = "config/imageMaxHeight";

// Historical attachment-file cache setting. Keep the key stable for existing
// installations, but expose it explicitly on the Cache settings tab.
// Composer key bindings. When enabled, Ctrl+Enter sends the message and plain
// Enter inserts a new line. When disabled (default), plain Enter sends.
static constexpr const char* COMPOSER_SEND_ON_CTRL_ENTER = "config/sendOnCtrlEnter";
static constexpr bool COMPOSER_SEND_ON_CTRL_ENTER_DEFAULT = false;

static constexpr const char* CACHE_SIZE_MB = "config/cacheSizeMB";
static constexpr int CACHE_SIZE_MB_DEFAULT = 1000;

// Persistent post cache policy.
static constexpr const char* POST_CACHE_DISK_CHANNEL_IDLE_HOURS =
    "cache/posts/disk/channelIdleHours";
static constexpr int POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT = 10;

static constexpr const char* POST_CACHE_DISK_MAX_MB = "cache/posts/disk/maxMB";
static constexpr int POST_CACHE_DISK_MAX_MB_DEFAULT = 5 * 1024;

static constexpr const char* POST_CACHE_DISK_MAX_POSTS = "cache/posts/disk/maxPosts";
static constexpr int POST_CACHE_DISK_MAX_POSTS_DEFAULT = 10000;

static constexpr const char* POST_CACHE_DISK_MAX_THREAD_REPLIES =
    "cache/posts/disk/maxThreadReplies";
static constexpr int POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT = 1000;

static constexpr const char* POST_CACHE_DISK_MAINTENANCE_MINUTES =
    "cache/posts/disk/maintenanceMinutes";
static constexpr int POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT = 10;

// Resident post cache policy. The channel horizon is an admission/retention
// gate; it is intentionally independent from message activity so busy channels
// the user does not read cannot keep themselves hot forever.
static constexpr const char* POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES =
    "cache/posts/memory/channelIdleMinutes";
static constexpr int POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT = 60;

static constexpr const char* POST_CACHE_MEMORY_HARD_MB = "cache/posts/memory/hardMB";
static constexpr int POST_CACHE_MEMORY_HARD_MB_DEFAULT = 500;

static constexpr const char* POST_CACHE_MEMORY_TARGET_MB = "cache/posts/memory/targetMB";
static constexpr int POST_CACHE_MEMORY_TARGET_MB_DEFAULT = 400;

static constexpr const char* POST_CACHE_MEMORY_POST_TTL_MINUTES =
    "cache/posts/memory/postTtlMinutes";
static constexpr int POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT = 5;

static constexpr const char* POST_CACHE_MEMORY_SWEEP_SECONDS =
    "cache/posts/memory/sweepSeconds";
static constexpr int POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT = 30;
