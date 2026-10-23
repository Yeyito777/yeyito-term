/* Run the real helper main in a short-lived child. Resolve its target only to
 * the parent test process; no frontmost app, focus, or user input is touched. */
#import <AppKit/AppKit.h>

@interface SymbolTestWorkspace : NSObject
+ (instancetype)sharedWorkspace;
- (NSRunningApplication *)frontmostApplication;
@end
@implementation SymbolTestWorkspace
+ (instancetype)sharedWorkspace { return [self new]; }
- (NSRunningApplication *)frontmostApplication
{
	return [NSRunningApplication runningApplicationWithProcessIdentifier:getppid()];
}
@end

#define NSWorkspace SymbolTestWorkspace
#include "../scripts/macos-type-symbol.m"
