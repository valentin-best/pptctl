// Pixel-compare a deck's slides with their slice of a merged deck (#142).
//
//   swift scripts/compare_slide_renders.swift MERGED.pdf SOURCE.pdf OFFSET
//
// Both PDFs are exports of the same renderer (Keynote: `export d ... as PDF`;
// PowerPoint: `save p ... as save as PDF`). Page i of SOURCE is compared with
// page OFFSET+i of MERGED, the source page against the same-size top-left
// region of the merged one (a slide merged onto a larger canvas keeps its
// geometry). Rendered at 0.4 px/pt so 4:3, 16:9 and 16:9-wide pages land on
// whole pixels; a page "differs" above a mean channel difference of 3 or a
// 99th percentile of 64. Dev-only; needs macOS (PDFKit).
import AppKit
import PDFKit

let merged = PDFDocument(url: URL(fileURLWithPath: CommandLine.arguments[1]))!
let source = PDFDocument(url: URL(fileURLWithPath: CommandLine.arguments[2]))!
let offset = Int(CommandLine.arguments[3])!
let scale: CGFloat = 0.4  // whole pixels for 4:3, 16:9 and 16:9-wide pages (405, 540, 1080 pt tall)

func render(_ page: PDFPage) -> (pixels: [UInt8], w: Int, h: Int) {
    let box = page.bounds(for: .mediaBox)
    let w = Int((box.width * scale).rounded()), h = Int((box.height * scale).rounded())
    var data = [UInt8](repeating: 255, count: w * h * 4)
    let ctx = CGContext(data: &data, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    ctx.scaleBy(x: scale, y: scale)
    ctx.translateBy(x: -box.origin.x, y: -box.origin.y)
    page.draw(with: .mediaBox, to: ctx)
    return (data, w, h)  // row 0 is the top of the page
}

var worst = 0.0
var bad = 0
for i in 0..<source.pageCount {
    guard let mp = merged.page(at: offset + i) else { print("page \(i + 1): missing in merged"); bad += 1; continue }
    let s = render(source.page(at: i)!), m = render(mp)
    let w = min(s.w, m.w), h = min(s.h, m.h)
    var diffs: [Int] = []
    diffs.reserveCapacity(w * h * 3)
    for y in 0..<h {
        for x in 0..<w {
            for c in 0..<3 {
                diffs.append(abs(Int(s.pixels[(y * s.w + x) * 4 + c]) - Int(m.pixels[(y * m.w + x) * 4 + c])))
            }
        }
    }
    diffs.sort()
    let mean = Double(diffs.reduce(0, +)) / Double(max(diffs.count, 1))
    let p99 = diffs[Int(Double(diffs.count - 1) * 0.99)]
    worst = max(worst, mean)
    let flag = (mean > 3 || p99 > 64) ? "  <-- differs" : ""
    if !flag.isEmpty { bad += 1 }
    print(String(format: "page %2d: mean %.2f  p99 %3d%@", i + 1, mean, p99, flag))
}
print(String(format: "%d pages, %d differ, worst mean %.2f", source.pageCount, bad, worst))
