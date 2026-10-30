#ifndef PS2_CODE_RESOLVER_H
#define PS2_CODE_RESOLVER_H

// Optional per-game hook for guest code that is not part of the recompiled image and not a
// registered overlay (e.g. a small stub the game loads into a heap buffer at a variable address).
//
// PS2Runtime::lookupFunction / hasFunction consult the resolver only after the generated function
// table and the overlay tables both miss. The resolver gets the guest RDRAM base, the guest PC and
// the runtime; it returns a function that executes the code at that PC, or nullptr to decline (the
// normal missing-function report then follows). It may be called for the same address more than
// once (hasFunction, then lookupFunction) and must be cheap and free of side effects on a miss.
//
// Default is no resolver, so games that never install one behave exactly as before.
//
// Not included by generated code; only ps2_runtime.cpp and game override modules include it.

#include "ps2_runtime.h"

using PS2CodeResolver = PS2Runtime::RecompiledFunction (*)(uint8_t *rdram, uint32_t address, PS2Runtime *runtime);

// Installs (or, with nullptr, removes) the process-wide resolver.
void ps2SetCodeResolver(PS2CodeResolver resolver);

#endif // PS2_CODE_RESOLVER_H
