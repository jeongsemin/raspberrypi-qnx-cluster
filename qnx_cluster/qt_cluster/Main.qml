import QtQuick
import QtQuick.Window
import QtQuick.Shapes

// Step 2: two dials, centre panel, warning lamps. Layout and colours follow cluster_display.c.
// The data state is decided in C++ (ClusterModel); this file only shows it. In the safe state
// (dataState 2) no value may be shown: dashes instead of numbers, no arcs, all lamps dim.
Window {
    id: win
    visible: true
    width: 1280
    height: 800
    color: "#050609"

    // ---- layout (1280x800 = 1 unit) ----
    readonly property real u: Math.min(width / 1280, height / 800)
    readonly property real dialR: Math.min(width * 0.19, height * 0.30)
    readonly property real cxl: Math.floor(width * 0.195)
    readonly property real cxr: Math.floor(width * 0.805)
    readonly property real cy: Math.floor(height * 0.56)
    readonly property real pw: (cxr - cxl) - 2 * dialR - 24 * u
    readonly property real ph: 560 * u
    readonly property real px: width / 2 - pw / 2
    readonly property real py: 120 * u
    readonly property real lx: px + 30 * u
    readonly property real rx: px + pw - 30 * u

    // ---- state ----
    readonly property bool live: cluster.dataState !== 2
    readonly property bool degraded: cluster.dataState === 1
    property int theme: 1               // follows the drive mode while there is valid data
    readonly property var themes: [
        { name: "ECO",     accent: "#60cea4", bgIn: "#0a1615", bgOut: "#030808" },
        { name: "COMFORT", accent: "#daba88", bgIn: "#111217", bgOut: "#050609" },
        { name: "SPORT",   accent: "#ee4836", bgIn: "#1d0c0c", bgOut: "#080303" }
    ]
    readonly property color accent: themes[theme].accent
    readonly property color white: "#f0f3f7"
    readonly property color dim: "#80868e"
    readonly property color safe: "#c85046"
    readonly property color lampOff: "#464c54"
    readonly property color red: "#e84c3c"
    readonly property color amber: "#ffaa00"
    readonly property color green: "#2ecc71"
    readonly property int warn: live ? cluster.warnings : 0
    property bool blink: false

    Connections {
        target: cluster
        function onChanged() {
            if (win.live && cluster.driveMode >= 0 && cluster.driveMode <= 2)
                win.theme = cluster.driveMode
        }
    }
    Timer { interval: 400; running: true; repeat: true; onTriggered: win.blink = !win.blink }
    property string clockText: Qt.formatTime(new Date(), "hh:mm")
    Timer { interval: 1000; running: true; repeat: true; onTriggered: win.clockText = Qt.formatTime(new Date(), "hh:mm") }

    // smoothed dial values: the messages come at 20 Hz, the animation is time based
    property real speedFrac: live ? Math.min(cluster.speed / 240, 1) : 0
    property real rpmFrac: live ? Math.min(cluster.rpm / 8000, 1) : 0
    Behavior on speedFrac { enabled: win.live; NumberAnimation { duration: 120 } }
    Behavior on rpmFrac { enabled: win.live; NumberAnimation { duration: 120 } }

    // ---- backdrop ----
    // The gradient and the parts of the centre panel that never change are drawn once into one texture; every frame only
    // draws that texture. It is drawn again only when the theme (the accent colour, the gradient) changes.
    Item {
        id: backdrop
        anchors.fill: parent
        layer.enabled: true
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeColor: "transparent"
                fillGradient: RadialGradient {
                    centerX: win.width / 2; centerY: win.height * 0.52
                    centerRadius: Math.hypot(win.width / 2, win.height * 0.52) * 0.95
                    focalX: centerX; focalY: centerY
                    GradientStop { position: 0.0; color: win.themes[win.theme].bgIn }
                    GradientStop { position: 1.0; color: win.themes[win.theme].bgOut }
                }
                startX: 0; startY: 0
                PathLine { x: win.width; y: 0 }
                PathLine { x: win.width; y: win.height }
                PathLine { x: 0; y: win.height }
            }
        }

        // centre panel
        Rectangle {
            x: win.px; y: win.py; width: win.pw; height: win.ph
            radius: 26 * win.u
            color: Qt.rgba(1, 1, 1, 0.04)
            border.color: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.27)
            border.width: Math.max(1, 1.4 * win.u)
        }
        Rectangle {
            x: win.px + 30 * win.u; y: win.py - 18 * win.u
            width: win.pw - 60 * win.u; height: Math.max(1, 1.6 * win.u)
            color: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.59)
        }
        Rectangle {
            x: win.px + 50 * win.u; y: win.py - 11 * win.u
            width: win.pw - 100 * win.u; height: Math.max(1, 1.6 * win.u)
            color: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.31)
        }

        Rectangle { x: win.lx; y: win.py + 160 * win.u; width: win.rx - win.lx; height: 1; color: Qt.rgba(1, 1, 1, 0.16) }
        Text { x: win.lx; y: win.py + 188 * win.u - 0.25 * font.pixelSize; text: "TRIP"; color: win.dim; font.pixelSize: 20 * win.u }
        Text { x: win.lx; y: win.py + 240 * win.u - 0.25 * font.pixelSize; text: "RANGE"; color: win.dim; font.pixelSize: 20 * win.u }
        Rectangle { x: win.lx; y: win.py + 302 * win.u; width: win.rx - win.lx; height: 1; color: Qt.rgba(1, 1, 1, 0.16) }
        Rectangle { x: win.lx; y: win.py + 442 * win.u; width: win.rx - win.lx; height: 1; color: Qt.rgba(1, 1, 1, 0.16) }
    }

    // ---- dials ----
    Dial {
        cx: win.cxl; cy: win.cy; outerR: win.dialR; u: win.u
        value: win.speedFrac; lit: win.live; accent: win.accent
        tickCount: 24; majorEvery: 4; labelMul: 10; labelDiv: 1
        caption: "KM/H"
    }
    Dial {
        cx: win.cxr; cy: win.cy; outerR: win.dialR; u: win.u
        value: win.rpmFrac; lit: win.live; accent: win.accent
        redFrom: 6500 / 8000
        tickCount: 16; majorEvery: 2; labelMul: 1; labelDiv: 2
        caption: "X1000 RPM"
    }

    // big values inside the dials (glyph height ~ 0.72 em)
    Text {
        x: win.cxl - width / 2; y: win.cy - 62 * win.u - 0.25 * font.pixelSize
        text: win.live ? Math.round(cluster.speed) : "---"
        color: win.live ? win.white : win.safe
        font.pixelSize: 164 * win.u; font.bold: true
    }
    Text {
        x: win.cxr - width / 2; y: win.cy - 62 * win.u - 0.25 * font.pixelSize
        text: win.live ? (cluster.gear >= 1 && cluster.gear <= 6 ? cluster.gear : "N") : "-"
        color: win.live ? win.white : win.safe
        font.pixelSize: 164 * win.u; font.bold: true
    }
    Text {
        x: win.cxr - width / 2; y: win.cy + 84 * win.u - 0.25 * font.pixelSize
        text: win.live ? (cluster.rpm / 1000).toFixed(1) : "-.-"
        color: win.live ? win.accent : win.safe
        font.pixelSize: 36 * win.u; font.bold: true
    }

    Text {   // clock
        x: win.px + win.pw / 2 - width / 2; y: win.py + 30 * win.u - 0.25 * font.pixelSize
        text: win.clockText; color: win.white
        font.pixelSize: 56 * win.u; font.bold: true
    }

    Rectangle {   // drive mode pill
        readonly property real w: 180 * win.u
        readonly property real h: 38 * win.u
        x: win.px + win.pw / 2 - w / 2; y: win.py + 92 * win.u
        width: w; height: h; radius: h / 2
        color: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.14)
        border.color: win.accent; border.width: Math.max(1, 1.6 * win.u)
        Text {
            anchors.centerIn: parent
            text: win.themes[win.theme].name; color: win.accent
            font.pixelSize: 25 * win.u; font.bold: true; font.letterSpacing: 2 * win.u
        }
    }

    Text {   // status notice
        visible: cluster.notice !== ""
        x: win.px + win.pw / 2 - width / 2; y: win.py + 138 * win.u - 0.25 * font.pixelSize
        text: cluster.notice
        color: win.degraded ? win.amber : win.red
        font.pixelSize: 20 * win.u; font.bold: true; font.letterSpacing: 1.6 * win.u
    }

    // trip / range
    Text {
        x: win.rx - width; y: win.py + 182 * win.u - 0.25 * font.pixelSize
        text: cluster.tripKm.toFixed(1) + " KM"; color: win.white
        font.pixelSize: 34 * win.u; font.bold: true
    }
    Text {
        x: win.rx - width; y: win.py + 234 * win.u - 0.25 * font.pixelSize
        text: win.live ? Math.round(cluster.fuel * 6) + " KM" : "--- KM"
        color: win.live ? win.white : win.safe
        font.pixelSize: 34 * win.u; font.bold: true
    }

    // fuel and coolant bars (empty in the safe state)
    Repeater {
        model: [
            { top: 340, label: "E", labelR: "F" },
            { top: 396, label: "C", labelR: "H" }
        ]
        delegate: Item {
            required property var modelData
            required property int index
            readonly property real frac: !win.live ? 0 :
                (index === 0 ? Math.max(0, Math.min(1, cluster.fuel / 100))
                             : Math.max(0, Math.min(1, (cluster.coolant - 40) / 80)))
            readonly property bool alarm: win.live && (index === 0 ? cluster.fuel < 15 : cluster.coolant > 110)
            Text { x: win.lx; y: win.py + (modelData.top - 20) * win.u - 0.25 * font.pixelSize; text: modelData.label; color: win.dim; font.pixelSize: 18 * win.u }
            Text { x: win.rx - width; y: win.py + (modelData.top - 20) * win.u - 0.25 * font.pixelSize; text: modelData.labelR; color: win.dim; font.pixelSize: 18 * win.u }
            Rectangle {
                x: win.lx; y: win.py + modelData.top * win.u
                width: win.rx - win.lx; height: 8 * win.u; radius: height / 2
                color: "#282d34"
                Rectangle {
                    visible: parent.parent.frac > 0
                    width: Math.max(parent.height, parent.width * parent.parent.frac); height: parent.height; radius: height / 2
                    color: parent.parent.alarm ? win.red : win.accent
                }
            }
        }
    }

    // gear selector P R N D (nothing lit in the safe state)
    Repeater {
        model: ["P", "R", "N", "D"]
        delegate: Item {
            required property string modelData
            required property int index
            readonly property int active: !win.live ? -1 : (cluster.gear === 0 ? 2 : 3)
            readonly property bool on: index === active
            readonly property real gx: win.px + win.pw * (0.2 + 0.2 * index)
            Text {
                x: parent.gx - width / 2; y: win.py + 470 * win.u - 0.25 * font.pixelSize
                text: modelData; color: parent.on ? win.accent : "#565c64"
                font.pixelSize: 42 * win.u; font.bold: true
            }
            Rectangle {
                visible: parent.on
                x: parent.gx - 12 * win.u; y: win.py + 518 * win.u; width: 24 * win.u; height: Math.max(1, 2.4 * win.u)
                color: win.accent
            }
        }
    }

    // error counters, once something was rejected
    Text {
        visible: cluster.anyErrors
        x: win.px + win.pw / 2 - width / 2; y: win.py + 528 * win.u - 0.25 * font.pixelSize
        text: cluster.countersA; color: win.dim; font.pixelSize: 14 * win.u
    }
    Text {
        visible: cluster.anyErrors
        x: win.px + win.pw / 2 - width / 2; y: win.py + 542 * win.u - 0.25 * font.pixelSize
        text: cluster.countersB; color: win.dim; font.pixelSize: 14 * win.u
    }

    // ---- warning lamps (dim when off) ----
    readonly property real lampY: 28 * u
    readonly property real cell: 70 * u
    readonly property real lampX0: width / 2 - 2 * cell + (cell - 48 * u) / 2
    LampIcon { shape: "engine";    u: win.u; x: win.lampX0;                  y: win.lampY; color: (win.warn & 1) ? win.amber : win.lampOff; alpha: (win.warn & 1) ? 1 : 0.35 }
    LampIcon { shape: "battery";   u: win.u; x: win.lampX0 + win.cell;       y: win.lampY; color: (win.warn & 2) ? win.red : win.lampOff;   alpha: (win.warn & 2) ? 1 : 0.35 }
    LampIcon { shape: "oil";       u: win.u; x: win.lampX0 + 2 * win.cell;   y: win.lampY; color: (win.warn & 4) ? win.red : win.lampOff;   alpha: (win.warn & 4) ? 1 : 0.35 }
    LampIcon { shape: "seatbelt";  u: win.u; x: win.lampX0 + 3 * win.cell;   y: win.lampY; color: (win.warn & 8) ? win.red : win.lampOff;   alpha: (win.warn & 8) ? 1 : 0.35 }
    LampIcon {
        shape: "turnLeft"; u: win.u; x: win.cxl - 24 * win.u; y: win.lampY
        readonly property bool on: (win.warn & 16) && win.blink
        color: on ? win.green : win.lampOff; alpha: on ? 1 : 0.27
    }
    LampIcon {
        shape: "turnRight"; u: win.u; x: win.cxr - 24 * win.u; y: win.lampY
        readonly property bool on: (win.warn & 32) && win.blink
        color: on ? win.green : win.lampOff; alpha: on ? 1 : 0.27
    }

    // data link lamp (top left): green valid, amber errors seen recently, red safe state
    Rectangle {
        x: 21 * win.u; y: 21 * win.u; width: 14 * win.u; height: width; radius: width / 2
        color: cluster.dataState === 0 ? win.green : (cluster.dataState === 1 ? win.amber : win.red)
    }

    // service lamp (top right): lit while cluster_diag holds a confirmed DTC. The number is how many.
    // It shows the diagnostic memory, not the data link, so it also stays on in the safe state.
    Rectangle {
        visible: cluster.serviceLamp
        readonly property real w: 150 * win.u
        readonly property real h: 30 * win.u
        x: win.width - 24 * win.u - w; y: win.lampY + 9 * win.u
        width: w; height: h; radius: h / 2
        color: Qt.rgba(win.amber.r, win.amber.g, win.amber.b, 0.16)
        border.color: win.amber; border.width: Math.max(1, 1.6 * win.u)
        Text {
            anchors.centerIn: parent
            text: "SERVICE  " + cluster.dtcCount
            color: win.amber; font.pixelSize: 15 * win.u; font.bold: true
        }
    }
}
