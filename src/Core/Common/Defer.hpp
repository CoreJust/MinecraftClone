#pragma once
#include <Core/Macro/NameGeneration.hpp>

namespace core::common {
	struct _DeferNote { };

	template<class Func>
	struct _DeferImpl final {
		Func f;

		~_DeferImpl() {
			f();
		}
	};

	template<class Func>
	_DeferImpl<Func> operator^(_DeferNote, Func f) {
		return _DeferImpl(f);
	}
} // namespace core::common

#define GEN_ASE_NAME(LINE) GEN_NAME_(ASE__, LINE)
#define defer auto GEN_ASE_NAME(__LINE__) = ::core::common::_DeferNote{} ^ [&]()
