#pragma once

namespace core {

struct ExecLambda final { };

template<typename Lambda>
auto operator^(ExecLambda, Lambda&& f) {
	return f();
}

#define ExEC_CODE_BLOCK ExecLambda{} ^ [&]
#define ExEC_CODE_BLOCK_WITH_CONTEXT(...) ExecLambda{} ^ [__VA_ARGS__]

} // namespace core
