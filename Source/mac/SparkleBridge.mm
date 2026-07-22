#import <Sparkle/Sparkle.h>

#include "SparkleBridge.h"

// canCheckForUpdates を KVO で追うためのオブザーバ（Sparkle サンプルの
// SwiftUI 版と同等のことを ObjC で行う）。コールバックはメインスレッドで呼ぶ。
@interface DawSparkleObserver : NSObject
@property (nonatomic, copy) void (^onChange) (BOOL canCheck);
@end

@implementation DawSparkleObserver

- (void) observeValueForKeyPath: (NSString*) keyPath
                       ofObject: (id) object
                         change: (NSDictionary*) change
                        context: (void*) context
{
    if ([keyPath isEqualToString: @"canCheckForUpdates"])
    {
        const BOOL canCheck = [(SPUUpdater*) object canCheckForUpdates];
        dispatch_async (dispatch_get_main_queue(), ^{
            if (self.onChange != nil)
                self.onChange (canCheck);
        });
    }
}

@end

namespace
{
SPUStandardUpdaterController* updaterController = nil;
DawSparkleObserver* observer = nil;
}

namespace SparkleBridge
{

void init (std::function<void (bool)> onCanCheckChanged)
{
    if (updaterController != nil)
        return;

    updaterController = [[SPUStandardUpdaterController alloc] initWithStartingUpdater: YES
                                                                      updaterDelegate: nil
                                                                   userDriverDelegate: nil];

    observer = [DawSparkleObserver new];
    observer.onChange = ^(BOOL canCheck) {
        onCanCheckChanged (canCheck);
    };
    // NSKeyValueObservingOptionInitial で登録直後にも1回発火させ、メニューの初期状態を反映する
    [updaterController.updater addObserver: observer
                                forKeyPath: @"canCheckForUpdates"
                                   options: NSKeyValueObservingOptionInitial | NSKeyValueObservingOptionNew
                                   context: nil];
}

void checkForUpdates()
{
    if (updaterController != nil)
        [updaterController.updater checkForUpdates];
}

void shutdown()
{
    if (observer != nil)
    {
        [updaterController.updater removeObserver: observer forKeyPath: @"canCheckForUpdates"];
        // dispatch 済みの pending ブロックは observer を retain しているが、
        // onChange を nil にすればブロック内の nil ガードで no-op になる
        observer.onChange = nil;
        observer = nil;
    }
}

} // namespace SparkleBridge
