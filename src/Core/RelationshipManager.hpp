#pragma once

#include <QHash>
#include <QObject>

#include <optional>

#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Snowflake.hpp"

namespace Acheron {
namespace Core {

class RelationshipManager : public QObject
{
    Q_OBJECT
public:
    explicit RelationshipManager(QObject *parent = nullptr);

    void loadFromReady(const QList<Discord::Relationship> &relationships);

    [[nodiscard]] std::optional<Discord::Relationship> getRelationship(Snowflake userId) const;

    [[nodiscard]] bool isFriend(Snowflake userId) const;
    [[nodiscard]] bool isBlockedOrIgnored(Snowflake userId) const;

public slots:
    void onRelationshipAdded(const Discord::Relationship &rel);
    void onRelationshipUpdated(const Discord::RelationshipPartial &rel);
    void onRelationshipRemoved(const Discord::RelationshipPartial &rel);

signals:
    void relationshipChanged(Snowflake userId);

private:
    QHash<Snowflake, Discord::Relationship> store;
};

} // namespace Core
} // namespace Acheron
