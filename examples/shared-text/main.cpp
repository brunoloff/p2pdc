#include "session.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    shared_text::Session session;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"session", QVariant::fromValue(&session)}});
    engine.load(QUrl(QStringLiteral("qrc:/SharedText/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
