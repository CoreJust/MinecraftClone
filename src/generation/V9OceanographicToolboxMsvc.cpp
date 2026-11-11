#include <cmath>
#include <complex>

#define real(value) std::real(value)
#define log(value) std::log(value)

// The C source uses the declarations above through these compatibility macros.
// DONT_CHECK INCLUDE_ORDER
#include "gsw_oceanographic_toolbox.c"

#undef log
#undef real
