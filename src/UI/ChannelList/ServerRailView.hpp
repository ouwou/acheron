#pragma once

#include <QListView>

#include "Core/Snowflake.hpp"
#include "UI/GuildSettings/GuildSettingsSection.hpp"

namespace Acheron {
namespace UI {

class MentionJumpIndicator;

class ServerRailView : public QListView
{
    Q_OBJECT
public:
    explicit ServerRailView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model) override;
    void setGuildSettingsProvider(GuildSettingsAccess::SectionsProvider provider);

signals:
    void accountHomeClicked(Core::Snowflake accountId);
    void guildClicked(Core::Snowflake accountId, Core::Snowflake guildId);
    void folderToggleClicked(Core::Snowflake accountId, Core::Snowflake folderId);
    void markAsReadRequested(Core::Snowflake accountId, Core::Snowflake id, bool isFolder);
    void leaveGuildRequested(Core::Snowflake accountId, Core::Snowflake guildId);
    void guildSettingsRequested(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    GuildSettingsAccess::SectionsProvider guildSettingsProvider;
    MentionJumpIndicator *mentionJump = nullptr;
};

} // namespace UI
} // namespace Acheron
