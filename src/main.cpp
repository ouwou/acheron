#include "App.hpp"
#include "UI/MainWindow.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Core/Session.hpp"
#include "Core/Logging.hpp"
#include "Discord/CurlUtils.hpp"

#include <curl/curl.h>

#include <QtGlobal>
#include <QNetworkAccessManager>
#include <QFontDatabase>

#include <dave/dave.h>
#include <dave/logger.h>

// potentially named after that river
// or the honkai star rail character
// who knows

void registerMetatypes()
{
    qRegisterMetaType<Acheron::Core::Snowflake>("Snowflake");

    QMetaType::registerConverter<Snowflake, QString>(
            [](const Snowflake &s) { return s.toString(); });
}

static void DaveLogSink(discord::dave::LoggingSeverity severity, const char *file, int line, const std::string &message)
{
    switch (severity) {
    case discord::dave::LoggingSeverity::LS_ERROR:
        qCCritical(LogDave) << file << ":" << line << ": " << message.c_str();
        break;
    case discord::dave::LoggingSeverity::LS_WARNING:
        qCWarning(LogDave) << file << ":" << line << ": " << message.c_str();
        break;
    case discord::dave::LoggingSeverity::LS_INFO:
        qCInfo(LogDave) << file << ":" << line << ": " << message.c_str();
        break;
    case discord::dave::LoggingSeverity::LS_VERBOSE:
        qCDebug(LogDave) << file << ":" << line << ": " << message.c_str();
        break;
    case discord::dave::LoggingSeverity::LS_NONE:
        break;
    }
}

int main(int argc, char *argv[])
{
    using namespace Acheron;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    App app(argc, argv);
    app.setStyle("Fusion");

    registerMetatypes();

    QNetworkAccessManager buildNumberNam;
    Discord::CurlUtils::fetchBuildNumber(&buildNumberNam);

#if 1
    QPalette darkCoolPurple;

    darkCoolPurple.setColor(QPalette::Window, QColor(24, 22, 34)); // deep cool background
    darkCoolPurple.setColor(QPalette::WindowText, QColor(210, 208, 225)); // soft light text
    darkCoolPurple.setColor(QPalette::Base, QColor(30, 28, 44)); // inputs, views
    darkCoolPurple.setColor(QPalette::AlternateBase, QColor(36, 34, 54)); // alternating rows
    darkCoolPurple.setColor(QPalette::ToolTipBase, QColor(40, 38, 60)); // tooltip background
    darkCoolPurple.setColor(QPalette::ToolTipText, QColor(220, 218, 240)); // tooltip text
    darkCoolPurple.setColor(QPalette::Text, QColor(200, 198, 220)); // primary text
    darkCoolPurple.setColor(QPalette::Button, QColor(40, 38, 60)); // buttons
    darkCoolPurple.setColor(QPalette::ButtonText, QColor(210, 208, 225)); // button text
    darkCoolPurple.setColor(QPalette::BrightText, QColor(189, 146, 236)); // alerts / emphasis
    darkCoolPurple.setColor(QPalette::Highlight, QColor(124, 92, 192)); // selection / focus
    darkCoolPurple.setColor(QPalette::HighlightedText, QColor(245, 244, 255)); // selected text

    qApp->setPalette(darkCoolPurple);

    qApp->setStyleSheet(
            /* ===== Base ===== */
            "QWidget {"
            "  background-color: #181622;"
            "  color: #d2d0e1;"
            "  selection-background-color: #7c5cc0;"
            "  selection-color: #f5f4ff;"
            "}"

            /* ===== Buttons ===== */
            "QPushButton {"
            "  background-color: #28263c;"
            "  border: 1px solid #3a3758;"
            "  border-radius: 4px;"
            "  padding: 6px 12px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #322f4d;"
            "  border-color: #6f5aa8;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #3a3660;"
            "}"
            "QPushButton:disabled {"
            "  background-color: #222035;"
            "  border-color: #2c2a44;"
            "  color: #7e7c99;"
            "}"

            /* ===== Inputs ===== */
            "QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
            "  background-color: #1e1c2c;"
            "  border: 1px solid #3a3758;"
            "  border-radius: 4px;"
            "  padding: 5px;"
            "}"
            "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, "
            "QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {"
            "  border-color: #7c5cc0;"
            "}"

            /* ===== ComboBox ===== */
            "QComboBox::drop-down {"
            "  border: none;"
            "  width: 20px;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background-color: #232136;"
            "  border: 1px solid #3a3758;"
            "  selection-background-color: #7c5cc0;"
            "}"

            /* ===== Checkboxes & Radios ===== */
            "QCheckBox::indicator, QRadioButton::indicator {"
            "  width: 14px;"
            "  height: 14px;"
            "  border: 1px solid #3a3758;"
            "  background-color: #1e1c2c;"
            "}"
            "QCheckBox::indicator:checked, QRadioButton::indicator:checked {"
            "  background-color: #7c5cc0;"
            "  border-color: #7c5cc0;"
            "}"

            /* ===== Scrollbars ===== */
            "QScrollBar:vertical, QScrollBar:horizontal {"
            "  background-color: #181622;"
            "  border: none;"
            "  margin: 0px;"
            "}"
            "QScrollBar::handle {"
            "  background-color: #3a3758;"
            "  border-radius: 4px;"
            "}"
            "QScrollBar::handle:hover {"
            "  background-color: #6f5aa8;"
            "}"
            "QScrollBar::add-line, QScrollBar::sub-line {"
            "  height: 0px;"
            "  width: 0px;"
            "}"

            /* ===== Tooltips ===== */
            "QToolTip {"
            "  background-color: #28263c;"
            "  color: #e6e4ff;"
            "  border: 1px solid #6f5aa8;"
            "  padding: 4px;"
            "}");
#endif

#if 0
    QPalette warmPastelPalette;

    warmPastelPalette.setColor(QPalette::Window, QColor(255, 244, 230)); // soft cream background
    warmPastelPalette.setColor(QPalette::WindowText, QColor(85, 52, 52)); // muted brown text
    warmPastelPalette.setColor(QPalette::Base, QColor(255, 250, 240)); // input backgrounds
    warmPastelPalette.setColor(QPalette::AlternateBase,
                               QColor(255, 239, 220)); // slightly darker for alternating rows
    warmPastelPalette.setColor(QPalette::ToolTipBase, QColor(255, 250, 240)); // tooltip background
    warmPastelPalette.setColor(QPalette::ToolTipText, QColor(85, 52, 52)); // tooltip text
    warmPastelPalette.setColor(QPalette::Text, QColor(102, 68, 68)); // standard text
    warmPastelPalette.setColor(QPalette::Button, QColor(255, 214, 179)); // button background
    warmPastelPalette.setColor(QPalette::ButtonText, QColor(102, 68, 68)); // button text
    warmPastelPalette.setColor(QPalette::BrightText, QColor(255, 102, 102)); // for alerts
    warmPastelPalette.setColor(QPalette::Highlight, QColor(255, 179, 128)); // selection color
    warmPastelPalette.setColor(QPalette::HighlightedText, QColor(255, 244, 230)); // selection text

    qApp->setPalette(warmPastelPalette);
#endif

    Acheron::Core::Logger::init();

    discord::dave::SetLogSink(DaveLogSink);

    qCInfo(LogCore) << "Starting Acheron...";

#ifdef Q_OS_WINDOWS
    int emojiFontId = QFontDatabase::addApplicationFont(
            QCoreApplication::applicationDirPath() + "/fonts/TwemojiCOLRv0.ttf");
    if (emojiFontId != -1) {
        QStringList families = QFontDatabase::applicationFontFamilies(emojiFontId);
        qCInfo(LogCore) << "Loaded emoji font:" << families;
        if (!families.isEmpty())
            QFontDatabase::addApplicationEmojiFontFamily(families.first());
    } else {
        qCWarning(LogCore) << "Failed to load TwemojiCOLRv0.ttf";
    }
#endif

    if (!Storage::DatabaseManager::instance().init()) {
        QMessageBox::critical(nullptr, "Fatal error",
                              "Could not initialize the database. Acheron will now close.");
        return -1;
    }

    int exitCode = 0;

    {
        Core::Session session;
        UI::MainWindow window(&session);
        window.show();

        exitCode = app.exec();
    }

    Storage::DatabaseManager::instance().shutdown();

    curl_global_cleanup();

    return exitCode;
}
