import AppKit
import CoreText

// LaLa アプリアイコン生成: 1024x1024 フルブリード（macOS Tahoe はシステム側で角丸マスクされる）
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

// 背景: 上から下へわずかに明→暗のダークグラデーション（UIの 0x2c2c30 系に合わせる）
let bg = NSGradient(colors: [rgb(0x2e2e34), rgb(0x1a1a1e)])!
bg.draw(in: NSRect(x: 0, y: 0, width: W, height: H), angle: -90)

// うっすら中央にブルーの光だまり（ビネットの逆）で奥行きを出す
let glow = NSGradient(colors: [rgb(0x4a6ea9, 0.18), rgb(0x4a6ea9, 0.0)])!
glow.draw(fromCenter: NSPoint(x: W * 0.5, y: H * 0.52), radius: 0,
          toCenter: NSPoint(x: W * 0.5, y: H * 0.52), radius: W * 0.62,
          options: [])

// 波形バー: 中央線対称の縦バー15本
let heights: [CGFloat] = [0.16, 0.34, 0.55, 0.80, 0.62, 0.92, 0.70, 0.44,
                          0.66, 0.88, 0.56, 0.38, 0.52, 0.28, 0.15]
let n = heights.count
let margin: CGFloat = 96          // 左右余白（フルブリードでも波形自体は少し内側に）
let gapRatio: CGFloat = 0.62      // バー幅に対する間隔
let slot = (W - margin * 2) / CGFloat(n)
let barW = slot / (1 + gapRatio)
let maxHalf: CGFloat = 330
let midY = H * 0.52

// 再生ヘッドは 9本目と10本目の間
let playheadX = margin + slot * 9.0 - (slot - barW) / 2

for (i, h) in heights.enumerated() {
    let x = margin + slot * CGFloat(i) + (slot - barW) / 2
    let half = maxHalf * h
    let bar = NSBezierPath(
        roundedRect: NSRect(x: x, y: midY - half, width: barW, height: half * 2),
        xRadius: barW / 2, yRadius: barW / 2)
    let played = (x + barW / 2) < playheadX
    if played {
        // 再生済み: ブルーの縦グラデーション（下を少し濃く）
        let g = NSGradient(colors: [rgb(0x6f9bd6), rgb(0x4a6ea9)])!
        g.draw(in: bar, angle: 90)
    } else {
        // 未再生: くすんだグレーブルー
        rgb(0x54565e).setFill()
        bar.fill()
    }
}

// 再生ヘッド: 赤い縦ライン＋グロー＋上部の下向き三角
let phTop = H * 0.10, phBottom = H * 0.94
for (radius, alpha) in [(CGFloat(26), 0.10), (16, 0.20), (9, 0.35)] {
    rgb(0xff5a4d, alpha).setFill()
    NSBezierPath(
        roundedRect: NSRect(x: playheadX - radius, y: H - phBottom,
                            width: radius * 2, height: phBottom - phTop),
        xRadius: radius, yRadius: radius).fill()
}
rgb(0xff5a4d).setFill()
NSBezierPath(
    roundedRect: NSRect(x: playheadX - 5, y: H - phBottom,
                        width: 10, height: phBottom - phTop),
    xRadius: 5, yRadius: 5).fill()

// 三角マーカー（ルーラーの再生ヘッド風・上端）
let triW: CGFloat = 88, triH: CGFloat = 74
let triTopY = H - phTop + 8
let tri = NSBezierPath()
tri.move(to: NSPoint(x: playheadX - triW / 2, y: triTopY))
tri.line(to: NSPoint(x: playheadX + triW / 2, y: triTopY))
tri.line(to: NSPoint(x: playheadX, y: triTopY - triH))
tri.close()
rgb(0xff5a4d).setFill()
tri.fill()

// dev 版: 右下コーナーを横切るアンバーのリボン＋「DEV」
// システムの角丸マスクはコーナーから対角に約90px削るだけなので、
// 対角距離300pxに置いた文字は削られない
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

    // 文字は実インク領域（glyph path bounds）でリボン中心へ合わせる
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
