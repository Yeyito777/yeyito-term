/* Native macOS clipboard transport for Kitty's OSC 5522 protocol. */
#ifndef ST_MACOS_PASTEBOARD5522_H
#define ST_MACOS_PASTEBOARD5522_H

#import <AppKit/AppKit.h>

#include "../clipboard5522.h"

@interface STPasteboard5522 : NSObject

- (instancetype)initWithPasteboard:(NSPasteboard *)pasteboard
	writer:(Clip5522Write)writer context:(void *)context;
- (void)beginPasteEvent;
- (void)readRequest:(const Clip5522Request *)request;
- (void)invalidate;

@end

#endif /* ST_MACOS_PASTEBOARD5522_H */
