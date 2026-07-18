#include "RelationshipManager.hpp"

#include "Logging.hpp"

namespace Acheron {
namespace Core {

RelationshipManager::RelationshipManager(QObject *parent) : QObject(parent) {}

void RelationshipManager::loadFromReady(const QList<Discord::Relationship> &relationships)
{
    store.clear();
    store.reserve(relationships.size());
    for (const auto &r : relationships) {
        if (r.id.hasValue())
            store.insert(r.id.get(), r);
    }
    qCInfo(LogCore) << "Loaded" << store.size() << "relationships from READY";
}

std::optional<Discord::Relationship> RelationshipManager::getRelationship(Snowflake userId) const
{
    auto it = store.constFind(userId);
    if (it == store.constEnd())
        return std::nullopt;
    return it.value();
}

bool RelationshipManager::isFriend(Snowflake userId) const
{
    auto it = store.constFind(userId);
    if (it == store.constEnd())
        return false;
    if (!it.value().type.hasValue())
        return false;
    return it.value().type.get() == Discord::RelationshipType::FRIEND;
}

void RelationshipManager::onRelationshipAdded(const Discord::Relationship &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    store.insert(id, rel);
    emit relationshipChanged(id);
}

void RelationshipManager::onRelationshipUpdated(const Discord::RelationshipPartial &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    auto it = store.find(id);
    if (it == store.end())
        return;
    it.value().type = rel.type.get();
    if (rel.nickname.hasValue())
        it.value().nickname = rel.nickname.get();
    else if (rel.nickname.isNull())
        it.value().nickname = nullptr;
    if (rel.since.hasValue())
        it.value().since = rel.since.get();
    if (rel.strangerRequest.hasValue())
        it.value().strangerRequest = rel.strangerRequest.get();
    it.value().userIgnored = rel.userIgnored.get();
    emit relationshipChanged(id);
}

void RelationshipManager::onRelationshipRemoved(const Discord::RelationshipPartial &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    if (store.remove(id))
        emit relationshipChanged(id);
}

} // namespace Core
} // namespace Acheron
