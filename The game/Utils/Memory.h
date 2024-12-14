#pragma once
#include <cstdlib>
#include <cstring>

#define arrayElem(arr, size, x, y) ((arr)[(y) * (size) + (x)])

namespace memory {
	template<class T>
	inline void exchange(T* f, T* s) {
		std::swap(*f, *s);
	}
}