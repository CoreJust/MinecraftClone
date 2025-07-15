#pragma once

namespace core {
    template<typename>
    struct UnPtrImpl { };
    
    template<typename T>
    struct UnPtrImpl<T*> {
        using Type = T;
    };

    template<typename T>
    using UnPtr = UnPtrImpl<T>::Type;
} // namespace core
