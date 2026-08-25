#pragma once

#include <QLineEdit>

#include <optional>

#include "Core/ProxyConfig.hpp"

namespace Acheron {
namespace UI {

class ProxyLineEdit : public QLineEdit
{
    Q_OBJECT
public:
    explicit ProxyLineEdit(QWidget *parent = nullptr);

    // empty text = no proxy
    // bad text is nullopt
    [[nodiscard]] std::optional<Core::ProxyConfig> parseOrWarn();
};

} // namespace UI
} // namespace Acheron
