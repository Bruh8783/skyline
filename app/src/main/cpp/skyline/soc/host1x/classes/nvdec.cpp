// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "nvdec.h"

namespace skyline::soc::host1x {
    NvDecClass::NvDecClass(const DeviceState &state, std::function<void()> opDoneCallback)
        : state(state),
          opDoneCallback(std::move(opDoneCallback)) {}

    void NvDecClass::CallMethod(u32 method, u32 argument) {
        state.logger->Warn("Unknown NVDEC class method called: 0x{:X} argument: 0x{:X}", method, argument);
    }
}
