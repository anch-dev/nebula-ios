// SPDX-License-Identifier: GPL-3.0-only
// The iOS app shell.
//
// It hosts the runtime the way NebulaRuntime.exe's launcher does on Windows:
// it applies the product environment (runtime-env.json), points the runtime
// at the game content and the saves in the app's Documents folder, and runs
// galaxy_runtime_main on a game thread. The runtime draws into this app's
// CAMetalLayer (host_platform::set_window) and reads input that the shell
// publishes through host_platform:
// - touch controls, as an Xbox-layout pad on slot 0 (docs/KEYBOARD_CONTROLS.md
//   has what each button does), with touches elsewhere on the picture as
//   the pointer (the mouse, on Windows);
// - game controllers (GCController extended gamepads), as Xbox pads; the
//   touch controls hide while one is connected;
// - a hardware keyboard, as the Windows keyboard.
// The game modules come in the app (Frameworks/, see
// tools/ios/build-modules.ps1); the content and saves live in Documents,
// which the Files app shows under On My iPhone > Nebula.

#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "galaxy/host_platform.h"
#include "galaxy/runtime_main.h"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace hp = galaxy::host_platform;

namespace {

// XInput buttons (XINPUT_GAMEPAD_*), the layout host_platform pads use.
enum : std::uint16_t {
    kPadUp = 0x0001,
    kPadDown = 0x0002,
    kPadLeft = 0x0004,
    kPadRight = 0x0008,
    kPadStart = 0x0010,
    kPadBack = 0x0020,
    kPadLeftThumb = 0x0040,
    kPadRightThumb = 0x0080,
    kPadLeftShoulder = 0x0100,
    kPadRightShoulder = 0x0200,
    kPadA = 0x1000,
    kPadB = 0x2000,
    kPadX = 0x4000,
    kPadY = 0x8000,
};

// Win32 virtual keys.
constexpr int kVkF1 = 0x70;

NSString* documents_dir() {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

bool is_dir(NSString* path) {
    BOOL dir = NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && dir;
}

// The product environment (installer/runtime-env.json, as the Windows
// launcher applies it), unless already set.
void apply_runtime_environment() {
    NSString* path = [[NSBundle mainBundle] pathForResource:@"runtime-env" ofType:@"json"];
    NSData* data = path != nil ? [NSData dataWithContentsOfFile:path] : nil;
    if (data == nil) return;
    id entries = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![entries isKindOfClass:[NSArray class]]) return;
    for (id entry in (NSArray*)entries) {
        if (![entry isKindOfClass:[NSDictionary class]]) continue;
        NSString* name = entry[@"name"];
        NSString* value = entry[@"value"];
        if ([name isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]]) {
            setenv(name.UTF8String, value.UTF8String, 0);
        }
    }
}

// stdout/stderr to Documents/nebula.log (the previous run's log is kept as
// nebula.previous.log), for reports.
void redirect_log() {
    NSString* log = [documents_dir() stringByAppendingPathComponent:@"nebula.log"];
    NSString* previous = [documents_dir() stringByAppendingPathComponent:@"nebula.previous.log"];
    NSFileManager* files = [NSFileManager defaultManager];
    [files removeItemAtPath:previous error:nil];
    [files moveItemAtPath:log toPath:previous error:nil];
    std::freopen(log.fileSystemRepresentation, "w", stdout);
    std::freopen(log.fileSystemRepresentation, "a", stderr);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

// ---------------------------------------------------------------------------
// Input state shared by the touch controls, controllers and keyboard.
// ---------------------------------------------------------------------------
std::mutex g_input_mutex;
hp::PadState g_touch_pad;
bool g_touch_enabled = true;

void publish_touch_pad() {
    hp::PadState state;
    {
        const std::lock_guard<std::mutex> lock(g_input_mutex);
        state = g_touch_pad;
        state.connected = g_touch_enabled;
        if (!g_touch_enabled) state = hp::PadState{};
    }
    hp::set_pad(0u, state);
}

std::int16_t axis(float value) {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

// ---------------------------------------------------------------------------
// The game view: a CAMetalLayer the Metal renderer presents into.
// ---------------------------------------------------------------------------
@interface NBGameView : UIView
@end

@implementation NBGameView
+ (Class)layerClass {
    return [CAMetalLayer class];
}
@end

// ---------------------------------------------------------------------------
// Touch controls.
// ---------------------------------------------------------------------------
@interface NBControl : NSObject
@property(nonatomic) NSString* title;
@property(nonatomic) std::uint16_t buttons;   // pad buttons while held
@property(nonatomic) bool trigger;            // left trigger while held
@property(nonatomic) int key;                 // virtual key while held (0: none)
@property(nonatomic) CGPoint anchor;          // centre, as fractions of the view
@property(nonatomic) CGFloat radius;          // points
@property(nonatomic) CAShapeLayer* shape;
@property(nonatomic) CATextLayer* label;
@property(nonatomic) NSUInteger holds;
@end

@implementation NBControl
@end

@interface NBTouchControls : UIView
@property(nonatomic, copy) void (^pointerHandler)(BOOL inside, CGPoint point);
@end

@implementation NBTouchControls {
    NSMutableArray<NBControl*>* _controls;
    NSMapTable<UITouch*, id>* _owners;  // touch -> NBControl, the stick (NSNull) or the pointer (@"pointer")
    UITouch* _stickTouch;
    CGPoint _stickOrigin;
    CAShapeLayer* _stickBase;
    CAShapeLayer* _stickKnob;
}

static const CGFloat kStickRadius = 64.0;

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    _owners = [NSMapTable strongToStrongObjectsMapTable];
    _controls = [NSMutableArray array];
    // Right hand: the face buttons. Top: the small buttons.
    [self addControl:@"A" buttons:kPadA at:CGPointMake(0.90, 0.76) radius:38];
    [self addControl:@"★" buttons:kPadLeftShoulder at:CGPointMake(0.80, 0.88) radius:30];  // Star Bits: Wii B
    [self addControl:@"Spin" buttons:kPadX at:CGPointMake(0.80, 0.62) radius:30];
    [self addControl:@"B" buttons:kPadB at:CGPointMake(0.92, 0.52) radius:26];
    NBControl* z = [self addControl:@"Z" buttons:0 at:CGPointMake(0.06, 0.38) radius:28];
    z.trigger = true;
    [self addControl:@"C" buttons:kPadLeftThumb at:CGPointMake(0.15, 0.38) radius:24];
    [self addControl:@"−" buttons:kPadBack at:CGPointMake(0.38, 0.07) radius:20];
    [self addControl:@"+" buttons:kPadStart at:CGPointMake(0.62, 0.07) radius:20];
    [self addControl:@"⌖" buttons:kPadRightThumb at:CGPointMake(0.94, 0.30) radius:22];  // pointer recenter
    NBControl* home = [self addControl:@"⌂" buttons:0 at:CGPointMake(0.50, 0.07) radius:20];
    home.key = 'H';
    NBControl* one = [self addControl:@"1" buttons:0 at:CGPointMake(0.04, 0.07) radius:18];
    one.key = '1';
    NBControl* two = [self addControl:@"2" buttons:0 at:CGPointMake(0.10, 0.07) radius:18];
    two.key = '2';
    NBControl* settings = [self addControl:@"⚙" buttons:0 at:CGPointMake(0.96, 0.07) radius:18];
    settings.key = kVkF1;
    // D-pad (camera), above the stick.
    [self addControl:@"▲" buttons:kPadUp at:CGPointMake(0.27, 0.56) radius:16];
    [self addControl:@"▼" buttons:kPadDown at:CGPointMake(0.27, 0.74) radius:16];
    [self addControl:@"◀" buttons:kPadLeft at:CGPointMake(0.23, 0.65) radius:16];
    [self addControl:@"▶" buttons:kPadRight at:CGPointMake(0.31, 0.65) radius:16];

    _stickBase = [self circleLayer:kStickRadius alpha:0.18];
    _stickKnob = [self circleLayer:kStickRadius * 0.45 alpha:0.35];
    _stickBase.hidden = YES;
    _stickKnob.hidden = YES;
    [self.layer addSublayer:_stickBase];
    [self.layer addSublayer:_stickKnob];
    return self;
}

- (CAShapeLayer*)circleLayer:(CGFloat)radius alpha:(CGFloat)alpha {
    CAShapeLayer* layer = [CAShapeLayer layer];
    layer.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-radius, -radius, radius * 2, radius * 2)].CGPath;
    layer.fillColor = [UIColor colorWithWhite:1.0 alpha:alpha].CGColor;
    layer.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.45].CGColor;
    layer.lineWidth = 1.5;
    return layer;
}

- (NBControl*)addControl:(NSString*)title buttons:(std::uint16_t)buttons at:(CGPoint)anchor radius:(CGFloat)radius {
    NBControl* control = [NBControl new];
    control.title = title;
    control.buttons = buttons;
    control.anchor = anchor;
    control.radius = radius;
    control.shape = [self circleLayer:radius alpha:0.16];
    control.label = [CATextLayer layer];
    control.label.string = title;
    control.label.fontSize = radius * 0.7;
    control.label.alignmentMode = kCAAlignmentCenter;
    control.label.foregroundColor = [UIColor colorWithWhite:1.0 alpha:0.8].CGColor;
    control.label.contentsScale = UIScreen.mainScreen.scale;
    control.label.frame = CGRectMake(-radius, -radius * 0.45, radius * 2, radius);
    [control.shape addSublayer:control.label];
    [self.layer addSublayer:control.shape];
    [_controls addObject:control];
    return control;
}

- (CGPoint)centerOf:(NBControl*)control {
    const CGRect bounds = self.bounds;
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat width = bounds.size.width - safe.left - safe.right;
    const CGFloat height = bounds.size.height - safe.top - safe.bottom;
    return CGPointMake(safe.left + control.anchor.x * width, safe.top + control.anchor.y * height);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (NBControl* control in _controls) control.shape.position = [self centerOf:control];
    [CATransaction commit];
}

- (NBControl*)controlAt:(CGPoint)point {
    for (NBControl* control in _controls) {
        const CGPoint center = [self centerOf:control];
        const CGFloat reach = control.radius * 1.25;
        if (std::hypot(point.x - center.x, point.y - center.y) <= reach) return control;
    }
    return nil;
}

- (void)setControl:(NBControl*)control held:(bool)held {
    if (held) {
        if (control.holds++ > 0) return;
    } else {
        if (control.holds == 0 || --control.holds > 0) return;
    }
    control.shape.fillColor = [UIColor colorWithWhite:1.0 alpha:held ? 0.45 : 0.16].CGColor;
    if (control.key != 0) {
        hp::set_key(control.key, held);
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(g_input_mutex);
        if (held) {
            g_touch_pad.buttons |= control.buttons;
        } else {
            g_touch_pad.buttons &= static_cast<std::uint16_t>(~control.buttons);
        }
        if (control.trigger) g_touch_pad.left_trigger = held ? 255u : 0u;
    }
    publish_touch_pad();
}

- (void)moveStick:(CGPoint)point {
    CGFloat dx = point.x - _stickOrigin.x;
    CGFloat dy = point.y - _stickOrigin.y;
    const CGFloat length = std::hypot(dx, dy);
    if (length > kStickRadius) {
        dx *= kStickRadius / length;
        dy *= kStickRadius / length;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _stickKnob.position = CGPointMake(_stickOrigin.x + dx, _stickOrigin.y + dy);
    [CATransaction commit];
    {
        const std::lock_guard<std::mutex> lock(g_input_mutex);
        g_touch_pad.left_x = axis(static_cast<float>(dx / kStickRadius));
        g_touch_pad.left_y = axis(static_cast<float>(-dy / kStickRadius));  // XInput: up is +
    }
    publish_touch_pad();
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const CGPoint point = [touch locationInView:self];
        if (NBControl* control = [self controlAt:point]) {
            [_owners setObject:control forKey:touch];
            [self setControl:control held:true];
        } else if (_stickTouch == nil && point.x < self.bounds.size.width * 0.4 &&
                   point.y > self.bounds.size.height * 0.45) {
            _stickTouch = touch;
            _stickOrigin = point;
            [_owners setObject:[NSNull null] forKey:touch];
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            _stickBase.position = point;
            _stickBase.hidden = NO;
            _stickKnob.hidden = NO;
            [CATransaction commit];
            [self moveStick:point];
        } else {
            [_owners setObject:@"pointer" forKey:touch];
            if (self.pointerHandler) self.pointerHandler(YES, point);
        }
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        id owner = [_owners objectForKey:touch];
        const CGPoint point = [touch locationInView:self];
        if (owner == [NSNull null]) {
            [self moveStick:point];
        } else if ([owner isKindOfClass:[NSString class]]) {
            if (self.pointerHandler) self.pointerHandler(YES, point);
        } else if ([owner isKindOfClass:[NBControl class]]) {
            // Slide between buttons, as on a pad.
            NBControl* now = [self controlAt:point];
            if (now != nil && now != owner) {
                [self setControl:owner held:false];
                [self setControl:now held:true];
                [_owners setObject:now forKey:touch];
            }
        }
    }
}

- (void)endTouches:(NSSet<UITouch*>*)touches {
    for (UITouch* touch in touches) {
        id owner = [_owners objectForKey:touch];
        if (owner == nil) continue;
        [_owners removeObjectForKey:touch];
        if (owner == [NSNull null]) {
            _stickTouch = nil;
            _stickBase.hidden = YES;
            _stickKnob.hidden = YES;
            {
                const std::lock_guard<std::mutex> lock(g_input_mutex);
                g_touch_pad.left_x = 0;
                g_touch_pad.left_y = 0;
            }
            publish_touch_pad();
        } else if ([owner isKindOfClass:[NBControl class]]) {
            [self setControl:owner held:false];
        }
        // The pointer stays where it was last touched, as a mouse would.
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches];
}
@end

// ---------------------------------------------------------------------------
// The view controller: view, input devices, the game thread.
// ---------------------------------------------------------------------------
@interface NBGameViewController : UIViewController
@end

static void* game_thread(void* context);

@implementation NBGameViewController {
    NBGameView* _gameView;
    NBTouchControls* _touch;
    UILabel* _message;
    bool _started;
    bool _active;
    NSMutableArray<GCController*>* _controllers;
}

- (void)loadView {
    _gameView = [[NBGameView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    _gameView.backgroundColor = UIColor.blackColor;
    _gameView.contentScaleFactor = UIScreen.mainScreen.nativeScale;
    self.view = _gameView;

    _touch = [[NBTouchControls alloc] initWithFrame:_gameView.bounds];
    _touch.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    __weak NBGameViewController* weak = self;
    _touch.pointerHandler = ^(BOOL inside, CGPoint point) {
        NBGameViewController* strong = weak;
        if (strong == nil) return;
        const CGFloat scale = strong->_gameView.contentScaleFactor;
        hp::set_pointer(inside, static_cast<int>(point.x * scale), static_cast<int>(point.y * scale));
    };
    [_gameView addSubview:_touch];

    _message = [[UILabel alloc] initWithFrame:CGRectInset(_gameView.bounds, 40, 40)];
    _message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _message.numberOfLines = 0;
    _message.textAlignment = NSTextAlignmentCenter;
    _message.textColor = UIColor.whiteColor;
    _message.font = [UIFont systemFontOfSize:17];
    _message.hidden = YES;
    [_gameView addSubview:_message];

    _controllers = [NSMutableArray array];
    _active = true;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(controllersChanged) name:GCControllerDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(controllersChanged) name:GCControllerDidDisconnectNotification object:nil];
    [center addObserver:self selector:@selector(keyboardConnected:) name:GCKeyboardDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(becameActive) name:UIApplicationDidBecomeActiveNotification object:nil];
    [center addObserver:self selector:@selector(resignedActive) name:UIApplicationWillResignActiveNotification object:nil];
    [self controllersChanged];
    if (GCKeyboard.coalescedKeyboard != nil) [self attachKeyboard:GCKeyboard.coalescedKeyboard];
    publish_touch_pad();
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (void)publishWindow {
    CAMetalLayer* layer = (CAMetalLayer*)_gameView.layer;
    const CGFloat scale = _gameView.contentScaleFactor;
    const CGSize size = _gameView.bounds.size;
    const int width = std::max(1, static_cast<int>(std::lround(size.width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(size.height * scale)));
    UIScreen* screen = _gameView.window.screen ?: UIScreen.mainScreen;
    const CGSize native = screen.nativeBounds.size;
    // nativeBounds is portrait; the app is landscape.
    hp::set_screen(static_cast<int>(std::max(native.width, native.height)),
                   static_cast<int>(std::min(native.width, native.height)),
                   static_cast<unsigned>(screen.maximumFramesPerSecond), static_cast<float>(screen.nativeScale));
    hp::set_window((__bridge void*)layer, width, height, true, _active);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self publishWindow];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self publishWindow];
    if (!_started) {
        _started = true;
        [self startGame];
    }
}

- (void)becameActive {
    _active = true;
    [self publishWindow];
}

- (void)resignedActive {
    _active = false;
    [self publishWindow];
}

- (void)showMessage:(NSString*)text {
    _message.text = text;
    _message.hidden = NO;
    _touch.hidden = YES;
}

// ----- The game -------------------------------------------------------------
- (void)startGame {
    NSString* documents = documents_dir();
    NSString* bundle = [NSBundle mainBundle].bundlePath;
    NSString* module = [bundle stringByAppendingPathComponent:@"Frameworks/RMGE01_game.framework/RMGE01_game"];
    NSString* content = [bundle stringByAppendingPathComponent:@"content"];
    if (!is_dir(content)) content = [documents stringByAppendingPathComponent:@"content"];
    // A placeholder so the folder shows up in the Files app.
    [@"Put the game content folder here, named \"content\".\n"
        writeToFile:[documents stringByAppendingPathComponent:@"README.txt"]
         atomically:YES encoding:NSUTF8StringEncoding error:nil];

    if (![[NSFileManager defaultManager] fileExistsAtPath:module]) {
        [self showMessage:@"This copy of Nebula has no game modules.\n\n"
                           "Build them from your copy of Super Mario Galaxy with build-modules.ps1 "
                           "(tools/ios in the Nebula iOS repository), which adds them to the IPA, "
                           "then install that IPA with SideStore or AltStore."];
        return;
    }
    if (!is_dir(content)) {
        [self showMessage:@"Game content not found.\n\n"
                           "Copy the \"content\" folder that build-modules.ps1 made to "
                           "On My iPhone › Nebula with the Files app, then open Nebula again."];
        return;
    }

    apply_runtime_environment();
    NSString* saves = [documents stringByAppendingPathComponent:@"saves"];
    [[NSFileManager defaultManager] createDirectoryAtPath:saves withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("GALAXY_NAND_ROOT", saves.fileSystemRepresentation, 0);
    setenv("GALAXY_RENDER_CONFIG_PATH",
           [documents stringByAppendingPathComponent:@"config.ini"].fileSystemRepresentation, 0);
    // The launcher's job on Windows; there is no launcher here.
    setenv("GALAXY_SKIP_LAUNCHER", "1", 1);
    chdir(documents.fileSystemRepresentation);
    redirect_log();

    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryAmbient error:nil];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    [UIApplication sharedApplication].idleTimerDisabled = YES;

    auto* arguments = new std::vector<std::string>{
        [NSBundle mainBundle].executablePath.fileSystemRepresentation,
        content.fileSystemRepresentation,
        module.fileSystemRepresentation,
    };
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // NebulaRuntime.exe's main thread has a 256 MiB stack (CMakeLists.txt).
    pthread_attr_setstacksize(&attributes, 256u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, &game_thread, arguments) != 0) {
        [self showMessage:@"Could not start the game thread."];
    }
    pthread_attr_destroy(&attributes);
}

static void* game_thread(void* context) {
    pthread_setname_np("Nebula game");
    auto* arguments = static_cast<std::vector<std::string>*>(context);
    std::vector<char*> argv;
    for (std::string& argument : *arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const int code = galaxy_runtime_main(static_cast<int>(argv.size() - 1u), argv.data());
    std::fflush(stdout);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController* root = UIApplication.sharedApplication.keyWindow.rootViewController;
        if ([root isKindOfClass:[NBGameViewController class]]) {
            [(NBGameViewController*)root
                showMessage:[NSString stringWithFormat:@"Nebula stopped (exit code %d).\n\n"
                                                        "The log is in Files › On My iPhone › Nebula › nebula.log.",
                                                       code]];
        }
    });
    return nullptr;
}

// ----- Controllers -----------------------------------------------------------
- (void)controllersChanged {
    [_controllers removeAllObjects];
    for (GCController* controller in GCController.controllers) {
        if (controller.extendedGamepad != nil && _controllers.count < 4) [_controllers addObject:controller];
    }
    // A controller takes over slot 0 and the touch controls hide.
    {
        const std::lock_guard<std::mutex> lock(g_input_mutex);
        g_touch_enabled = _controllers.count == 0;
    }
    _touch.hidden = _controllers.count > 0 || !_message.hidden;
    if (_controllers.count > 0) {
        for (unsigned slot = 0; slot < 4u; ++slot) hp::set_pad(slot, hp::PadState{});
    } else {
        for (unsigned slot = 1; slot < 4u; ++slot) hp::set_pad(slot, hp::PadState{});
        publish_touch_pad();
    }
    unsigned slot = 0;
    for (GCController* controller in _controllers) {
        const unsigned this_slot = slot++;
        controller.playerIndex = static_cast<GCControllerPlayerIndex>(this_slot);
        controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* pad, GCControllerElement*) {
            hp::set_pad(this_slot, [NBGameViewController padState:pad]);
        };
        hp::set_pad(this_slot, [NBGameViewController padState:controller.extendedGamepad]);
    }
}

+ (hp::PadState)padState:(GCExtendedGamepad*)pad {
    hp::PadState state;
    state.connected = true;
    std::uint16_t buttons = 0;
    // Xbox layout by position: Nintendo controllers' face buttons are
    // remapped by the system to match.
    if (pad.buttonA.pressed) buttons |= kPadA;
    if (pad.buttonB.pressed) buttons |= kPadB;
    if (pad.buttonX.pressed) buttons |= kPadX;
    if (pad.buttonY.pressed) buttons |= kPadY;
    if (pad.leftShoulder.pressed) buttons |= kPadLeftShoulder;
    if (pad.rightShoulder.pressed) buttons |= kPadRightShoulder;
    if (pad.leftThumbstickButton.pressed) buttons |= kPadLeftThumb;
    if (pad.rightThumbstickButton.pressed) buttons |= kPadRightThumb;
    if (pad.buttonMenu.pressed) buttons |= kPadStart;
    if (pad.buttonOptions.pressed) buttons |= kPadBack;
    if (pad.dpad.up.pressed) buttons |= kPadUp;
    if (pad.dpad.down.pressed) buttons |= kPadDown;
    if (pad.dpad.left.pressed) buttons |= kPadLeft;
    if (pad.dpad.right.pressed) buttons |= kPadRight;
    state.buttons = buttons;
    state.left_trigger = static_cast<std::uint8_t>(std::lround(pad.leftTrigger.value * 255.0f));
    state.right_trigger = static_cast<std::uint8_t>(std::lround(pad.rightTrigger.value * 255.0f));
    state.left_x = axis(pad.leftThumbstick.xAxis.value);
    state.left_y = axis(pad.leftThumbstick.yAxis.value);
    state.right_x = axis(pad.rightThumbstick.xAxis.value);
    state.right_y = axis(pad.rightThumbstick.yAxis.value);
    return state;
}

// ----- Keyboard -------------------------------------------------------------
- (void)keyboardConnected:(NSNotification*)notification {
    [self attachKeyboard:notification.object];
}

static int virtual_key(GCKeyCode code) {
    const auto usage = static_cast<long>(code);
    if (usage >= 0x04 && usage <= 0x1D) return 'A' + static_cast<int>(usage - 0x04);
    if (usage >= 0x1E && usage <= 0x26) return '1' + static_cast<int>(usage - 0x1E);
    if (usage >= 0x3A && usage <= 0x45) return 0x70 + static_cast<int>(usage - 0x3A);  // F1-F12
    switch (usage) {
    case 0x27: return '0';
    case 0x28: return 0x0D;  // Return
    case 0x29: return 0x1B;  // Escape
    case 0x2A: return 0x08;  // Backspace
    case 0x2B: return 0x09;  // Tab
    case 0x2C: return 0x20;  // Space
    case 0x2D: return 0xBD;  // -
    case 0x2E: return 0xBB;  // =
    case 0x4F: return 0x27;  // Right
    case 0x50: return 0x25;  // Left
    case 0x51: return 0x28;  // Down
    case 0x52: return 0x26;  // Up
    case 0xE0: return 0xA2;  // Left Control
    case 0xE1: return 0xA0;  // Left Shift
    case 0xE2: return 0xA4;  // Left Alt
    case 0xE4: return 0xA3;  // Right Control
    case 0xE5: return 0xA1;  // Right Shift
    case 0xE6: return 0xA5;  // Right Alt
    default: return 0;
    }
}

- (void)attachKeyboard:(GCKeyboard*)keyboard {
    keyboard.keyboardInput.keyChangedHandler = ^(GCKeyboardInput*, GCControllerButtonInput*, GCKeyCode code, BOOL pressed) {
        const int key = virtual_key(code);
        if (key == 0) return;
        hp::set_key(key, pressed);
        // The generic modifier keys too, as Windows reports both.
        if (key == 0xA0 || key == 0xA1) hp::set_key(0x10, pressed);
        if (key == 0xA2 || key == 0xA3) hp::set_key(0x11, pressed);
        if (key == 0xA4 || key == 0xA5) hp::set_key(0x12, pressed);
    };
}
@end

// ---------------------------------------------------------------------------
@interface NBAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic) UIWindow* window;
@end

@implementation NBAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [NBGameViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([NBAppDelegate class]));
    }
}
