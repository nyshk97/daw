import AppKit
import CoreText

// Salva アプリアイコン生成: 1024x1024 フルブリード（macOS Tahoe はシステム側で角丸マスクされる）
// モチーフはレコード盤（レコード/ローカル音源のサンプリング素材化アプリ）。
// 使い方: make_icon [--dev] <出力パス>   （--dev で右下にアンバーの DEV リボンを重ねる）
let isDev = CommandLine.arguments.contains("--dev")
guard let outPath = CommandLine.arguments.dropFirst().first(where: { $0 != "--dev" }) else {
    fatalError("usage: make_icon [--dev] <output.png>")
}
let size = 1024
guard let rep = NSBitmapImageRep(
    bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
) else { fatalError("rep") }

NSGraphicsContext.saveGraphicsState()
let ctx = NSGraphicsContext(bitmapImageRep: rep)!
NSGraphicsContext.current = ctx

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1.0) -> NSColor {
    NSColor(
        calibratedRed: CGFloat((hex >> 16) & 0xff) / 255.0,
        green: CGFloat((hex >> 8) & 0xff) / 255.0,
        blue: CGFloat(hex & 0xff) / 255.0, alpha: alpha)
}

let W = CGFloat(size), H = CGFloat(size)
let center = NSPoint(x: W * 0.5, y: H * 0.5)

func disc(radius: CGFloat) -> NSBezierPath {
    NSBezierPath(ovalIn: NSRect(x: center.x - radius, y: center.y - radius,
                                width: radius * 2, height: radius * 2))
}

// 背景: レコードスリーブ風のクリーム紙（LaLaの寒色と対で、掘る側の暖色）
let bg = NSGradient(colorsAndLocations:
    (rgb(0xf6ecd4), 0.0), (rgb(0xecd9ae), 1.0))!
bg.draw(in: NSRect(x: 0, y: 0, width: W, height: H), angle: -60)

// 盤面: ほぼフルブリードのヴィニール（角に紙が覗く程度に大きく）
let discR = W * 0.46
rgb(0x232228).setFill()
disc(radius: discR).fill()

// 溝: 同心円のストローク（外周ほど密。うっすら明るい線）
for i in 0..<26 {
    let t = CGFloat(i) / 25.0
    let r = discR * (0.42 + t * 0.55)
    let path = disc(radius: r)
    path.lineWidth = 2.5
    rgb(0x3b3a44, 0.35 + 0.25 * (1 - t)).setStroke()
    path.stroke()
}

// 盤面の照り: 上方からの光が乗る半透明グラデーション
NSGraphicsContext.saveGraphicsState()
disc(radius: discR).addClip()
let sheen = NSGradient(colors: [rgb(0xffffff, 0.14), rgb(0xffffff, 0.0)])!
sheen.draw(from: NSPoint(x: center.x - discR, y: center.y + discR),
           to: NSPoint(x: center.x, y: center.y), options: [])
NSGraphicsContext.restoreGraphicsState()

// ラベル: LaLaの warm 系グラデを流用（兄弟アプリの血縁を色で示す）
let labelR = W * 0.165
let warm = NSGradient(colorsAndLocations:
    (rgb(0xffd23f), 0.0), (rgb(0xff7a2e), 0.48), (rgb(0xe02fb0), 1.0))!
warm.draw(in: disc(radius: labelR), angle: -90)

// センターホール（背景の紙色が覗く）
rgb(0xf2e5c6).setFill()
disc(radius: W * 0.022).fill()

// 区間選択のブラケット: ラベルを跨ぐ2本の縦線＋間の半透明帯（「区間を切り出す」アプリの記号）
let selL = W * 0.40, selR = W * 0.60
let selRect = NSRect(x: selL, y: H * 0.315, width: selR - selL, height: H * 0.37)
rgb(0xffffff, 0.16).setFill()
NSBezierPath(rect: selRect).fill()
rgb(0xf6ecd4, 0.9).setFill()
NSBezierPath(rect: NSRect(x: selL - 7, y: selRect.minY, width: 14, height: selRect.height)).fill()
NSBezierPath(rect: NSRect(x: selR - 7, y: selRect.minY, width: 14, height: selRect.height)).fill()

// dev 版: 右下コーナーを横切るアンバーのリボン＋「DEV」（LaLaと同じ流儀）
if isDev {
    NSGraphicsContext.saveGraphicsState()
    let t = NSAffineTransform()
    t.translateX(by: W, yBy: 0)   // 右下コーナー原点
    t.rotate(byDegrees: 45)       // x軸 = コーナーを横切る斜め方向
    t.concat()

    let dist: CGFloat = 300       // コーナーから対角内側へのリボン中心距離
    let bandH: CGFloat = 150
    rgb(0xdfae4a).setFill()
    NSBezierPath(rect: NSRect(x: -600, y: dist - bandH / 2,
                              width: 1200, height: bandH)).fill()

    let attrs: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: 118, weight: .heavy),
        .foregroundColor: rgb(0x1e1e22),
        .kern: 16,
    ]
    let line = CTLineCreateWithAttributedString(
        NSAttributedString(string: "DEV", attributes: attrs))
    let ink = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    let cg = NSGraphicsContext.current!.cgContext
    cg.textPosition = CGPoint(x: -ink.midX, y: dist - ink.midY)
    CTLineDraw(line, cg)
    NSGraphicsContext.restoreGraphicsState()
}

NSGraphicsContext.current?.flushGraphics()
NSGraphicsContext.restoreGraphicsState()

let png = rep.representation(using: .png, properties: [:])!
let out = URL(fileURLWithPath: outPath)
try! png.write(to: out)
print("wrote \(out.path)")
