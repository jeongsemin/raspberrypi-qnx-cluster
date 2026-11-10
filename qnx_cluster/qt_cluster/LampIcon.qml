import QtQuick
import QtQuick.Shapes

// Warning lamp / turn arrow, drawn on a 48 x 48 grid and scaled with `u`.
Item {
    id: icon

    property string shape: "engine"     // engine, battery, oil, seatbelt, turnLeft, turnRight
    property color color: "white"
    property real alpha: 1
    property real u: 1

    readonly property var paths: ({
        engine: "M10,16 h26 v18 h-26 z M16,10 h14 v7 h-14 z M4,20 h6 v10 h-6 z M36,20 h6 v10 h-6 z",
        battery: "M4,14 h40 v24 h-40 z M10,10 h8 v6 h-8 z M30,10 h8 v6 h-8 z",
        oil: "M11,30 a11,11 0 1,0 22,0 a11,11 0 1,0 -22,0 z M22,6 L11.5,27 L32.5,27 z",
        seatbelt: "M18,10 a6,6 0 1,0 12,0 a6,6 0 1,0 -12,0 z M16,18 h16 v22 h-16 z",
        turnLeft: "M4,24 L24,6 L24,42 z M22,17 h22 v14 h-22 z",
        turnRight: "M44,24 L24,6 L24,42 z M4,17 h22 v14 h-22 z"
    })
    // marks cut into the body in the background colour
    readonly property var cuts: ({
        battery: "M11,26 h8 M29,26 h8 M33,22 v8",
        seatbelt: "M14,17 L36,42"
    })

    width: 48 * u
    height: 48 * u
    opacity: alpha

    Shape {
        width: 48; height: 48
        scale: icon.u
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: icon.color
            strokeColor: "transparent"
            PathSvg { path: icon.paths[icon.shape] }
        }
        ShapePath {
            strokeColor: icon.cuts[icon.shape] ? "#0a0b0e" : "transparent"
            strokeWidth: icon.shape === "seatbelt" ? 3.2 : 2.6
            fillColor: "transparent"
            PathSvg { path: icon.cuts[icon.shape] ? icon.cuts[icon.shape] : "M0,0" }
        }
    }
}
