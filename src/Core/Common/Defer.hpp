#pragma once
#include <Core/Macro/NameGeneration.hpp>

namespace core {
	struct _DeferNote { };

	template<typename Func>
	struct _DeferImpl final {
		Func f;

		~_DeferImpl() {
			f();
		}
	};

	template<typename Func>
	_DeferImpl<Func> operator^(_DeferNote, Func f) {
		return _DeferImpl<Func>(f);
	}
} // namespace core

#define GEN_ASE_NAME(LINE) GEN_NAME_(ASE__, LINE)
#define defer auto GEN_ASE_NAME(__LINE__) = ::core::_DeferNote{} ^ [&]()
