#pragma once

namespace graphics::vulkan::internal {
    enum class AllocationTypeBit {
        None                           = 0,
        DedicatedMemory                = 1,
        NeverAllocate                  = 2,
        Mapped                         = 4,
        UserDataCopyString             = 32,
        UpperAddress                   = 64,
        DontBind                       = 128,
        WithinBudget                   = 256,
        CanAlias                       = 512,
        HostAccessSequentialWrite      = 1024,
        HostAccessRandom               = 2048,
        CanBeMapped                    = HostAccessSequentialWrite | HostAccessRandom,
        HostAccessAllowTransferInstead = 4096,
        StrategyMinMemory              = 65536,
        StrategyMinTime                = 131072,
        StrategyMinOffset              = 262144,
    };

    constexpr AllocationTypeBit operator|(AllocationTypeBit lhs, AllocationTypeBit rhs) noexcept {
        return static_cast<AllocationTypeBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr AllocationTypeBit operator&(AllocationTypeBit lhs, AllocationTypeBit rhs) noexcept {
        return static_cast<AllocationTypeBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::vulkan::internal
