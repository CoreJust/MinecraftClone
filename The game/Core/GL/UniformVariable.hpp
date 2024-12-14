#pragma once
#include <Core/GL/Type.hpp>

namespace core::gl {
	struct UniformVariable final {
		// Name is expected to be stored elsewhere, probably as a key for a map
		Type type;
		SID location = 0;

		template<class T>
		void operator=(const T&) const;
	};
}