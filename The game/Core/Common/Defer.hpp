#pragma once
#include <Core/Macro/NameGeneration.hpp>

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

#define GEN_ASE_NAME(LINE) GEN_NAME_(ASE__, LINE)
#define defer auto GEN_ASE_NAME(__LINE__) = _DeferNote{} ^ [&]()
