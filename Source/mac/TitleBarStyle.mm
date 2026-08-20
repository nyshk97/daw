#import <AppKit/AppKit.h>

#include "TitleBarStyle.h"

namespace TitleBarStyle
{

void apply (void* nsView, bool silver, float r, float g, float b)
{
    NSView* view = (__bridge NSView*) nsView;
    NSWindow* window = view.window;
    if (window == nil)
        return;

    // titlebarAppearsTransparent にするとタイトルバーは backgroundColor で塗られる。
    // タイトル文字色は appearance で決まる（aqua=濃灰・darkAqua=白）ので、シルバー時は
    // ウィンドウ単位で aqua に固定する（アプリ全体の外観は変えない。JUCE描画のUIには無関係）
    window.titlebarAppearsTransparent = silver;
    window.backgroundColor = silver ? [NSColor colorWithSRGBRed: r green: g blue: b alpha: 1.0]
                                    : NSColor.windowBackgroundColor;
    window.appearance = silver ? [NSAppearance appearanceNamed: NSAppearanceNameAqua] : nil;
}

} // namespace TitleBarStyle
