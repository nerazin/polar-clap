// Polar Reconstruction CLAP — macOS Cocoa GUI.
// Objective-C++, manual retain/release (compiled without ARC so the plugin
// object can be held as a plain C++ pointer).
// All entry points are called on the host's main thread (CLAP GUI contract).
//
// The layout follows the original Polar panel: Algorithm, Delay Buffer, two
// Shifters, Dry Signal, Envelope, LFO and Filter. Amber marks a control's value,
// cyan an internal LFO/envelope depth, violet the host's (Bitwig's) modulation.

#include "polar_gui.hpp"
#include "../plugin.hpp"
#include "../params.hpp"

#import <AppKit/AppKit.h>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using polar_clap::PolarPlugin;
using polar_clap::kParams;
using polar_clap::PARAM_COUNT;
namespace pc = polar_clap;

static NSColor* RGB(int hex, CGFloat alpha = 1.0) {
    return [NSColor colorWithCalibratedRed:((hex >> 16) & 0xFF) / 255.0
                                      green:((hex >> 8) & 0xFF) / 255.0
                                       blue:(hex & 0xFF) / 255.0
                                      alpha:alpha];
}
static NSColor* CBackground() { return RGB(0x14161c); }
static NSColor* CPanel()      { return RGB(0x1d2028); }
static NSColor* CPanelEdge()  { return RGB(0x2b2f3b); }
static NSColor* CTrack()      { return RGB(0x353a48); }
static NSColor* CAmber()      { return RGB(0xf0b545); }
static NSColor* CCyan()       { return RGB(0x55c6e6); }
static NSColor* CMod()        { return RGB(0xb48cff); }
static NSColor* CText()       { return RGB(0xe6eaf2); }
static NSColor* CLabel()      { return RGB(0xa3abbb); }
static NSColor* CHeader()     { return RGB(0x8a93a6); }
static NSColor* CLedOff()     { return RGB(0x3a2a2a); }
static NSColor* CLedOn()      { return RGB(0xff5a4a); }

static NSFont* LabelFont() { return [NSFont systemFontOfSize:10.0 weight:NSFontWeightMedium]; }
static NSFont* ValueFont() { return [NSFont monospacedDigitSystemFontOfSize:9.5 weight:NSFontWeightRegular]; }
static NSFont* HeaderFont() { return [NSFont systemFontOfSize:10.5 weight:NSFontWeightBold]; }

static void DrawText(NSString* s, NSRect r, NSFont* f, NSColor* c, NSTextAlignment align = NSTextAlignmentCenter) {
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = align;
    style.lineBreakMode = NSLineBreakByClipping;
    [s drawInRect:r withAttributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: c,
                                     NSParagraphStyleAttributeName: style}];
    [style release];
}

static NSString* ValueText(uint32_t index, float v) {
    char buf[64];
    pc::formatValue(kParams[index], (double)v, buf, sizeof(buf));
    return [NSString stringWithUTF8String:buf];
}

// Polyline arc in a FLIPPED view: angles in degrees, 0 = right, 90 = down.
static void AppendArc(NSBezierPath* path, NSPoint c, double radius, double a0, double a1) {
    if (a0 > a1) std::swap(a0, a1);
    if (a1 - a0 < 1e-3) return;
    const double step = 3.0;
    [path moveToPoint:NSMakePoint(c.x + radius * cos(a0 * M_PI / 180.0), c.y + radius * sin(a0 * M_PI / 180.0))];
    for (double a = a0 + step; a < a1; a += step)
        [path lineToPoint:NSMakePoint(c.x + radius * cos(a * M_PI / 180.0), c.y + radius * sin(a * M_PI / 180.0))];
    [path lineToPoint:NSMakePoint(c.x + radius * cos(a1 * M_PI / 180.0), c.y + radius * sin(a1 * M_PI / 180.0))];
}

// ---------------------------------------------------------------- controls

enum ControlStyle { kKnob, kBigKnob, kDepthKnob, kSlider, kLed, kRadio, kSegments, kLockButton, kResetButton };

@interface PolarControl : NSView {
@public
    uint32_t mIndex;
    ControlStyle mStyle;
    NSString* mLabel;
    std::function<bool()> mDimmed;
    std::vector<std::string> mShortNames;  // segment labels when the enum names are too long
@protected
    PolarPlugin* mPlugin;
    std::shared_ptr<std::atomic<bool>> mAlive;
    float mShownBase, mShownValue;
    BOOL mShownDim;
    BOOL mDragging;
    double mDragValue;
    NSPoint mLastLocation;
}
- (id)initWithIndex:(uint32_t)index plugin:(PolarPlugin*)plugin frame:(NSRect)frame
              style:(ControlStyle)style label:(NSString*)label;
- (void)refreshFromPlugin;
@end

@implementation PolarControl

- (id)initWithIndex:(uint32_t)index plugin:(PolarPlugin*)plugin frame:(NSRect)frame
              style:(ControlStyle)style label:(NSString*)label {
    self = [super initWithFrame:frame];
    if (self) {
        mIndex = index;
        mStyle = style;
        mLabel = [label retain];
        mPlugin = plugin;
        mAlive = plugin->guiAlive;
        mShownBase = plugin->uiBase(index);
        mShownValue = plugin->uiValue(index);
        mShownDim = NO;
        mDragging = NO;
        self.toolTip = [NSString stringWithUTF8String:kParams[index].name];
    }
    return self;
}

- (void)dealloc {
    [mLabel release];
    [super dealloc];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)alive { return mAlive && mAlive->load(); }

- (void)refreshFromPlugin {
    if (![self alive]) return;
    const float b = mPlugin->uiBase(mIndex), v = mPlugin->uiValue(mIndex);
    const BOOL dim = mDimmed ? (mDimmed() ? YES : NO) : NO;
    if (mDragging) { if (dim != mShownDim) { mShownDim = dim; [self setNeedsDisplay:YES]; } return; }
    if (b != mShownBase || v != mShownValue || dim != mShownDim) {
        mShownBase = b; mShownValue = v; mShownDim = dim;
        [self setNeedsDisplay:YES];
    }
}

- (const pc::ParamDesc&)desc { return kParams[mIndex]; }

- (double)quantize:(double)v {
    const pc::ParamDesc& d = kParams[mIndex];
    v = std::fmin(std::fmax(v, d.min), d.max);
    if (d.kind == pc::kInt) v = std::floor(v + 0.5);
    if (d.kind == pc::kBool || d.kind == pc::kTrigger) v = v >= 0.5 ? 1 : 0;
    return v;
}

// Drag edits are one host gesture: begin on mouse down, values while dragging, end on mouse up.
- (void)setDuringGesture:(double)raw {
    if (![self alive]) return;
    const float v = (float)[self quantize:raw];
    if (v != mPlugin->uiBase(mIndex)) mPlugin->uiSetValue(mIndex, v);
    mShownBase = mPlugin->uiBase(mIndex);
    mShownValue = mShownBase + mPlugin->uiMod(mIndex);
    [self setNeedsDisplay:YES];
}
- (void)tap:(double)raw {
    if (![self alive]) return;
    mPlugin->uiTap(mIndex, (float)[self quantize:raw]);
    mShownBase = mPlugin->uiBase(mIndex);
    [self setNeedsDisplay:YES];
}

- (BOOL)isContinuous { return mStyle == kKnob || mStyle == kBigKnob || mStyle == kDepthKnob || mStyle == kSlider; }

- (void)mouseDown:(NSEvent*)event {
    if (![self alive]) return;
    const pc::ParamDesc& d = kParams[mIndex];
    if (mStyle == kResetButton) { mPlugin->uiTriggerReset(); mShownValue = 1; [self setNeedsDisplay:YES]; return; }
    if (mStyle == kLed || mStyle == kLockButton) { [self tap:mPlugin->uiBase(mIndex) >= 0.5f ? 0.0 : 1.0]; return; }
    if (mStyle == kRadio || mStyle == kSegments) {
        NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
        const int n = (int)(d.max - d.min) + 1;
        NSRect b = [self bounds];
        int k = mStyle == kRadio ? (int)(p.y / (b.size.height / n)) : (int)(p.x / (b.size.width / n));
        k = std::max(0, std::min(n - 1, k));
        [self tap:d.min + k];
        return;
    }
    mPlugin->uiBeginGesture(mIndex);
    if (event.clickCount >= 2) {  // double click: default value
        [self setDuringGesture:d.def];
        mPlugin->uiEndGesture(mIndex);
        return;
    }
    mDragging = YES;
    mDragValue = mPlugin->uiBase(mIndex);
    mLastLocation = [event locationInWindow];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!mDragging) return;
    NSPoint loc = [event locationInWindow];
    const double dy = loc.y - mLastLocation.y;  // window coordinates: up is positive
    mLastLocation = loc;
    const pc::ParamDesc& d = kParams[mIndex];
    double perPixel = (d.max - d.min) / (mStyle == kSlider ? 90.0 : 200.0);
    if (event.modifierFlags & NSEventModifierFlagShift) perPixel /= 8.0;
    mDragValue = std::fmin(std::fmax(mDragValue + dy * perPixel, d.min), d.max);
    [self setDuringGesture:mDragValue];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (mStyle == kResetButton) { mShownValue = 0; [self setNeedsDisplay:YES]; return; }
    if (!mDragging) return;
    mDragging = NO;
    if ([self alive]) mPlugin->uiEndGesture(mIndex);
}

- (void)scrollWheel:(NSEvent*)event {
    if (![self isContinuous] || ![self alive]) return;
    const pc::ParamDesc& d = kParams[mIndex];
    double delta = event.scrollingDeltaY * (d.max - d.min) / (event.hasPreciseScrollingDeltas ? 400.0 : 40.0);
    if (event.modifierFlags & NSEventModifierFlagShift) delta *= 0.125;
    if (delta == 0) return;
    mPlugin->uiBeginGesture(mIndex);
    [self setDuringGesture:mPlugin->uiBase(mIndex) + delta];
    mPlugin->uiEndGesture(mIndex);
}

// ---- drawing

- (CGFloat)fraction:(float)v {
    const pc::ParamDesc& d = kParams[mIndex];
    return std::fmin(std::fmax((v - d.min) / (d.max - d.min), 0.0), 1.0);
}

- (void)drawKnobAt:(NSPoint)c radius:(CGFloat)r color:(NSColor*)color {
    const pc::ParamDesc& d = kParams[mIndex];
    const double a0 = 135, span = 270;  // flipped: 135 deg = lower left, sweeps clockwise to lower right
    auto angle = [&](double f) { return a0 + span * f; };
    const double fb = [self fraction:mShownBase], fv = [self fraction:mShownValue];
    const double origin = (d.min < 0 && d.max > 0) ? [self fraction:0.0f] : 0.0;  // bipolar arcs start at zero

    NSBezierPath* track = [NSBezierPath bezierPath];
    track.lineWidth = 3.0; track.lineCapStyle = NSLineCapStyleRound;
    AppendArc(track, c, r, a0, a0 + span);
    [CTrack() setStroke]; [track stroke];

    NSBezierPath* arc = [NSBezierPath bezierPath];
    arc.lineWidth = 3.0; arc.lineCapStyle = NSLineCapStyleRound;
    AppendArc(arc, c, r, angle(origin), angle(fb));
    [color setStroke]; [arc stroke];

    if (std::fabs(fv - fb) > 1e-4) {  // host modulation: arc from base to modulated value, plus a dot
        NSBezierPath* mod = [NSBezierPath bezierPath];
        mod.lineWidth = 2.0; mod.lineCapStyle = NSLineCapStyleRound;
        AppendArc(mod, c, r + 4.0, angle(fb), angle(fv));
        [CMod() setStroke]; [mod stroke];
        const double am = angle(fv) * M_PI / 180.0;
        [CMod() setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x + (r + 4) * cos(am) - 2.5, c.y + (r + 4) * sin(am) - 2.5, 5, 5)] fill];
    }

    [RGB(0x2a2e39) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - r + 4, c.y - r + 4, 2 * r - 8, 2 * r - 8)] fill];
    const double an = angle(fb) * M_PI / 180.0;
    NSBezierPath* needle = [NSBezierPath bezierPath];
    needle.lineWidth = 2.0; needle.lineCapStyle = NSLineCapStyleRound;
    [needle moveToPoint:NSMakePoint(c.x + (r - 12) * cos(an), c.y + (r - 12) * sin(an))];
    [needle lineToPoint:NSMakePoint(c.x + (r - 5) * cos(an), c.y + (r - 5) * sin(an))];
    [CText() setStroke]; [needle stroke];
}

- (void)drawLedIn:(NSRect)r on:(BOOL)on modulated:(BOOL)modulated {
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:2.5 yRadius:2.5];
    [(on ? CLedOn() : CLedOff()) setFill]; [p fill];
    if (on) { [[NSColor colorWithCalibratedWhite:1 alpha:0.25] setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 3, 3) xRadius:1.5 yRadius:1.5] fill]; }
    if (modulated) { p.lineWidth = 1.5; [CMod() setStroke]; [p stroke]; }
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const pc::ParamDesc& d = kParams[mIndex];
    NSRect b = [self bounds];
    const CGFloat W = b.size.width, H = b.size.height;
    [NSGraphicsContext saveGraphicsState];
    if (mShownDim) CGContextSetAlpha([[NSGraphicsContext currentContext] CGContext], 0.35);
    switch (mStyle) {
    case kKnob: case kBigKnob: case kDepthKnob: {
        const CGFloat r = mStyle == kBigKnob ? 21 : 14;
        const NSPoint c = NSMakePoint(W / 2, r + 6);
        [self drawKnobAt:c radius:r color:mStyle == kDepthKnob ? CCyan() : CAmber()];
        DrawText(mLabel, NSMakeRect(0, 2 * r + 12, W, 14), LabelFont(), CLabel());
        DrawText(ValueText(mIndex, mDragging ? mShownBase : mShownValue), NSMakeRect(-6, 2 * r + 25, W + 12, 13), ValueFont(), CText());
        break;
    }
    case kSlider: {
        const NSRect track = NSMakeRect(W / 2 - 2, 4, 4, H - 34);
        [CTrack() setFill]; [[NSBezierPath bezierPathWithRoundedRect:track xRadius:2 yRadius:2] fill];
        const CGFloat yb = NSMaxY(track) - track.size.height * [self fraction:mShownBase];
        const CGFloat yv = NSMaxY(track) - track.size.height * [self fraction:mShownValue];
        [CAmber() setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(W / 2 - 2, yb, 4, NSMaxY(track) - yb) xRadius:2 yRadius:2] fill];
        if (std::fabs(yv - yb) > 0.5) { [CMod() setFill]; [NSBezierPath fillRect:NSMakeRect(W / 2 + 4, std::fmin(yb, yv), 2, std::fabs(yv - yb))]; }
        [CText() setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(W / 2 - 8, yb - 3, 16, 6) xRadius:2 yRadius:2] fill];
        DrawText(mLabel, NSMakeRect(0, H - 28, W, 14), LabelFont(), CLabel());
        DrawText(ValueText(mIndex, mShownValue), NSMakeRect(-8, H - 15, W + 16, 13), ValueFont(), CText());
        break;
    }
    case kLed: {
        const BOOL on = mShownBase >= 0.5f, eff = mShownValue >= 0.5f;
        [self drawLedIn:NSMakeRect(2, (H - 12) / 2, 18, 12) on:eff modulated:(on != eff)];
        DrawText(mLabel, NSMakeRect(26, (H - 14) / 2, W - 26, 14), LabelFont(), CLabel(), NSTextAlignmentLeft);
        break;
    }
    case kLockButton: {
        const BOOL eff = mShownValue >= 0.5f;
        const NSPoint c = NSMakePoint(W / 2, 30);
        NSBezierPath* ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 24, c.y - 24, 48, 48)];
        [RGB(0x2a2e39) setFill]; [ring fill];
        ring.lineWidth = 3; [(eff ? CAmber() : CTrack()) setStroke]; [ring stroke];
        [(eff ? CAmber() : RGB(0x454b5b)) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 13, c.y - 13, 26, 26)] fill];
        if ((mShownBase >= 0.5f) != eff) { NSBezierPath* m = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 28, c.y - 28, 56, 56)]; m.lineWidth = 2; [CMod() setStroke]; [m stroke]; }
        DrawText(mLabel, NSMakeRect(0, 60, W, 14), LabelFont(), CLabel());
        DrawText(eff ? @"Locked" : @"Live", NSMakeRect(0, 73, W, 13), ValueFont(), eff ? CAmber() : CText());
        break;
    }
    case kResetButton: {
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 1, 1) xRadius:4 yRadius:4];
        [(mShownValue >= 0.5f ? CAmber() : RGB(0x2a2e39)) setFill]; [p fill];
        p.lineWidth = 1; [CPanelEdge() setStroke]; [p stroke];
        DrawText(mLabel, NSMakeRect(0, (H - 14) / 2, W, 14), LabelFont(), mShownValue >= 0.5f ? RGB(0x14161c) : CText());
        break;
    }
    case kRadio: case kSegments: {
        const int n = (int)(d.max - d.min) + 1;
        const int base = (int)std::lround(mShownBase - d.min), eff = (int)std::lround(mShownValue - d.min);
        for (int k = 0; k < n; ++k) {
            NSString* name = k < (int)mShortNames.size() ? [NSString stringWithUTF8String:mShortNames[k].c_str()]
                           : d.enums ? [NSString stringWithUTF8String:d.enums[k]] : [NSString stringWithFormat:@"%d", k];
            if (mStyle == kRadio) {
                const CGFloat rowH = H / n, y = k * rowH;
                [self drawLedIn:NSMakeRect(2, y + (rowH - 11) / 2, 16, 11) on:k == base modulated:(k == eff && eff != base)];
                DrawText([name uppercaseString], NSMakeRect(24, y + (rowH - 14) / 2, W - 24, 14), LabelFont(),
                         k == eff ? CText() : CLabel(), NSTextAlignmentLeft);
            } else {
                const CGFloat segW = W / n;
                NSRect r = NSMakeRect(k * segW + 1, 1, segW - 2, H - 2);
                NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:3 yRadius:3];
                [(k == base ? CAmber() : RGB(0x2a2e39)) setFill]; [p fill];
                if (k == eff && eff != base) { p.lineWidth = 1.5; [CMod() setStroke]; [p stroke]; }
                DrawText(name, NSMakeRect(r.origin.x, (H - 13) / 2, r.size.width, 13), [NSFont systemFontOfSize:9.5 weight:NSFontWeightMedium],
                         k == base ? RGB(0x14161c) : CText());
            }
        }
        break;
    }
    }
    [NSGraphicsContext restoreGraphicsState];
}

@end

// ------------------------------------------------------------------- panel

struct Section { NSRect r; NSString* title; };

@interface PolarPanel : NSView {
@public
    std::vector<Section> mSections;
    NSString* mStatus;
}
@end

@implementation PolarPanel
- (BOOL)isFlipped { return YES; }
- (void)dealloc {
    for (auto& s : mSections) [s.title release];
    [mStatus release];
    [super dealloc];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = [self bounds];
    [CBackground() setFill];
    NSRectFill(b);
    // Header strip
    DrawText(@"POLAR", NSMakeRect(16, 11, 120, 26), [NSFont systemFontOfSize:22 weight:NSFontWeightHeavy], CAmber(), NSTextAlignmentLeft);
    DrawText(@"DUAL PITCH SHIFTER  ·  reconstruction", NSMakeRect(112, 20, 300, 14), [NSFont systemFontOfSize:9.5 weight:NSFontWeightSemibold], CHeader(), NSTextAlignmentLeft);
    if (mStatus) DrawText(mStatus, NSMakeRect(b.size.width - 430, 20, 300, 14), ValueFont(), CHeader(), NSTextAlignmentRight);
    [CPanelEdge() setFill];
    NSRectFill(NSMakeRect(0, 47, b.size.width, 1));
    for (auto& s : mSections) {
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:s.r xRadius:7 yRadius:7];
        [CPanel() setFill]; [p fill];
        p.lineWidth = 1; [CPanelEdge() setStroke]; [p stroke];
        DrawText(s.title, NSMakeRect(s.r.origin.x + 12, s.r.origin.y + 7, s.r.size.width - 24, 14), HeaderFont(), CHeader(), NSTextAlignmentLeft);
    }
}
- (void)setStatus:(NSString*)s {
    if ([s isEqualToString:mStatus]) return;
    [mStatus release]; mStatus = [s retain];
    [self setNeedsDisplayInRect:NSMakeRect(self.bounds.size.width - 440, 0, 440, 46)];
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
- (NSView*)view;
@end

@implementation PolarGuiController

- (id)initWithPlugin:(PolarPlugin*)plugin {
    self = [super init];
    if (self) {
        mPlugin = plugin;
        mControls = [[NSMutableArray alloc] init];
        [self buildLayout];
        mTimer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 target:self selector:@selector(tick:)
                                                 userInfo:nil repeats:YES] retain];
    }
    return self;
}

- (PolarControl*)add:(pc::ParamId)id style:(ControlStyle)style x:(CGFloat)x y:(CGFloat)y
                  w:(CGFloat)w h:(CGFloat)h label:(NSString*)label {
    PolarControl* c = [[PolarControl alloc] initWithIndex:(uint32_t)id plugin:mPlugin
                                                    frame:NSMakeRect(x, y, w, h) style:style label:label];
    [mPanel addSubview:c];
    [mControls addObject:c];
    [c release];
    return c;
}
- (void)section:(NSString*)title x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w h:(CGFloat)h {
    mPanel->mSections.push_back({NSMakeRect(x, y, w, h), [title retain]});
}

- (void)buildLayout {
    const CGFloat W = 1000, H = 478, top = 58;
    mPanel = [[PolarPanel alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    PolarPlugin* pl = mPlugin;
    auto eff = [pl](pc::ParamId id) { return pl->uiValue((uint32_t)id); };
    auto fftMode = [eff] { return eff(pc::P_ALGORITHM) < 1.5f; };
    const CGFloat KW = 58, KH = 72, BKH = 86;  // knob cell / big knob cell

    // ALGORITHM + OUTPUT
    [self section:@"ALGORITHM" x:12 y:top w:128 h:276];
    PolarControl* speed = [self add:pc::P_SPEED style:kRadio x:24 y:top + 28 w:104 h:60 label:@""];
    speed->mDimmed = [fftMode] { return !fftMode(); };
    [self add:pc::P_ALGORITHM style:kRadio x:24 y:top + 100 w:104 h:84 label:@""];
    PolarControl* length = [self add:pc::P_LOOP style:kKnob x:47 y:top + 196 w:KW h:KH label:@"Length"];
    length->mDimmed = fftMode;
    [self section:@"OUTPUT" x:12 y:top + 284 w:128 h:126];
    [self add:pc::P_OUTPUT_DB style:kKnob x:16 y:top + 310 w:KW h:KH label:@"Output"];
    [self add:pc::P_RESET style:kResetButton x:74 y:top + 320 w:58 h:24 label:@"Reset"];
    [self add:pc::P_BYPASS style:kLed x:72 y:top + 354 w:66 h:18 label:@"Bypass"];

    // DELAY BUFFER + DRY SIGNAL
    const CGFloat bx = 150;
    [self section:@"DELAY BUFFER" x:bx y:top w:210 h:236];
    [self add:pc::P_LOCK style:kLockButton x:bx + 10 y:top + 30 w:76 h:90 label:@"Lock"];
    [self add:pc::P_POSITION style:kBigKnob x:bx + 108 y:top + 28 w:80 h:BKH label:@"Delay"];
    [self add:pc::P_STEREO style:kLed x:bx + 14 y:top + 140 w:80 h:18 label:@"Stereo"];
    [self add:pc::P_POSITION_LFO style:kDepthKnob x:bx + 92 y:top + 132 w:KW h:KH label:@"LFO"];
    [self add:pc::P_POSITION_ENV style:kDepthKnob x:bx + 148 y:top + 132 w:KW h:KH label:@"Env"];
    [self section:@"DRY SIGNAL" x:bx y:top + 244 w:210 h:166];
    [self add:pc::P_DRY_DELAY style:kKnob x:bx + 30 y:top + 286 w:KW h:KH label:@"Delay"];
    [self add:pc::P_DRY style:kKnob x:bx + 120 y:top + 286 w:KW h:KH label:@"Volume"];

    // SHIFTERS
    auto shifter = [&](int n, CGFloat x) {
        const bool two = n == 2;
        [self section:two ? @"      SHIFTER 2" : @"SHIFTER 1" x:x y:top w:178 h:410];
        auto voice2Off = [eff] { return eff(pc::P_ENABLE_2) < 0.5f; };
        auto dimmed = [two, voice2Off] { return two && voice2Off(); };
        if (two) [self add:pc::P_ENABLE_2 style:kLed x:x + 8 y:top + 5 w:24 h:18 label:@""];
        PolarControl* c;
        c = [self add:two ? pc::P_PITCH_2 : pc::P_PITCH_1 style:kBigKnob x:x + 12 y:top + 30 w:80 h:BKH label:@"Shift"]; c->mDimmed = dimmed;
        c = [self add:two ? pc::P_TUNE_2 : pc::P_TUNE_1 style:kKnob x:x + 102 y:top + 38 w:KW h:KH label:@"Fine"]; c->mDimmed = dimmed;
        c = [self add:two ? pc::P_FEEDBACK_2 : pc::P_FEEDBACK_1 style:kKnob x:x + 22 y:top + 126 w:KW h:KH label:@"F.Back"]; c->mDimmed = dimmed;
        if (two) { c = [self add:pc::P_DELAY_2 style:kKnob x:x + 98 y:top + 126 w:KW h:KH label:@"Delay"]; c->mDimmed = dimmed; }
        c = [self add:two ? pc::P_PITCH_LFO_2 : pc::P_PITCH_LFO_1 style:kDepthKnob x:x + 22 y:top + 214 w:KW h:KH label:@"LFO"]; c->mDimmed = dimmed;
        c = [self add:two ? pc::P_PITCH_ENV_2 : pc::P_PITCH_ENV_1 style:kDepthKnob x:x + 98 y:top + 214 w:KW h:KH label:@"Env"]; c->mDimmed = dimmed;
        c = [self add:two ? pc::P_PAN_2 : pc::P_PAN_1 style:kKnob x:x + 22 y:top + 312 w:KW h:KH label:@"Pan"]; c->mDimmed = dimmed;
        c = [self add:two ? pc::P_LEVEL_2 : pc::P_LEVEL_1 style:kKnob x:x + 98 y:top + 312 w:KW h:KH label:@"Volume"]; c->mDimmed = dimmed;
    };
    shifter(1, 370);
    shifter(2, 558);

    // ENVELOPE / LFO / FILTER
    const CGFloat rx = 746, rw = 242;
    [self section:@"ENVELOPE" x:rx y:top w:rw h:124];
    [self add:pc::P_GATE style:kLed x:rx + 120 y:top + 5 w:60 h:18 label:@"Gate"];
    const pc::ParamId adsr[4] = {pc::P_ATTACK, pc::P_DECAY, pc::P_SUSTAIN, pc::P_RELEASE};
    NSString* adsrName[4] = {@"A", @"D", @"S", @"R"};
    for (int i = 0; i < 4; ++i) [self add:adsr[i] style:kSlider x:rx + 6 + i * 44 y:top + 26 w:44 h:94 label:adsrName[i]];
    [self add:pc::P_ENV_TO_AMP style:kDepthKnob x:rx + 184 y:top + 34 w:54 h:KH label:@"Amp"];

    [self section:@"LFO" x:rx y:top + 132 w:rw h:130];
    auto synced = [eff] { return eff(pc::P_LFO_SYNC) >= 0.5f; };
    PolarControl* rate = [self add:pc::P_LFO_RATE style:kKnob x:rx + 4 y:top + 154 w:KW h:KH label:@"Rate"];
    rate->mDimmed = synced;
    PolarControl* div = [self add:pc::P_DIVISION style:kKnob x:rx + 62 y:top + 154 w:KW h:KH label:@"Cyc/Beat"];
    div->mDimmed = [synced] { return !synced(); };
    [self add:pc::P_LFO_PHASE style:kKnob x:rx + 120 y:top + 154 w:KW h:KH label:@"Phase"];
    [self add:pc::P_PAN_LFO style:kDepthKnob x:rx + 178 y:top + 154 w:KW h:KH label:@"Pan"];
    PolarControl* wave = [self add:pc::P_LFO_WAVE style:kSegments x:rx + 10 y:top + 230 w:150 h:22 label:@""];
    wave->mShortNames = {"Sine", "Tri", "Saw", "Sqr"};
    [self add:pc::P_LFO_SYNC style:kLed x:rx + 170 y:top + 232 w:70 h:18 label:@"Sync"];

    [self section:@"      FILTER" x:rx y:top + 270 w:rw h:140];
    [self add:pc::P_FILTER_ON style:kLed x:rx + 8 y:top + 275 w:24 h:18 label:@""];
    auto filterOff = [eff] { return eff(pc::P_FILTER_ON) < 0.5f; };
    PolarControl* c;
    c = [self add:pc::P_CUTOFF style:kBigKnob x:rx + 2 y:top + 292 w:78 h:BKH label:@"Freq"]; c->mDimmed = filterOff;
    c = [self add:pc::P_RESONANCE style:kKnob x:rx + 80 y:top + 300 w:54 h:KH label:@"Res"]; c->mDimmed = filterOff;
    c = [self add:pc::P_FILTER_LFO style:kDepthKnob x:rx + 134 y:top + 300 w:54 h:KH label:@"LFO"]; c->mDimmed = filterOff;
    c = [self add:pc::P_FILTER_ENV style:kDepthKnob x:rx + 188 y:top + 300 w:52 h:KH label:@"Env"]; c->mDimmed = filterOff;
    c = [self add:pc::P_FILTER_MODE style:kSegments x:rx + 12 y:top + 382 w:218 h:20 label:@""]; c->mDimmed = filterOff;

    mSize = NSMakeSize(W, H);
    [self tick:nil];
}

- (NSSize)size { return mSize; }
- (NSView*)view { return mPanel; }

- (void)attachToView:(NSView*)parent {
    if (mPanel.superview == parent) return;
    [mPanel removeFromSuperview];
    [parent addSubview:mPanel];
}

- (void)setVisible:(BOOL)visible { mPanel.hidden = !visible; }

- (void)tick:(NSTimer*)timer {
    (void)timer;
    for (PolarControl* c in mControls) [c refreshFromPlugin];
    const float tempo = mPlugin->uiTempo();
    [mPanel setStatus:[NSString stringWithFormat:@"LFO tempo %.1f BPM   ·   latency 64 samples", tempo]];
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

bool snapshot(PolarPlugin& p, const char* pngPath) {
    PolarGuiController* c = controllerOf(p);
    if (!c || !pngPath) return false;
    NSView* v = [c view];
    [c tick:nil];
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:[NSString stringWithUTF8String:pngPath] atomically:YES];
}

} // namespace gui
} // namespace polar_clap
