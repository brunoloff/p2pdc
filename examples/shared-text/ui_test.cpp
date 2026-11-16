#include "session.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

TEST(SharedTextUi, EditsDocumentAndRejectsInvalidInput) {
    shared_text::Session session;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"session", QVariant::fromValue(&session)}});
    engine.load(QUrl::fromLocalFile(QStringLiteral(P2PDC_SHARED_TEXT_QML)));
    ASSERT_EQ(engine.rootObjects().size(), 1);
    auto *editor = engine.rootObjects().front()->findChild<QObject *>("textEditor");
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(editor->setProperty("text", QString::fromUtf8("hello 🌍")));
    EXPECT_EQ(session.document(), QString::fromUtf8("hello 🌍"));
    ASSERT_TRUE(editor->setProperty("text", QString(QChar::Null)));
    EXPECT_EQ(session.document(), QString::fromUtf8("hello 🌍"));
    EXPECT_EQ(editor->property("text").toString(), session.document());
    auto *window = engine.rootObjects().front();
    ASSERT_TRUE(window->setProperty("width", 320));
    ASSERT_TRUE(window->setProperty("height", 640));
    QCoreApplication::processEvents();
    EXPECT_GT(editor->property("height").toDouble(), 80.0);
}
TEST(SharedTextUi, CursorColorCanBeChangedAndRejectsInvalidInput) {
    shared_text::Session session;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"session", QVariant::fromValue(&session)}});
    engine.load(QUrl::fromLocalFile(QStringLiteral(P2PDC_SHARED_TEXT_QML)));
    ASSERT_EQ(engine.rootObjects().size(), 1);
    auto *field = engine.rootObjects().front()->findChild<QObject *>("cursorColorField");
    ASSERT_NE(field, nullptr);
    ASSERT_TRUE(field->setProperty("text", "#aabbcc"));
    ASSERT_TRUE(QMetaObject::invokeMethod(field, "editingFinished"));
    EXPECT_EQ(session.cursorColor(), "#aabbcc");
    ASSERT_TRUE(field->setProperty("text", "invalid"));
    ASSERT_TRUE(QMetaObject::invokeMethod(field, "editingFinished"));
    EXPECT_EQ(session.cursorColor(), "#aabbcc");
    EXPECT_EQ(field->property("text").toString(), "#aabbcc");
}
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    qmlRegisterUncreatableType<shared_text::Session>("SharedText", 1, 0, "EditorSession",
                                                     "Created by the application");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
