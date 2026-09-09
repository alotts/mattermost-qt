/**
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

#include "SettingsWindow.h"

#include <QDir>
#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QIntValidator>
#include <QLabel>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Settings.h"
#include "ui_SettingsWindow.h"

namespace Mattermost {
namespace {

QSpinBox* makeSpinBox(QWidget* parent,
                      int minimum,
                      int maximum,
                      int value,
                      const QString& suffix = QString())
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    spin->setAccelerated(true);
    return spin;
}

QLabel* makeDescription(QWidget* parent, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

} // namespace

SettingsWindow::SettingsWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SettingsWindow)
{
    ui->setupUi(this);
    ui->imageMaxWidthValue->setValidator(new QIntValidator (10, 1000, this));
    ui->imageMaxHeightValue->setValidator(new QIntValidator (10, 1000, this));

    QString defaultDownloadDir (QStandardPaths::writableLocation (QStandardPaths::DownloadLocation));

    QSettings settings;
    ui->downloadLocationValue->setText (settings.value (DOWNLOAD_LOCATION, defaultDownloadDir).toString());
    ui->askLocationCheckBox->setChecked (settings.value (DOWNLOAD_ASK, 0).toBool());
    ui->imageMaxWidthValue->setText (settings.value (DOWNLOAD_IMAGE_MAX_WIDTH, 400).toString());
    ui->imageMaxHeightValue->setText (settings.value (DOWNLOAD_IMAGE_MAX_HEIGHT, 400).toString());

    // The old form mixed attachment settings with one unlabeled cache-size
    // field. Keep the generated UI stable for now, but move all cache policy to
    // an explicit tab and hide that legacy row.
    ui->label_3->hide();
    ui->label_4->hide();
    ui->cacheSizeMBValue->hide();

    ui->verticalLayout->removeWidget(ui->downloads);
    ui->verticalLayout->removeItem(ui->verticalSpacer);
    delete ui->verticalSpacer;
    ui->verticalSpacer = nullptr;

    auto* tabs = new QTabWidget(this);
    ui->downloads->setParent(tabs);
    tabs->addTab(ui->downloads, tr("Attachments"));

    auto* cacheScroll = new QScrollArea(tabs);
    cacheScroll->setWidgetResizable(true);
    cacheScroll->setFrameShape(QFrame::NoFrame);

    auto* cachePage = new QWidget(cacheScroll);
    auto* cacheLayout = new QVBoxLayout(cachePage);
    cacheLayout->setContentsMargins(12, 12, 12, 12);
    cacheLayout->setSpacing(12);

    auto* admissionDescription = makeDescription(
        cachePage,
        tr("Post caches are admitted by channel recency, not by message activity. "
           "A busy channel that you do not open therefore cannot keep itself hot."));
    cacheLayout->addWidget(admissionDescription);

    auto* attachmentGroup = new QGroupBox(tr("Attachment files"), cachePage);
    auto* attachmentForm = new QFormLayout(attachmentGroup);
    attachmentCacheSizeMB = makeSpinBox(
        attachmentGroup, 16, 102400,
        settings.value(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT).toInt(), tr(" MB"));
    attachmentForm->addRow(tr("Maximum disk cache:"), attachmentCacheSizeMB);
    cacheLayout->addWidget(attachmentGroup);

    auto* diskGroup = new QGroupBox(tr("Post cache on disk"), cachePage);
    auto* diskForm = new QFormLayout(diskGroup);
    diskChannelIdleHours = makeSpinBox(
        diskGroup, 1, 720,
        settings.value(POST_CACHE_DISK_CHANNEL_IDLE_HOURS,
                       POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT).toInt(),
        tr(" h"));
    diskMaxMB = makeSpinBox(
        diskGroup, 64, 102400,
        settings.value(POST_CACHE_DISK_MAX_MB,
                       POST_CACHE_DISK_MAX_MB_DEFAULT).toInt(),
        tr(" MB"));
    diskMaxPosts = makeSpinBox(
        diskGroup, 100, 1000000,
        settings.value(POST_CACHE_DISK_MAX_POSTS,
                       POST_CACHE_DISK_MAX_POSTS_DEFAULT).toInt());
    diskMaxThreadReplies = makeSpinBox(
        diskGroup, 10, 100000,
        settings.value(POST_CACHE_DISK_MAX_THREAD_REPLIES,
                       POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT).toInt());
    diskMaintenanceMinutes = makeSpinBox(
        diskGroup, 1, 1440,
        settings.value(POST_CACHE_DISK_MAINTENANCE_MINUTES,
                       POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT).toInt(),
        tr(" min"));

    diskForm->addRow(tr("Keep channels opened within:"), diskChannelIdleHours);
    diskForm->addRow(tr("Maximum compressed payload:"), diskMaxMB);
    diskForm->addRow(tr("Maximum posts:"), diskMaxPosts);
    diskForm->addRow(tr("Maximum replies per thread:"), diskMaxThreadReplies);
    diskForm->addRow(tr("Maintenance interval:"), diskMaintenanceMinutes);
    cacheLayout->addWidget(diskGroup);

    auto* memoryGroup = new QGroupBox(tr("Post cache in memory"), cachePage);
    auto* memoryForm = new QFormLayout(memoryGroup);
    memoryChannelIdleMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        settings.value(POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES,
                       POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT).toInt(),
        tr(" min"));
    memoryHardMB = makeSpinBox(
        memoryGroup, 64, 32768,
        settings.value(POST_CACHE_MEMORY_HARD_MB,
                       POST_CACHE_MEMORY_HARD_MB_DEFAULT).toInt(),
        tr(" MB"));
    memoryTargetMB = makeSpinBox(
        memoryGroup, 32, memoryHardMB->value(),
        settings.value(POST_CACHE_MEMORY_TARGET_MB,
                       POST_CACHE_MEMORY_TARGET_MB_DEFAULT).toInt(),
        tr(" MB"));
    memoryPostTtlMinutes = makeSpinBox(
        memoryGroup, 1, 1440,
        settings.value(POST_CACHE_MEMORY_POST_TTL_MINUTES,
                       POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT).toInt(),
        tr(" min"));
    memorySweepSeconds = makeSpinBox(
        memoryGroup, 5, 3600,
        settings.value(POST_CACHE_MEMORY_SWEEP_SECONDS,
                       POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT).toInt(),
        tr(" s"));

    connect(memoryHardMB, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int hardLimit) {
        memoryTargetMB->setMaximum(hardLimit);
        if (memoryTargetMB->value() > hardLimit) {
            memoryTargetMB->setValue(hardLimit);
        }
    });

    memoryForm->addRow(tr("Keep channels opened within:"), memoryChannelIdleMinutes);
    memoryForm->addRow(tr("Hard accounted limit:"), memoryHardMB);
    memoryForm->addRow(tr("Trim target after pressure:"), memoryTargetMB);
    memoryForm->addRow(tr("Cold post idle TTL:"), memoryPostTtlMinutes);
    memoryForm->addRow(tr("Sweep interval:"), memorySweepSeconds);
    cacheLayout->addWidget(memoryGroup);

    cacheLayout->addWidget(makeDescription(
        cachePage,
        tr("SQLite page size, WAL mode and bounded vacuum details are implementation "
           "invariants rather than cache-policy knobs and remain internal.")));
    cacheLayout->addStretch(1);

    cacheScroll->setWidget(cachePage);
    tabs->addTab(cacheScroll, tr("Cache"));
    ui->verticalLayout->insertWidget(0, tabs, 1);

    // Composer / key bindings
    auto* composerPage = new QWidget(tabs);
    auto* composerLayout = new QVBoxLayout(composerPage);
    composerLayout->setContentsMargins(12, 12, 12, 12);
    composerLayout->setSpacing(12);

    auto* composerGroup = new QGroupBox(tr("Message editor"), composerPage);
    auto* composerForm = new QFormLayout(composerGroup);
    sendOnCtrlEnter = new QCheckBox(composerGroup);
    sendOnCtrlEnter->setChecked(settings.value(
        COMPOSER_SEND_ON_CTRL_ENTER, COMPOSER_SEND_ON_CTRL_ENTER_DEFAULT).toBool());
    composerForm->addRow(tr("Send on Ctrl+Enter:"), sendOnCtrlEnter);
    composerLayout->addWidget(composerGroup);

    composerLayout->addWidget(makeDescription(
        composerPage,
        tr("When enabled, press Ctrl+Enter to send a message and plain Enter to "
           "insert a new line. When disabled, plain Enter sends and Ctrl+Enter "
           "inserts a new line.")));

    auto* sidebarGroup = new QGroupBox(tr("Sidebar"), composerPage);
    auto* sidebarForm = new QFormLayout(sidebarGroup);
    alwaysShowFollowingTab = new QCheckBox(sidebarGroup);
    alwaysShowFollowingTab->setChecked(settings.value(
        ALWAYS_SHOW_FOLLOWING_TAB, ALWAYS_SHOW_FOLLOWING_TAB_DEFAULT).toBool());
    sidebarForm->addRow(tr("Always show Following tab:"), alwaysShowFollowingTab);
    composerLayout->addWidget(sidebarGroup);

    composerLayout->addWidget(makeDescription(
        composerPage,
        tr("Keep the Following tab visible even while \"Show unread only\" is "
           "active. When disabled it is hidden, leaving only Channels and "
           "Attention.")));
    composerLayout->addStretch(1);
    tabs->addTab(composerPage, tr("General"));

    connect (ui->downloadLocationButton, &QPushButton::clicked, [this] {
        QDir defaultDir (ui->downloadLocationValue->text());

        if (!defaultDir.exists()) {
            defaultDir = QDir::home();
        }
        ui->downloadLocationValue->setText (QFileDialog::getExistingDirectory (this, "Select destination directory", defaultDir.absolutePath()));
    });
}

SettingsWindow::~SettingsWindow()
{
    delete ui;
}

void SettingsWindow::applyNewSettings ()
{
    QSettings settings;
    settings.setValue (DOWNLOAD_LOCATION, ui->downloadLocationValue->text());
    settings.setValue (DOWNLOAD_ASK, ui->askLocationCheckBox->isChecked());
    settings.setValue (DOWNLOAD_IMAGE_MAX_WIDTH, ui->imageMaxWidthValue->text());
    settings.setValue (DOWNLOAD_IMAGE_MAX_HEIGHT, ui->imageMaxHeightValue->text());

    settings.setValue(CACHE_SIZE_MB, attachmentCacheSizeMB->value());
    settings.setValue(POST_CACHE_DISK_CHANNEL_IDLE_HOURS, diskChannelIdleHours->value());
    settings.setValue(POST_CACHE_DISK_MAX_MB, diskMaxMB->value());
    settings.setValue(POST_CACHE_DISK_MAX_POSTS, diskMaxPosts->value());
    settings.setValue(POST_CACHE_DISK_MAX_THREAD_REPLIES, diskMaxThreadReplies->value());
    settings.setValue(POST_CACHE_DISK_MAINTENANCE_MINUTES, diskMaintenanceMinutes->value());
    settings.setValue(POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES, memoryChannelIdleMinutes->value());
    settings.setValue(POST_CACHE_MEMORY_HARD_MB, memoryHardMB->value());
    settings.setValue(POST_CACHE_MEMORY_TARGET_MB, memoryTargetMB->value());
    settings.setValue(POST_CACHE_MEMORY_POST_TTL_MINUTES, memoryPostTtlMinutes->value());
    settings.setValue(POST_CACHE_MEMORY_SWEEP_SECONDS, memorySweepSeconds->value());
    settings.setValue(COMPOSER_SEND_ON_CTRL_ENTER, sendOnCtrlEnter->isChecked());
    settings.setValue(ALWAYS_SHOW_FOLLOWING_TAB, alwaysShowFollowingTab->isChecked());
    settings.sync ();
}

} /* namespace Mattermost */
