#pragma once

#include <QTreeView>
#include <QHash>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

class ChannelTreeView : public QTreeView
{
    Q_OBJECT
public:
    ChannelTreeView(QWidget *parent = nullptr);

    void performDefaultExpansion();

    void setAccountVoiceChannel(Core::Snowflake accountId, Core::Snowflake channelId);
    [[nodiscard]] bool isAccountInVoice(Core::Snowflake accountId) const;

signals:
    void markAsReadRequested(const QModelIndex &proxyIndex);
    void openInNewTabRequested(const QModelIndex &proxyIndex);
    void joinVoiceChannelRequested(const QModelIndex &proxyIndex);
    void disconnectVoiceRequested(const QModelIndex &proxyIndex);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    bool handleMouseEventForExpansion(QMouseEvent *event);

    Core::Snowflake findAccountIdForIndex(const QModelIndex &sourceIndex) const;

    QHash<Core::Snowflake, Core::Snowflake> accountVoiceChannels; // accountId -> channelId
};

} // namespace UI
} // namespace Acheron
