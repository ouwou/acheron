#pragma once

#include "UI/Dialogs/BasePopup.hpp"

#include "Core/Snowflake.hpp"

#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace Acheron {
namespace Discord {
struct Message;
}
namespace Core {
class ClientInstance;
class ImageManager;
}
namespace UI {

// Discord's server-side message search for the current server (or DM). Results are fetched a
// page (25) at a time and dropped when the popup closes.
class MessageSearchPopup : public BasePopup
{
    Q_OBJECT
public:
    explicit MessageSearchPopup(Core::ImageManager *images, QWidget *parent = nullptr);

    void configure(Core::ClientInstance *instance, Core::Snowflake guildId, Core::Snowflake channelId, const QString &channelName);

signals:
    void messageActivated(Core::Snowflake channelId, Core::Snowflake messageId);
    void linkActivated(const QString &url);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void startSearch();
    void fetchPage();
    void clearResults();
    void setStatus(const QString &text);
    void addCard(const Discord::Message &msg);
    [[nodiscard]] int bodyWidth() const;

    Core::ImageManager *images;
    QPointer<Core::ClientInstance> instance;
    Core::Snowflake guildId;
    Core::Snowflake channelId;

    QString query;
    QStringList has;
    int offset = 0;
    int total = 0;
    int generation = 0;
    bool loading = false;

    QLineEdit *input = nullptr;
    QComboBox *scope = nullptr;
    QScrollArea *scroll = nullptr;
    QVBoxLayout *cardsLayout = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *loadMoreButton = nullptr;
    QTimer *retryTimer = nullptr;
};

} // namespace UI
} // namespace Acheron
