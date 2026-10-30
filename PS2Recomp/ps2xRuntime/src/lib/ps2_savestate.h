#pragma once
// Save states: a snapshot of the whole emulated machine (EE memory and kernel, DMA/VIF/GIF/timers, VU0/VU1, GS, IOP,
// SPU2), written and read at a safe point of the EE scheduler (between guest dispatches, render threads drained).
//
// File layout: Header, then sections. Each section is a 4-byte tag, a 4-byte version, a 64-bit byte count and its
// payload, so a reader can check it is reading the section it expects and that it consumed exactly the payload.
// Loading refuses a file from another game build (ELF hash) or another state format.
//
// Subsystems save/load through ps2_state::Access (declared friend in the classes that hold machine state), so the
// state code lives in .cpp files and adding a field does not touch headers every generated file includes.

#include "ps2x/state_io.h"

#include <string>

class PS2Runtime;

namespace ps2_state
{
    // Friend of the classes that hold machine state; all save/load code is in ps2_savestate.cpp.
    struct Access;
}

namespace ps2_state
{
    // Called once at startup (names the slot files and lets requests wake an idle EE executor).
    void attach(PS2Runtime &runtime, const std::string &gameName);

    // ---- Requests (any thread: hotkeys on the window thread, test switches) ------------------------------------------
    void requestSave(int slot);
    void requestLoad(int slot);
    void requestSaveTo(const std::string &path);
    void requestLoadFrom(const std::string &path);
    int slot();
    void setSlot(int slot);
    // Selects the previous/next slot and shows what it holds.
    void stepSlot(int delta);
    std::string slotPath(int slot);
    bool pending();
    // Last result ("Saved state 3", "Cannot save: ..."), and how many ms ago it was set (for the on-screen message).
    std::string lastMessage(double &ageSeconds);

    // ---- EE executor, at the scheduler loop top (no guest code on the host stack) ---------------------------------------
    // True when it saved or loaded (the scheduler restarts its iteration); false when the request has to wait for a
    // better moment (it stays pending and the game keeps running in between).
    bool service(PS2Runtime &runtime);
    // Test switches, checked once per vsync tick on the executor: PS2X_STATE_SAVE_AT=<tick>,<file>,
    // PS2X_STATE_LOAD_AT=<tick>,<file>.
    void checkAutomation(uint64_t vsyncTick);
}

namespace ps2_state
{
    // Machine state that lives in file-local globals (the library replacements): each file registers how to save it.
    // Parts are saved in name order, each in its own section.
    struct Part
    {
        const char *name;
        void (*save)(Writer &w);
        void (*load)(Reader &r);
        const char *(*blocker)(); // optional: non-null result = cannot be saved now, with the reason
        void (*describe)() = nullptr; // optional: logs the part's live state when a save is refused (diagnostics)
    };
    bool registerPart(const Part &part);

    // The runtime being saved/loaded (valid after attach()).
    PS2Runtime *runtime();
}
