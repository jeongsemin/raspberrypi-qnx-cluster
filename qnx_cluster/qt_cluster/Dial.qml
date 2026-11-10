import QtQuick
import QtQuick.Shapes

// One round dial: dim track, tick marks with labels, and the lit arc up to `value`.
// Geometry follows cluster_display.c: the arc starts at 135 degrees and sweeps 270 clockwise.
Item {
    id: dial

    property real cx
    property real cy
    property real outerR                 // outer radius of the dial
    property real u: 1              // screen scale (1280x800 = 1)
    property real value: 0          // 0..1
    property bool lit: true         // false in the safe state: no arc, no marker
    property color accent: "white"
    property real redFrom: 2.0      // fraction from which the scale and the arc are red (2 = never)
    property int tickCount: 24
    property int majorEvery: 4
    property int labelMul: 1
    property int labelDiv: 1
    property string caption: ""

    readonly property real startDeg: 135
    readonly property real sweepDeg: 270
    readonly property real halfThick: 4.5 * u
    readonly property real rc: outerR - 6 * u - halfThick     // radius of the arc centre line
    readonly property real tickOuter: rc - halfThick - 9 * u
    readonly property color trackColor: "#282d34"
    readonly property color trackRed: "#56241f"
    readonly property color redColor: "#e84c3c"
    readonly property real frac: Math.max(0, Math.min(1, value))
    readonly property real redClamped: Math.min(redFrom, 1)

    function px(radius, deg) { return cx + radius * Math.cos(deg * Math.PI / 180) }
    function py(radius, deg) { return cy + radius * Math.sin(deg * Math.PI / 180) }

    anchors.fill: parent

    // The face (track, ticks, numbers, caption) does not change while the dial runs. It is drawn once into a texture of
    // its own, and every frame only draws that texture instead of about 100 items. The coordinates in here are relative
    // to the face (lcx, lcy is the dial centre); the lit arc and the marker below stay in window coordinates.
    Item {
        id: face
        x: Math.round(dial.cx - dial.outerR)
        y: Math.round(dial.cy - dial.outerR)
        width: Math.ceil(2 * dial.outerR)
        height: width
        layer.enabled: true
        readonly property real lcx: dial.cx - x
        readonly property real lcy: dial.cy - y
        function px(radius, deg) { return lcx + radius * Math.cos(deg * Math.PI / 180) }
        function py(radius, deg) { return lcy + radius * Math.sin(deg * Math.PI / 180) }

        // track (red part beyond redFrom)
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: dial.trackColor
                strokeWidth: 2 * dial.halfThick
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: face.lcx; centerY: face.lcy; radiusX: dial.rc; radiusY: dial.rc
                    startAngle: dial.startDeg
                    sweepAngle: dial.sweepDeg * dial.redClamped
                }
            }
            ShapePath {
                strokeColor: dial.redFrom < 1 ? dial.trackRed : "transparent"
                strokeWidth: 2 * dial.halfThick
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: face.lcx; centerY: face.lcy; radiusX: dial.rc; radiusY: dial.rc
                    startAngle: dial.startDeg + dial.sweepDeg * dial.redClamped
                    sweepAngle: Math.max(0.01, dial.sweepDeg * (1 - dial.redClamped))
                }
            }
        }

        // ticks and labels
        Repeater {
            model: dial.tickCount + 1
            delegate: Item {
                required property int index
                readonly property real f: index / dial.tickCount
                readonly property real deg: dial.startDeg + dial.sweepDeg * f
                readonly property bool major: index % dial.majorEvery === 0
                readonly property bool red: f >= dial.redFrom
                readonly property real len: (major ? 22 : 11) * dial.u
                readonly property color col: red ? "#be524a" : (major ? "#cdd2d8" : "#70767e")

                x: face.lcx
                y: face.lcy
                rotation: deg
                Rectangle {
                    x: dial.tickOuter - parent.len
                    y: -height / 2
                    width: parent.len
                    height: (parent.major ? 3.2 : 2) * dial.u
                    radius: height / 2
                    color: parent.col
                }
            }
        }
        Repeater {
            model: dial.tickCount + 1
            delegate: Text {
                required property int index
                readonly property real deg: dial.startDeg + dial.sweepDeg * index / dial.tickCount
                readonly property bool major: index % dial.majorEvery === 0
                readonly property real r: dial.tickOuter - 22 * dial.u - 20 * dial.u
                visible: major
                text: Math.round(index * dial.labelMul / dial.labelDiv)
                font.pixelSize: 22 * dial.u
                color: (index / dial.tickCount) >= dial.redFrom ? "#be524a" : "#cdd2d8"
                x: face.px(r, deg) - width / 2
                y: face.py(r, deg) - height / 2
            }
        }

        Text {
            text: dial.caption
            color: "#80868e"
            font.pixelSize: 22 * dial.u
            font.letterSpacing: 1.5 * dial.u
            x: face.lcx - width / 2
            y: face.lcy + dial.outerR * 0.60 - 6 * dial.u
        }
    }

    // lit arc: accent up to the redline, red beyond it (halo first, then the core)
    Shape {
        anchors.fill: parent
        visible: dial.lit
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Qt.rgba(dial.accent.r, dial.accent.g, dial.accent.b, 0.22)
            strokeWidth: 2 * dial.halfThick * 2.2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: dial.cx; centerY: dial.cy; radiusX: dial.rc; radiusY: dial.rc
                startAngle: dial.startDeg
                sweepAngle: Math.max(0.01, dial.sweepDeg * Math.min(dial.frac, dial.redClamped))
            }
        }
        ShapePath {
            strokeColor: dial.accent
            strokeWidth: 2 * dial.halfThick
            fillColor: "transparent"
            capStyle: ShapePath.FlatCap
            PathAngleArc {
                centerX: dial.cx; centerY: dial.cy; radiusX: dial.rc; radiusY: dial.rc
                startAngle: dial.startDeg
                sweepAngle: Math.max(0.01, dial.sweepDeg * Math.min(dial.frac, dial.redClamped))
            }
        }
        ShapePath {
            strokeColor: dial.frac > dial.redClamped && dial.redFrom < 1 ? dial.redColor : "transparent"
            strokeWidth: 2 * dial.halfThick
            fillColor: "transparent"
            capStyle: ShapePath.FlatCap
            PathAngleArc {
                centerX: dial.cx; centerY: dial.cy; radiusX: dial.rc; radiusY: dial.rc
                startAngle: dial.startDeg + dial.sweepDeg * dial.redClamped
                sweepAngle: Math.max(0.01, dial.sweepDeg * (dial.frac - dial.redClamped))
            }
        }
    }

    // marker at the end of the lit arc
    Rectangle {
        visible: dial.lit
        readonly property real deg: dial.startDeg + dial.sweepDeg * dial.frac
        readonly property color hc: dial.frac >= dial.redFrom ? dial.redColor : dial.accent
        width: 34 * dial.u; height: width; radius: width / 2
        color: Qt.rgba(hc.r, hc.g, hc.b, 0.24)
        x: dial.px(dial.rc, deg) - width / 2
        y: dial.py(dial.rc, deg) - height / 2
        Rectangle {
            anchors.centerIn: parent
            width: 17 * dial.u; height: width; radius: width / 2
            color: "#f0f3f7"
        }
    }
}
