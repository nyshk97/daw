#import <AppKit/AppKit.h>
#include <cmath>

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
    // 信号ボタンの配色は appearance で決まるので、シルバー時はウィンドウ単位で aqua に固定する
    // （アプリ全体の外観は変えない。JUCE描画のUIには無関係）
    window.titlebarAppearsTransparent = silver;
    window.backgroundColor = silver ? [NSColor colorWithSRGBRed: r green: g blue: b alpha: 1.0]
                                    : NSColor.windowBackgroundColor;
    window.appearance = silver ? [NSAppearance appearanceNamed: NSAppearanceNameAqua] : nil;
    window.titleVisibility = silver ? NSWindowTitleHidden : NSWindowTitleVisible;
    if (silver)
        window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    else
        window.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
}

int titleBarInset (void* nsView)
{
    NSView* view = (__bridge NSView*) nsView;
    NSWindow* window = view.window;
    if (window == nil || (window.styleMask & NSWindowStyleMaskFullSizeContentView) == 0)
        return 0;
    // contentLayoutRect はタイトルバーを除いた領域。contentView 全体との差がタイトルバー高
    return (int) std::lround (window.contentView.frame.size.height - window.contentLayoutRect.size.height);
}

void beginWindowDrag (void* nsView)
{
    NSView* view = (__bridge NSView*) nsView;
    NSWindow* window = view.window;
    NSEvent* event = NSApp.currentEvent;
    if (window != nil && event != nil && event.type == NSEventTypeLeftMouseDown)
        [window performWindowDragWithEvent: event];
}

void titleBarDoubleClicked (void* nsView)
{
    NSView* view = (__bridge NSView*) nsView;
    NSWindow* window = view.window;
    if (window == nil)
        return;
    NSString* action = [NSUserDefaults.standardUserDefaults stringForKey: @"AppleActionOnDoubleClick"];
    if ([action isEqualToString: @"Minimize"])
        [window performMiniaturize: nil];
    else if (action == nil || [action isEqualToString: @"Maximize"])
        [window performZoom: nil];
}

} // namespace TitleBarStyle
