#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

namespace ps2_state
{
    struct Access; // save states (src/lib/ps2_savestate.cpp)
}

class PSPadBackend
{
    friend struct ps2_state::Access;

public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
};

#endif
