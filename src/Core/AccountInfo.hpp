#pragma once

#include <QString>
#include <QPixmap>
#include <QMetaType>

#include "Enums.hpp"
#include "Snowflake.hpp"

namespace Acheron {
namespace Core {
struct AccountInfo
{
    Snowflake id;
    QString username;
    QString displayName;
    QString token;
    QString avatar;

    QString gatewayUrl = "wss://gateway.discord.gg/?encoding=json&v=9&compress=zlib-stream";
    QString restUrl = "https://discord.com/api/v9";
    QString cdnUrl = "https://cdn.discordapp.com";

    int displayOrder = 0;
    bool autoConnect = false;

    ConnectionState state = ConnectionState::Disconnected;

    [[nodiscard]] QString getEntryString() const
    {
        if (displayName.isEmpty())
            return username;

        return QString("%1 (%2)").arg(displayName).arg(username);
    }
};
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::AccountInfo)
