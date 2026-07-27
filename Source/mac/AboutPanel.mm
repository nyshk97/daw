#import <AppKit/AppKit.h>

#include "AboutPanel.h"

namespace AboutPanel
{

void show()
{
    // 素の standard panel は Info.plist から "Version <CFBundleShortVersionString>
    // (<CFBundleVersion>)" を組み立てるが、daw は両方に PROJECT_VERSION を入れて
    // いるため "Version 0.7.0 (0.7.0)" と同じ数字が二重に出る。
    // options で上書きして 1 つに見せる（実測メモ）:
    //   - ObjC では orderFrontStandardAboutPanel: は sender を取るセレクタで、
    //     辞書を渡しても黙って無視される。options 版は WithOptions: の方
    //   - ApplicationVersion は "Version" ラベルごと置き換わるので自前で付ける
    //   - Version は括弧内。空文字なら括弧ごと消える
    NSString* shortVersion = [NSBundle.mainBundle objectForInfoDictionaryKey: @"CFBundleShortVersionString"];
    [NSApp orderFrontStandardAboutPanelWithOptions: @{
        NSAboutPanelOptionApplicationVersion: [NSString stringWithFormat: @"Version %@",
                                                                          shortVersion != nil ? shortVersion : @"?"],
        NSAboutPanelOptionVersion: @""
    }];
}

} // namespace AboutPanel
