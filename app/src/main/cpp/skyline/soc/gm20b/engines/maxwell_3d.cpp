// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>

namespace skyline::soc::gm20b::engine::maxwell3d {
    Maxwell3D::Maxwell3D(const DeviceState &state) : Engine(state), macroInterpreter(*this) {
        ResetRegs();
    }

    void Maxwell3D::ResetRegs() {
        registers = {};

        registers.rasterizerEnable = true;

        for (auto &transform : registers.viewportTransform) {
            transform.swizzles.x = Registers::ViewportTransform::Swizzle::PositiveX;
            transform.swizzles.y = Registers::ViewportTransform::Swizzle::PositiveY;
            transform.swizzles.z = Registers::ViewportTransform::Swizzle::PositiveZ;
            transform.swizzles.w = Registers::ViewportTransform::Swizzle::PositiveW;
        }

        for (auto &viewport : registers.viewport) {
            viewport.depthRangeFar = 1.0f;
            viewport.depthRangeNear = 0.0f;
        }

        registers.polygonMode.front = Registers::PolygonMode::Fill;
        registers.polygonMode.back = Registers::PolygonMode::Fill;

        registers.stencilFront.failOp = registers.stencilFront.zFailOp = registers.stencilFront.zPassOp = Registers::StencilOp::Keep;
        registers.stencilFront.compare.op = Registers::CompareOp::Always;
        registers.stencilFront.compare.mask = 0xFFFFFFFF;
        registers.stencilFront.writeMask = 0xFFFFFFFF;

        registers.stencilTwoSideEnable = true;
        registers.stencilBack.failOp = registers.stencilBack.zFailOp = registers.stencilBack.zPassOp = Registers::StencilOp::Keep;
        registers.stencilBack.compareOp = Registers::CompareOp::Always;
        registers.stencilBackExtra.compareMask = 0xFFFFFFFF;
        registers.stencilBackExtra.writeMask = 0xFFFFFFFF;

        registers.rtSeparateFragData = true;

        for (auto &attribute : registers.vertexAttributeState)
            attribute.fixed = true;

        registers.depthTestFunc = Registers::CompareOp::Always;

        registers.blend.colorOp = registers.blend.alphaOp = Registers::Blend::Op::Add;
        registers.blend.colorSrcFactor = registers.blend.alphaSrcFactor = Registers::Blend::Factor::One;
        registers.blend.colorDestFactor = registers.blend.alphaDestFactor = Registers::Blend::Factor::Zero;

        registers.lineWidthSmooth = 1.0f;
        registers.lineWidthAliased = 1.0f;

        registers.pointSpriteEnable = true;
        registers.pointSpriteSize = 1.0f;
        registers.pointCoordReplace.enable = true;

        registers.frontFace = Registers::FrontFace::CounterClockwise;
        registers.cullFace = Registers::CullFace::Back;

        for (auto &mask : registers.colorMask)
            mask.r = mask.g = mask.b = mask.a = 1;

        for (auto &blend : registers.independentBlend) {
            blend.colorOp = blend.alphaOp = Registers::Blend::Op::Add;
            blend.colorSrcFactor = blend.alphaSrcFactor = Registers::Blend::Factor::One;
            blend.colorDestFactor = blend.alphaDestFactor = Registers::Blend::Factor::Zero;
        }

        registers.viewportTransformEnable = true;
    }

    void Maxwell3D::CallMethod(u32 method, u32 argument, bool lastCall) {
        state.logger->Debug("Called method in Maxwell 3D: 0x{:X} args: 0x{:X}", method, argument);

        // Methods that are greater than the register size are for macro control
        if (method > RegisterCount) [[unlikely]] {
            // Starting a new macro at index 'method - RegisterCount'
            if (!(method & 1)) {
                if (macroInvocation.index != -1) {
                    // Flush the current macro as we are switching to another one
                    macroInterpreter.Execute(macroPositions[macroInvocation.index], macroInvocation.arguments);
                    macroInvocation.arguments.clear();
                }

                // Setup for the new macro index
                macroInvocation.index = ((method - RegisterCount) >> 1) % macroPositions.size();
            }

            macroInvocation.arguments.emplace_back(argument);

            // Flush macro after all of the data in the method call has been sent
            if (lastCall && macroInvocation.index != -1) {
                macroInterpreter.Execute(macroPositions[macroInvocation.index], macroInvocation.arguments);
                macroInvocation.arguments.clear();
                macroInvocation.index = -1;
            }

            // Bail out early
            return;
        }

        registers.raw[method] = argument;

        if (shadowRegisters.mme.shadowRamControl == Registers::MmeShadowRamControl::MethodTrack || shadowRegisters.mme.shadowRamControl == Registers::MmeShadowRamControl::MethodTrackWithFilter)
            shadowRegisters.raw[method] = argument;
        else if (shadowRegisters.mme.shadowRamControl == Registers::MmeShadowRamControl::MethodReplay)
            argument = shadowRegisters.raw[method];

        switch (method) {
            case MAXWELL3D_OFFSET(mme.instructionRamLoad):
                if (registers.mme.instructionRamPointer >= macroCode.size())
                    throw exception("Macro memory is full!");

                macroCode[registers.mme.instructionRamPointer++] = argument;

                // Wraparound writes
                registers.mme.instructionRamPointer %= macroCode.size();

                break;
            case MAXWELL3D_OFFSET(mme.startAddressRamLoad):
                if (registers.mme.startAddressRamPointer >= macroPositions.size())
                    throw exception("Maximum amount of macros reached!");

                macroPositions[registers.mme.startAddressRamPointer++] = argument;
                break;
            case MAXWELL3D_OFFSET(mme.shadowRamControl):
                shadowRegisters.mme.shadowRamControl = static_cast<Registers::MmeShadowRamControl>(argument);
                break;
            case MAXWELL3D_OFFSET(syncpointAction):
                state.logger->Debug("Increment syncpoint: {}", static_cast<u16>(registers.syncpointAction.id));
                state.soc->host1x.syncpoints.at(registers.syncpointAction.id).Increment();
                break;
            case MAXWELL3D_OFFSET(semaphore.info):
                switch (registers.semaphore.info.op) {
                    case Registers::SemaphoreInfo::Op::Release:
                        WriteSemaphoreResult(registers.semaphore.payload);
                        break;
                    case Registers::SemaphoreInfo::Op::Counter:
                        HandleSemaphoreCounterOperation();
                        break;
                    default:
                        state.logger->Warn("Unsupported semaphore operation: 0x{:X}", static_cast<u8>(registers.semaphore.info.op));
                        break;
                }
                break;
            case MAXWELL3D_OFFSET(firmwareCall[4]):
                registers.raw[0xD00] = 1;
                break;
            default:
                break;
        }
    }

    void Maxwell3D::HandleSemaphoreCounterOperation() {
        switch (registers.semaphore.info.counterType) {
            case Registers::SemaphoreInfo::CounterType::Zero:
                WriteSemaphoreResult(0);
                break;
            default:
                state.logger->Warn("Unsupported semaphore counter type: 0x{:X}", static_cast<u8>(registers.semaphore.info.counterType));
                break;
        }
    }

    void Maxwell3D::WriteSemaphoreResult(u64 result) {
        struct FourWordResult {
            u64 value;
            u64 timestamp;
        };

        switch (registers.semaphore.info.structureSize) {
            case Registers::SemaphoreInfo::StructureSize::OneWord:
                state.soc->gm20b.gmmu.Write<u32>(registers.semaphore.address.Pack(), static_cast<u32>(result));
                break;
            case Registers::SemaphoreInfo::StructureSize::FourWords: {
                // Convert the current nanosecond time to GPU ticks
                constexpr u64 NsToTickNumerator{384};
                constexpr u64 NsToTickDenominator{625};

                u64 nsTime{util::GetTimeNs()};
                u64 timestamp{(nsTime / NsToTickDenominator) * NsToTickNumerator + ((nsTime % NsToTickDenominator) * NsToTickNumerator) / NsToTickDenominator};

                state.soc->gm20b.gmmu.Write<FourWordResult>(registers.semaphore.address.Pack(), FourWordResult{result, timestamp});
                break;
            }
        }
    }
}
