#include "UI/Emoji/EmojiGridView.hpp"

#include <QApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>

using namespace Acheron;
using UI::EmojiGridView;

class TestEmojiGrid : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();

    void firstCellBecomesActive();
    void horizontalMovesWrapAcrossRowsAndSections();
    void verticalMovesClampToShorterRows();
    void movesStopAtTheGridEdges();
    void keyboardSelectionScrollsIntoView();
    void clickReportsEmojiAndModifiers();
    void headerClickCollapsesSection();
    void topSectionFollowsScroll();

private:
    [[nodiscard]] QString activeName() const;

    EmojiGridView *grid = nullptr;
};

static Core::PickerSection makeSection(const QString &id, const QString &title, int count)
{
    Core::PickerSection section;
    section.id = id;
    section.title = title;
    for (int i = 0; i < count; i++) {
        Core::PickerEmoji emoji;
        emoji.name = id + QString::number(i);
        emoji.surrogates = QString::fromUtf8("\xF0\x9F\x98\x80");
        section.emojis.append(emoji);
    }
    return section;
}

void TestEmojiGrid::initTestCase()
{
    QCOMPARE(EmojiGridView::Columns, 9);
}

void TestEmojiGrid::init()
{
    grid = new EmojiGridView;
    grid->resize(grid->sizeHint());
    grid->setSections({ makeSection("a", "First", 11), makeSection("b", "Second", 3), makeSection("c", "Third", 200) });
    grid->show();
    QVERIFY(QTest::qWaitForWindowExposed(grid));
}

void TestEmojiGrid::cleanup()
{
    delete grid;
    grid = nullptr;
}

QString TestEmojiGrid::activeName() const
{
    const Core::PickerEmoji *emoji = grid->activeEmoji();
    return emoji ? emoji->name : QString();
}

void TestEmojiGrid::firstCellBecomesActive()
{
    QVERIFY(grid->activeEmoji() == nullptr);

    QSignalSpy activeChanged(grid, &EmojiGridView::activeEmojiChanged);
    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("a0"));
    QVERIFY(grid->activeIsFirst());
    QCOMPARE(activeChanged.count(), 1);

    grid->moveActive(1, 0);
    QVERIFY(!grid->activeIsFirst());
}

void TestEmojiGrid::horizontalMovesWrapAcrossRowsAndSections()
{
    grid->activateFirst();
    for (int i = 0; i < 8; i++)
        grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a8"));

    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a9"));
    grid->moveActive(1, 0);
    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("b0"));

    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("a10"));
}

void TestEmojiGrid::verticalMovesClampToShorterRows()
{
    grid->activateFirst();
    for (int i = 0; i < 4; i++)
        grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a4"));

    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("a10"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("b1"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("c1"));

    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("b1"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a10"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a1"));
}

void TestEmojiGrid::movesStopAtTheGridEdges()
{
    grid->activateFirst();
    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("a0"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a0"));

    grid->setSections({ makeSection("a", "Only", 2) });
    grid->activateFirst();
    grid->moveActive(1, 0);
    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a1"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("a1"));
}

void TestEmojiGrid::keyboardSelectionScrollsIntoView()
{
    grid->activateFirst();
    QCOMPARE(grid->verticalScrollBar()->value(), 0);

    constexpr int RowsDown = 20;
    for (int i = 0; i < RowsDown; i++)
        grid->moveActive(0, 1);

    QCOMPARE(activeName(), QStringLiteral("c%1").arg((RowsDown - 3) * EmojiGridView::Columns));
    QVERIFY(grid->verticalScrollBar()->value() > 0);
    QCOMPARE(grid->topSection(), 2);
}

void TestEmojiGrid::clickReportsEmojiAndModifiers()
{
    grid->setSections({ makeSection("a", QString(), 20) });

    QStringList clickedNames;
    Qt::KeyboardModifiers clickedWith;
    connect(grid, &EmojiGridView::emojiClicked, this, [&](const Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers) {
        clickedNames.append(emoji.name);
        clickedWith = modifiers;
    });

    const QPoint secondRowFirstCell(EmojiGridView::CellPx - 1, EmojiGridView::CellPx + EmojiGridView::CellPx / 2);
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::ShiftModifier, secondRowFirstCell);

    QCOMPARE(clickedNames, QStringList({ QStringLiteral("a9") }));
    QVERIFY(clickedWith.testFlag(Qt::ShiftModifier));
}

void TestEmojiGrid::headerClickCollapsesSection()
{
    QSignalSpy collapsedChanged(grid, &EmojiGridView::collapsedSectionsChanged);
    const QPoint firstHeader(EmojiGridView::CellPx, 4);
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, firstHeader);

    QCOMPARE(collapsedChanged.count(), 1);
    QVERIFY(grid->collapsedSections().contains(QStringLiteral("a")));

    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("b0"));
    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("b0"));

    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, firstHeader);
    QVERIFY(grid->collapsedSections().isEmpty());
    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("a0"));
}

void TestEmojiGrid::topSectionFollowsScroll()
{
    QSignalSpy topChanged(grid, &EmojiGridView::topSectionChanged);
    QCOMPARE(grid->topSection(), 0);

    grid->scrollToSection(2);
    QCOMPARE(grid->topSection(), 2);
    QCOMPARE(topChanged.last().at(0).toInt(), 2);

    grid->scrollToSection(1);
    QCOMPARE(grid->topSection(), 1);
}

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    TestEmojiGrid test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_EmojiGrid.moc"
