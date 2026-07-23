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

// 背景: 左上ブルー → 右下バイオレットの対角グラデーション
let bg = NSGradient(colorsAndLocations:
    (rgb(0x3fa9f6), 0.0), (rgb(0x5b6df3), 0.5), (rgb(0x6a45ee), 1.0))!
bg.draw(in: NSRect(x: 0, y: 0, width: W, height: H), angle: -45)

// 左上寄りにうっすら白の光だまり
let glow = NSGradient(colors: [rgb(0xffffff, 0.15), rgb(0xffffff, 0.0)])!
glow.draw(fromCenter: NSPoint(x: W * 0.35, y: H * 0.80), radius: 0,
          toCenter: NSPoint(x: W * 0.35, y: H * 0.80), radius: W * 0.80,
          options: [])

// ドット波形: 8列、列ごとの粒数が波形の高さ。
// 各列に上端イエロー → オレンジ → 下端マゼンタのグラデを列単位で張る
let heights: [CGFloat] = [0.28, 0.60, 1.00, 0.68, 0.88, 0.50, 0.74, 0.32]
let margin: CGFloat = 128
let slot = (W - margin * 2) / CGFloat(heights.count)
let dotR: CGFloat = 43
let spacing: CGFloat = 106
let midY = H * 0.5
let warm = NSGradient(colorsAndLocations:
    (rgb(0xffd23f), 0.0), (rgb(0xff7a2e), 0.48), (rgb(0xe02fb0), 1.0))!

for (i, h) in heights.enumerated() {
    let x = margin + slot * CGFloat(i) + slot / 2
    let count = max(1, Int((h * 5).rounded()))
    let column = NSBezierPath()
    for k in 0..<count {
        let off = (CGFloat(k) - CGFloat(count - 1) / 2) * spacing
        column.appendOval(in: NSRect(x: x - dotR, y: midY + off - dotR,
                                     width: dotR * 2, height: dotR * 2))
    }
    // 列のドット群をまとめて塗る＝グラデが列全体（上端→下端）に一枚で架かる
    warm.draw(in: column, angle: -90)
}

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
