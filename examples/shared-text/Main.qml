pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SharedText 1.0

ApplicationWindow {
    id: window
    required property EditorSession session
    width: 960
    height: 720
    visible: true
    title: "Shared Text — p2pdc example"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        Label {
            text: "Shared Text"
            font.pixelSize: 24
            font.bold: true
        }
        Label {
            text: window.session.status + " · Route: " + window.session.route
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(sessionBox.implicitHeight, window.height * 0.55)
            clip: true
            contentWidth: availableWidth
            GroupBox {
                id: sessionBox
                width: parent.width
                title: "Session"
                implicitHeight: sessionControls.implicitHeight + topPadding + bottomPadding
                ColumnLayout {
                    id: sessionControls
                    anchors.fill: parent
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: capability
                            placeholderText: "Shared capability (64 hex digits)"
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                        Button {
                            text: "Generate"
                            enabled: !window.session.started
                            onClicked: capability.text = window.session.newCapability()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: bootstrap
                            text: "bootstrap.jami.net"
                            placeholderText: "OpenDHT bootstrap"
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                        TextField {
                            id: bootstrapPort
                            text: "4222"
                            placeholderText: "Port"
                            Layout.preferredWidth: 75
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !window.session.started
                        }
                    }
                    TextField {
                        id: proxy
                        text: "https://dhtproxy.jami.net"
                        placeholderText: "Browser OpenDHT proxy"
                        Layout.fillWidth: true
                        enabled: !window.session.started
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: "Start"
                            enabled: !window.session.started
                            onClicked: window.session.start(capability.text, bootstrap.text, Number(bootstrapPort.text), proxy.text, stunHost.text, Number(stunPort.text), turnHost.text, Number(turnPort.text), turnUser.text, turnPassword.text, turnTransport.currentText)
                        }
                        Button {
                            text: "Stop"
                            enabled: window.session.started
                            onClicked: window.session.stop()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: "Join"
                            enabled: window.session.started
                            onClicked: window.session.join()
                        }
                        Button {
                            text: "Leave"
                            enabled: window.session.started
                            onClicked: window.session.leave()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: stunHost
                            placeholderText: "STUN host (optional)"
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                        TextField {
                            id: stunPort
                            text: "3478"
                            placeholderText: "Port"
                            Layout.preferredWidth: 75
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !window.session.started
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: turnHost
                            placeholderText: "TURN host (optional)"
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                        TextField {
                            id: turnPort
                            text: "3478"
                            placeholderText: "Port"
                            Layout.preferredWidth: 75
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !window.session.started
                        }
                        ComboBox {
                            id: turnTransport
                            model: ["udp", "tcp", "tls"]
                            enabled: !window.session.started
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: turnUser
                            placeholderText: "TURN username"
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                        TextField {
                            id: turnPassword
                            placeholderText: "TURN password"
                            echoMode: TextInput.Password
                            Layout.fillWidth: true
                            enabled: !window.session.started
                        }
                    }
                    Label {
                        text: "Share the capability privately. Anyone who has it can join and edit."
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
        RowLayout {
            Label { text: "Your cursor color" }
            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 4
                color: window.session.cursorColor
                border.color: "#888888"
            }
            TextField {
                id: cursorColor
                objectName: "cursorColorField"
                text: window.session.cursorColor
                placeholderText: "#RRGGBB"
                maximumLength: 7
                Layout.preferredWidth: 110
                onEditingFinished: {
                    if (!window.session.setCursorColor(text))
                        text = window.session.cursorColor
                }
                Connections {
                    target: window.session
                    function onCursorColorChanged() { cursorColor.text = window.session.cursorColor }
                }
            }
        }
        Label {
            text: "The document is kept in memory while the app is open."
            Layout.fillWidth: true
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                id: editor
                objectName: "textEditor"
                placeholderText: "Write together here…"
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                cursorDelegate: Rectangle {
                    width: 2
                    color: window.session.cursorColor
                }
                onCursorPositionChanged: window.session.setCursorPosition(cursorPosition)
                onTextChanged: if (text !== window.session.document)
                    window.session.edit(text)
                Repeater {
                    model: window.session.remoteCursors
                    delegate: Rectangle {
                        required property var modelData
                        objectName: "remoteCursorMarker"
                        parent: editor
                        property string documentVersion: editor.text
                        property rect caret: {
                            const limit = Math.min(editor.length, documentVersion.length);
                            return editor.positionToRectangle(Math.min(modelData.position, limit));
                        }
                        x: caret.x
                        y: caret.y
                        width: 3
                        height: Math.max(caret.height, 16)
                        color: modelData.color
                        z: 10
                        Rectangle {
                            x: 0
                            y: -3
                            width: 9
                            height: 4
                            color: parent.color
                        }
                    }
                }
                Connections {
                    target: window.session
                    function onDocumentChanged() {
                        if (editor.text !== window.session.document) {
                            const position = editor.cursorPosition;
                            editor.text = window.session.document;
                            editor.cursorPosition = Math.min(position, editor.length);
                        }
                    }
                }
            }
        }
    }
}
