// Polar Reconstruction CLAP — macOS Cocoa GUI.
// Objective-C++, manual retain/release (compiled without ARC so the plugin
// object can be held as a plain C++ pointer).
// All entry points are called on the host's main thread (CLAP GUI contract).

#include "polar_gui.hpp"
#include "../plugin.hpp"
#include "../params.hpp"

#import <AppKit/AppKit.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using polar_clap::PolarPlugin;
using polar_clap::kParams;
using polar_clap::PARAM_COUNT;

static NSColor* RGB(int hex, float alpha = 1.0f) {
    return [NSColor colorWithCalibratedRed:((hex >> 16) & 0xFF) / 255.0f
                                      green:((hex >> 8) & 0xFF) / 255.0f
                                       blue:(hex & 0xFF) / 255.0f
                                      alpha:alpha];
}

static NSColor* CBackground() { return RGB(0x171921); }
static const CGFloat marginH = 12.0;
static NSColor* CPanel()      { return RGB(0x1f222c); }
static NSColor* CTrack()      { return RGB(0x3a3f4e); }
static NSColor* CValue()      { return RGB(0x5fb0ff); }
static NSColor* CMod()        { return RGB(0xc9a6ff); }
static NSColor* CNeedle()     { return RGB(0xe9edf5); }
static NSColor* CText()       { return RGB(0xdfe5f0); }
static NSColor* CLabel()      { return RGB(0x99a2b4); }
static NSColor* CHeader()     { return RGB(0x7f889b); }
static NSColor* CAccent()     { return RGB(0x5fb0ff); }
static NSColor* CWarn()       { return RGB(0xff7a6b); }

static void DrawCenteredString(NSString* s, NSRect r, NSFont* f, NSColor* c) {
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = NSTextAlignmentCenter;
    NSDictionary* attrs = @{
        NSFontAttributeName: f,
        NSForegroundColorAttributeName: c,
        NSParagraphStyleAttributeName: style,
    };
    [s drawInRect:r withAttributes:attrs];
    [style release];
}

// ---------------------------------------------------------------- controls

@interface PolarControl : NSView {
@protected
    uint32_t mIndex;
    PolarPlugin* mPlugin;
    std::shared_ptr<std::atomic<bool>> mAlive;  // plugin lifetime token
    float mShownValue;
    float mShownMod;
    BOOL mDragging;
    double mDragValue;
    NSPoint mLastLocation;
}
- (id)initWithIndex:(uint32_t)index plugin:(PolarPlugin*)plugin frame:(NSRect)frame;
- (void)refreshFromPlugin;
@end

static NSString* ValueText(uint32_t index, float v) {
    char buf[64];
    polar_clap::formatValue(polar_clap::kParams[index], (double)v, buf, sizeof(buf));
    return [NSString stringWithUTF8String:buf];
}

@implementation PolarControl

- (id)initWithIndex:(uint32_t)index plugin:(PolarPlugin*)plugin frame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        mIndex = index;
        mPlugin = plugin;
        mAlive = plugin->guiAlive;
        mShownValue = plugin->uiValue(index);
        mShownMod = plugin->uiMod(index);
        mDragging = NO;
    }
    return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)isFlipped { return NO; }

- (void)refreshFromPlugin {
    if (mDragging) return;
    float v = mPlugin->uiValue(mIndex);
    float m = mPlugin->uiMod(mIndex);
    if (v != mShownValue || m != mShownMod) {
        mShownValue = v;
        mShownMod = m;
        [self setNeedsDisplay:YES];
    }
}

- (const polar_clap::ParamDesc&)desc { return kParams[mIndex]; }

- (double)commitValue:(double)raw {
    const polar_clap::ParamDesc& d = kParams[mIndex];
    double v = raw;
    if (v < d.min) v = d.min;
    if (v > d.max) v = d.max;
    if (d.kind == polar_clap::kInt) v = floor(v + 0.5);
    if (d.kind == polar_clap::kBool || d.kind == polar_clap::kTrigger) v = v >= 0.5 ? 1 : 0;
    return v;
}

- (void)commit:(double)raw {
    if (!mAlive || !mAlive->load()) return;  // plugin gone
    double v = [self commitValue:raw];
    mPlugin->uiSetValue(mIndex, (float)v);
    mShownValue = mPlugin->uiValue(mIndex);
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)event {
    const polar_clap::ParamDesc& d = kParams[mIndex];
    if (d.kind == polar_clap::kTrigger) {
        [self commit:1.0];
        [self performSelector:@selector(releaseTrigger:) withObject:nil afterDelay:0.18];
        return;
    }
    if (d.kind == polar_clap::kBool) {
        [self commit:(mPlugin->uiValue(mIndex) >= 0.5f) ? 0.0 : 1.0];
        return;
    }
    if (event.clickCount >= 2) {
        mDragValue = d.def;
        [self commit:d.def];
        return;
    }
    mDragging = YES;
    mDragValue = mPlugin->uiBase(mIndex);
    mLastLocation = [event locationInWindow];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!mDragging) return;
    NSPoint loc = [event locationInWindow];
    double dy = mLastLocation.y - loc.y;
    mLastLocation = loc;
    const polar_clap::ParamDesc& d = kParams[mIndex];
    double range = d.max - d.min;
    double perPixel = range / 220.0;
    if (event.modifierFlags & NSEventModifierFlagShift) perPixel /= 8.0;
    mDragValue += dy * perPixel;
    [self commit:mDragValue];
}

- (void)mouseUp:(NSEvent*)event { mDragging = NO; }

- (void)scrollWheel:(NSEvent*)event {
    const polar_clap::ParamDesc& d = kParams[mIndex];
    if (d.kind == polar_clap::kBool || d.kind == polar_clap::kTrigger) return;
    double range = d.max - d.min;
    double delta = event.scrollingDeltaY * range / 260.0;
    if (event.modifierFlags & NSEventModifierFlagShift) delta *= 0.15;
    [self commit:mPlugin->uiBase(mIndex) + delta];
}

- (void)releaseTrigger:(id)sender {
    (void)sender;
    if (!mAlive || !mAlive->load()) return;  // plugin gone during the delay
    if (kParams[mIndex].kind == polar_clap::kTrigger) [self commit:0.0];
}

@end

// ------------------------------------------------------------------- knob

@interface PolarKnob : PolarControl
@end

// Appends a polyline arc from angle a0 down to a1 (degrees, decreasing =
// clockwise on screen; non-flipped view: 0 = right, 90 = up). Built as a
// polyline so the result does not depend on AppKit arc-direction conventions.
static void AppendArc(NSBezierPath* path, NSPoint c, double radius, double a0, double a1) {
    if (!(a0 > a1)) return;  // empty span
    const double step = 3.0;
    double a = a0;
    [path moveToPoint:NSMakePoint(c.x + radius * cos(a * M_PI / 180.0),
                                  c.y + radius * sin(a * M_PI / 180.0))];
    for (a = a0 - step; a > a1; a -= step) {
        double rad = a * M_PI / 180.0;
        [path lineToPoint:NSMakePoint(c.x + radius * cos(rad), c.y + radius * sin(rad))];
    }
    double rad = a1 * M_PI / 180.0;
    [path lineToPoint:NSMakePoint(c.x + radius * cos(rad), c.y + radius * sin(rad))];
}

@implementation PolarKnob

- (void)drawRect:(NSRect)rect {
    (void)rect;
    const polar_clap::ParamDesc& d = kParams[mIndex];
    NSRect b = [self bounds];
    float cx = b.size.width * 0.5f;
    float cy = b.size.height - 22.0f;
    float r = 13.0f;
    NSPoint center = NSMakePoint(cx, cy);

    double vmin = (double)d.min, vmax = (double)d.max;
    double frac = (mShownValue - vmin) / (vmax - vmin);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    // track: 270 degrees, from 7 o'clock (225 deg) through 12 to 5 o'clock (-45 deg)
    NSBezierPath* track = [NSBezierPath bezierPath];
    track.lineWidth = 3.0;
    AppendArc(track, center, r, 225, -45);
    [CTrack() setStroke];
    [track stroke];

    // modulation ring (outside the track), spanning value..value+mod
    if (fabsf(mShownMod) > 1e-4f && d.kind == polar_clap::kFloat) {
        double modFrac = mShownMod / (vmax - vmin);
        double av = 225 - 270.0 * frac;
        double am = 225 - 270.0 * (frac + modFrac);
        if (av > 225) av = 225; else if (av < -45) av = -45;
        if (am > 225) am = 225; else if (am < -45) am = -45;
        NSBezierPath* modArc = [NSBezierPath bezierPath];
        modArc.lineWidth = 2.0;
        AppendArc(modArc, center, r + 3.5f, fmax(av, am), fmin(av, am));
        [CMod() setStroke];
        [modArc stroke];
    }

    // value arc
    if (frac > 0.001) {
        NSBezierPath* arc = [NSBezierPath bezierPath];
        arc.lineWidth = 3.0;
        AppendArc(arc, center, r, 225, 225 - 270.0 * frac);
        [CValue() setStroke];
        [arc stroke];
    }

    // needle
    double ang = (225 - 270.0 * frac) * M_PI / 180.0;
    NSBezierPath* needle = [NSBezierPath bezierPath];
    needle.lineWidth = 1.6;
    [needle moveToPoint:NSMakePoint(cx, cy)];
    [needle lineToPoint:NSMakePoint(cx + (r - 1.0) * cos(ang), cy + (r - 1.0) * sin(ang))];
    [CNeedle() setStroke];
    [needle stroke];

    [[NSColor colorWithCalibratedWhite:1.0f alpha:0.06f] setFill];
    NSBezierPath* hub = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cx - 1.8, cy - 1.8, 3.6, 3.6)];
    [hub fill];

    // label + value
    DrawCenteredString([NSString stringWithUTF8String:d.label],
                       NSMakeRect(0, 20, b.size.width, 12),
                       [NSFont labelFontOfSize:8.0], CLabel());
    DrawCenteredString(ValueText(mIndex, mShownValue),
                       NSMakeRect(0, 6, b.size.width, 13),
                       [NSFont monospacedDigitSystemFontOfSize:8.0 weight:NSFontWeightRegular],
                       CText());
}

@end

// ----------------------------------------------------------------- switch

@interface PolarSwitch : PolarControl
@end

@implementation PolarSwitch

- (void)drawRect:(NSRect)rect {
    (void)rect;
    const polar_clap::ParamDesc& d = kParams[mIndex];
    NSRect b = [self bounds];
    BOOL on = mShownValue >= 0.5;
    BOOL trigger = d.kind == polar_clap::kTrigger;

    NSRect box = NSMakeRect(4, b.size.height - 44.0, b.size.width - 8, 22);
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:box xRadius:5 yRadius:5];
    if (on) {
        [[trigger ? CWarn() : CAccent() colorWithAlphaComponent:0.85f] setFill];
        [path fill];
    } else {
        [RGB(0x2a2e3a) setFill];
        [path fill];
    }
    [[NSColor colorWithCalibratedWhite:1.0f alpha:on ? 0.0f : 0.05f] setFill];
    [path fill];

    NSString* stateText = on ? @"On" : @"Off";
    if (trigger) stateText = on ? @"Resetting" : @"Reset";
    DrawCenteredString(stateText,
                       NSMakeRect(box.origin.x, box.origin.y + 4, box.size.width, 14),
                       [NSFont boldSystemFontOfSize:8.5],
                       on ? RGB(0x10131a) : CText());

    DrawCenteredString([NSString stringWithUTF8String:d.label],
                       NSMakeRect(0, 20, b.size.width, 12),
                       [NSFont labelFontOfSize:8.0], CLabel());
    DrawCenteredString(ValueText(mIndex, mShownValue),
                       NSMakeRect(0, 6, b.size.width, 13),
                       [NSFont monospacedDigitSystemFontOfSize:8.0 weight:NSFontWeightRegular],
                       CText());
}

@end

// ------------------------------------------------------------------- panel

@interface PolarPanel : NSView {
    std::vector<std::pair<NSRect, NSString*>> mSections;
    NSRect mTipsRect;
    NSString* mTips[8];
    int mTipCount;
}
- (void)setSections:(std::vector<std::pair<NSRect, NSString*>>*)sections;
- (void)setTips:(NSString* const*)tips count:(int)count rect:(NSRect)r;
@end

@implementation PolarPanel

- (void)setSections:(std::vector<std::pair<NSRect, NSString*>>*)sections {
    for (auto& s : mSections) [s.second release];
    mSections = *sections;
    for (auto& s : mSections) [s.second retain];
}

- (void)setTips:(NSString* const*)tips count:(int)count rect:(NSRect)r {
    for (int i = 0; i < mTipCount; ++i) [mTips[i] release];
    mTipCount = count > 8 ? 8 : count;
    for (int i = 0; i < mTipCount; ++i) { mTips[i] = [tips[i] retain]; }
    mTipsRect = r;
}

- (void)dealloc {
    for (auto& s : mSections) [s.second release];
    for (int i = 0; i < mTipCount; ++i) [mTips[i] release];
    [super dealloc];
}

- (BOOL)isFlipped { return NO; }

- (void)drawRect:(NSRect)rect {
    (void)rect;
    NSRect b = [self bounds];
    [CBackground() setFill];
    [NSBezierPath fillRect:b];

    // title strip
    NSDictionary* titleAttrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:11.5],
        NSForegroundColorAttributeName: CText(),
    };
    NSDictionary* subAttrs = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:8.5 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: CHeader(),
    };
    [@"POLAR RECONSTRUCTION" drawAtPoint:NSMakePoint(14, b.size.height - 24)
                              withAttributes:titleAttrs];
    [@"granular / spectral / periodic    CLAP 0.2 - 64-frame engine"
        drawAtPoint:NSMakePoint(b.size.width - 320, b.size.height - 22)
        withAttributes:subAttrs];
    [RGB(0x2a2e3a) setFill];
    [NSBezierPath fillRect:NSMakeRect(0, b.size.height - 36, b.size.width, 1)];

    NSDictionary* hAttrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:8.0],
        NSForegroundColorAttributeName: CHeader(),
    };
    NSDictionary* tAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:9.0],
        NSForegroundColorAttributeName: CLabel(),
    };

    for (auto& s : mSections) {
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:s.first xRadius:6 yRadius:6];
        [CPanel() setFill];
        [p fill];
        [s.second drawAtPoint:NSMakePoint(s.first.origin.x + marginH,
                                          s.first.origin.y + s.first.size.height - 14)
                withAttributes:hAttrs];
    }

    if (mTipCount > 0 && mTipsRect.size.width > 0) {
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:mTipsRect xRadius:6 yRadius:6];
        [CPanel() setFill];
        [p fill];
        [@"NOTES" drawAtPoint:NSMakePoint(mTipsRect.origin.x + marginH,
                                          mTipsRect.origin.y + mTipsRect.size.height - 14)
                withAttributes:hAttrs];
        for (int i = 0; i < mTipCount; ++i) {
            [mTips[i] drawAtPoint:NSMakePoint(mTipsRect.origin.x + marginH,
                                              mTipsRect.origin.y + mTipsRect.size.height - 32 - i * 14)
                    withAttributes:tAttrs];
        }
    }
}

@end

// ------------------------------------------------------------- controller

@interface PolarGuiController : NSObject {
    PolarPlugin* mPlugin;
    PolarPanel* mPanel;
    NSMutableArray* mControls;
    NSTimer* mTimer;
    NSSize mSize;
}
- (id)initWithPlugin:(PolarPlugin*)plugin;
- (void)attachToView:(NSView*)parent;
- (NSSize)size;
- (void)setVisible:(BOOL)visible;
- (void)shutdown;
@end

static NSString* kTips[] = {
    @"Capture: keep audio flowing, then engage Lock.",
    @"Pitch a single voice or enable Voice 2 for dual pitch.",
    @"Reset Capture clears the buffer and all running state.",
    @"Float knobs accept Bitwig modulators; stepped ones do not.",
    nil
};

@implementation PolarGuiController

- (id)initWithPlugin:(PolarPlugin*)plugin {
    self = [super init];
    if (self) {
        mPlugin = plugin;
        mControls = [[NSMutableArray alloc] init];
        mPanel = [[PolarPanel alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        [self buildLayout];
        // retain our own reference: the runloop holds one, released by invalidate
        mTimer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                                   target:self
                                                 selector:@selector(tick:)
                                                 userInfo:nil
                                                  repeats:YES] retain];
    }
    return self;
}

- (void)buildLayout {
    const CGFloat cellW = 56, cellH = 70;
    const CGFloat colW = 480;
    const CGFloat gap = 8;
    const CGFloat headerH = 20;

    // Collect sections in table order; a module seen twice (Reset joins
    // Algorithm) merges into the first occurrence.
    std::vector<std::pair<std::string, std::vector<uint32_t>>> sections;
    for (int i = 0; i < PARAM_COUNT; ++i) {
        const char* m = kParams[i].module;
        int found = -1;
        for (size_t s = 0; s < sections.size(); ++s)
            if (sections[s].first == m) { found = (int)s; break; }
        if (found < 0) {
            sections.push_back({m, {}});
            sections.back().second.push_back((uint32_t)i);
        } else {
            sections[found].second.push_back((uint32_t)i);
        }
    }

    int cellsPerRow = (int)floor((colW - 2 * marginH) / cellW);
    auto sectionHeight = [&](size_t n) -> CGFloat {
        int rows = (int)ceil((double)n / cellsPerRow);
        return headerH + 4 + rows * cellH + (rows - 1) * 2 + 8;
    };

    // Fixed two-column assignment keeps related groups together.
    std::vector<std::string> leftModules = {"Algorithm", "Grain", "Voice 1", "Voice 2", "Filter"};
    std::vector<const std::pair<std::string, std::vector<uint32_t>>*> left, right;
    CGFloat leftH = 0, rightH = 0;
    for (auto& s : sections) {
        CGFloat h = sectionHeight(s.second.size());
        bool isLeft = false;
        for (auto& lm : leftModules)
            if (s.first == lm) { isLeft = true; break; }
        if (isLeft) { left.push_back(&s); leftH += h + gap; }
        else { right.push_back(&s); rightH += h + gap; }
    }
    int tipLines = 0;
    while (kTips[tipLines]) ++tipLines;
    CGFloat tipsH = 26 + tipLines * 14 + 8;

    CGFloat contentH = fmax(leftH, rightH + tipsH + gap);
    CGFloat totalW = marginH + colW + marginH + colW + marginH;
    CGFloat titleH = 42;
    CGFloat totalH = titleH + contentH + marginH;

    mPanel.frame = NSMakeRect(0, 0, totalW, totalH);
    std::vector<std::pair<NSRect, NSString*>> sectionRects;

    struct ColumnSpec { std::vector<const std::pair<std::string, std::vector<uint32_t>>*>* list; CGFloat x; };
    ColumnSpec cols[2] = {{&left, marginH}, {&right, marginH + colW + marginH}};

    for (int c = 0; c < 2; ++c) {
        CGFloat x = cols[c].x;
        // non-flipped coordinates: walk down from the top
        CGFloat top = totalH - titleH;
        for (auto* s : *cols[c].list) {
            CGFloat h = sectionHeight(s->second.size());
            CGFloat y = top - h;
            NSRect sec = NSMakeRect(x, y, colW, h);
            NSString* title = [[[[NSString stringWithUTF8String:s->first.c_str()]
                stringByReplacingOccurrencesOfString:@" " withString:@""] uppercaseString] retain];
            sectionRects.push_back({sec, title});

            int n = (int)s->second.size();
            for (int k = 0; k < n; ++k) {
                uint32_t index = s->second[k];
                int row = k / cellsPerRow, col = k % cellsPerRow;
                CGFloat cx = x + marginH + col * cellW;
                CGFloat cy = y + h - 8 - headerH - 4 - (row + 1) * cellH - row * 2;
                NSRect frame = NSMakeRect(cx, cy, cellW, cellH);
                PolarControl* ctl = nil;
                if (kParams[index].kind == polar_clap::kBool ||
                    kParams[index].kind == polar_clap::kTrigger)
                    ctl = [[PolarSwitch alloc] initWithIndex:index plugin:mPlugin frame:frame];
                else
                    ctl = [[PolarKnob alloc] initWithIndex:index plugin:mPlugin frame:frame];
                [mPanel addSubview:ctl];
                [ctl release];
                [mControls addObject:ctl];
            }
            top = y - gap;
        }
    }

    // tips panel in the right column, below its last section
    {
        CGFloat x = cols[1].x;
        CGFloat top = totalH - titleH;
        CGFloat used = 0;
        for (auto* s : right) used += sectionHeight(s->second.size()) + gap;
        CGFloat y = top - used - tipsH;
        if (y > marginH) {
            NSRect sec = NSMakeRect(x, y, colW, tipsH);
            [mPanel setTips:kTips count:tipLines rect:sec];
        }
    }

    [mPanel setSections:&sectionRects];
    mSize = NSMakeSize(totalW, totalH);
}

- (NSSize)size { return mSize; }

- (void)attachToView:(NSView*)parent {
    if (mPanel.superview == parent) return;
    [mPanel removeFromSuperview];
    [parent addSubview:mPanel];
}

- (void)setVisible:(BOOL)visible { mPanel.hidden = !visible; }

- (void)tick:(NSTimer*)timer {
    (void)timer;
    for (PolarControl* c in mControls) [c refreshFromPlugin];
}

- (void)shutdown {
    [mTimer invalidate];
    [mTimer release];
    mTimer = nil;
    [mPanel removeFromSuperview];
    [mControls release];
    mControls = nil;
    [mPanel release];
    mPanel = nil;
}

- (void)dealloc {
    if (mTimer) [mTimer invalidate];
    [mTimer release];
    [mControls release];
    [mPanel release];
    [super dealloc];
}

@end

// ------------------------------------------------------------- gui facade

namespace polar_clap {
namespace gui {

static PolarGuiController* controllerOf(PolarPlugin& p) {
    return static_cast<PolarGuiController*>(p.guiState);
}

bool create(PolarPlugin& p) {
    if (p.guiState) destroy(p);
    PolarGuiController* c = [[PolarGuiController alloc] initWithPlugin:&p];
    if (!c) return false;
    p.guiState = c;  // retained by alloc
    return true;
}

void destroy(PolarPlugin& p) {
    PolarGuiController* c = controllerOf(p);
    if (!c) return;
    [c shutdown];
    [c release];
    p.guiState = nullptr;
}

bool getSize(PolarPlugin& p, uint32_t* w, uint32_t* h) {
    PolarGuiController* c = controllerOf(p);
    if (!c || !w || !h) return false;
    NSSize s = [c size];
    *w = (uint32_t)s.width;
    *h = (uint32_t)s.height;
    return true;
}

bool setSize(PolarPlugin& p, uint32_t w, uint32_t h) {
    PolarGuiController* c = controllerOf(p);
    if (!c) return false;
    NSSize s = [c size];
    return (uint32_t)s.width == w && (uint32_t)s.height == h;
}

bool setParent(PolarPlugin& p, const void* nsView) {
    PolarGuiController* c = controllerOf(p);
    if (!c || !nsView) return false;
    NSView* parent = static_cast<NSView*>(const_cast<void*>(nsView));
    if (![parent isKindOfClass:[NSView class]]) return false;
    [c attachToView:parent];
    return true;
}

bool show(PolarPlugin& p, bool shown) {
    PolarGuiController* c = controllerOf(p);
    if (!c) return false;
    [c setVisible:shown ? YES : NO];
    return true;
}

} // namespace gui
} // namespace polar_clap
