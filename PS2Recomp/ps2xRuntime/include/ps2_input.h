#ifndef PS2_INPUT_H
#define PS2_INPUT_H

#include <cstdint>
#include <string>

// Host input for the emulated DualShock 2: keyboard (raylib) and any game controller (SDL2 on
// desktop), mapped through the player's bindings in ps2Settings(). Polled once per host frame
// on the window thread; the game thread reads the latest snapshot.

enum Ps2PadAction : int
{
    kPadUp,
    kPadDown,
    kPadLeft,
    kPadRight,
    kPadCross,
    kPadCircle,
    kPadSquare,
    kPadTriangle,
    kPadL1,
    kPadR1,
    kPadL2,
    kPadR2,
    kPadL3,
    kPadR3,
    kPadStart,
    kPadSelect,
    kPadOpenMenu,   // not a PS2 button: opens the in-game menu
    kPadLightPress, // not a PS2 button: while held (or toggled on), pressed buttons report a light press
    kPadActionCount
};

// DualShock 2 pressure: the 12 pressure-sensitive buttons in pad-data order (bytes 8..19 of the pad data).
constexpr int kPadPressureCount = 12;

// A controller input code: 0 = none, kPadCodeButton + SDL button, or kPadCodeAxis + axis * 2 +
// (1 for the positive direction) for a stick or trigger pushed past half way.
constexpr int kPadCodeButton = 1;
constexpr int kPadCodeAxis = 64;

struct Ps2PadSnapshot
{
    uint16_t buttons = 0xFFFFu; // PS2 layout, active low
    uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
};

const char *ps2PadActionName(int action);
std::string ps2KeyName(int key);
std::string ps2ControllerInputName(int code);

void ps2InputInit();     // window thread, after InitWindow
void ps2InputShutdown(); // window thread
void ps2InputUpdate();   // window thread, once per host frame

// Two pad ports. A controller takes the lowest free port when it connects and keeps it until it is unplugged
// (further controllers are spares: one takes a freed port after 3 s, unless a newly connected pad takes it first).
// The keyboard is always port 0. PS2X_PAD_PORTS=1: the old single-player behaviour (every controller and the
// keyboard drive port 0, every port reads port 0 and reports connected).
constexpr int kPs2InputPorts = 2;
Ps2PadSnapshot ps2InputSnapshot();         // any thread; port 0
Ps2PadSnapshot ps2InputSnapshot(int port); // any thread
bool ps2InputPortConnected(int port);      // any thread; port 0 always (keyboard), port 1 with a controller on it
std::string ps2InputPortControllerName(int port); // window thread; the controller on that port, or ""
bool ps2InputPadSwapPossible(); // window thread; both ports have a controller
void ps2InputSwapPlayers();     // window thread; exchanges the controllers of ports 0 and 1 once (not saved)
bool ps2InputMenuPressed();        // the controller's "open menu" button went down this frame
int ps2InputCapturedControllerInput(); // for rebinding: a controller input that went down this frame, or 0
std::string ps2InputControllerNames(); // connected controllers, for the menu

// Controller buttons held now, by position and independent of the game bindings, for driving the menu (ImGui reads
// only raylib's own gamepad, which does not see SDL's controllers). D-pad and left stick are merged. All false when
// PS2X_INPUT_LIVE=0.
struct Ps2MenuPad
{
    bool up, down, left, right;
    bool confirm; // south face button (Cross / A)
    bool back;    // east face button (Circle / B)
    bool l1, r1;
};
Ps2MenuPad ps2InputMenuPad(); // window thread, after ps2InputUpdate

// Pressure bytes for the game (any thread). buttons = the active-low button word the game receives (scripted
// presses included); a pressed button reads its live level (255 for keys and digital buttons, the trigger travel
// for an analog trigger), lowered to the light-press level while the modifier is on (or forceLight, for scripts).
// Order: Right, Left, Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2.
void ps2InputPressure(uint16_t buttons, bool forceLight, uint8_t out[kPadPressureCount]); // port 0
void ps2InputPressure(int port, uint16_t buttons, bool forceLight, uint8_t out[kPadPressureCount]);
bool ps2InputLightPressOn(); // the modifier is on (held, or toggled on), port 0
// PS2X_PAD_PRESSURE=0: the pad never reports pressure mode (old behaviour: every press reads as full).
bool ps2PadPressureEnabled();

// Rumble: DualShock 2 small motor (on/off) and large motor (0..1), from the game thread. Each port drives the
// controller mapped to it (nothing when none is); with PS2X_PAD_PORTS=1 only port 0 counts and drives every controller.
void ps2InputSetRumble(int port, bool smallMotor, float largeMotor);
void ps2InputTestRumble();

#endif // PS2_INPUT_H
