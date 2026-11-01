#ifndef LP_I586_CXXCONFIG_H
#define LP_I586_CXXCONFIG_H

// Force-included into every C++ file (see CMakeLists.txt). libstdc++ declares std::string and
// std::wstring "extern template", so their code would come precompiled from the library,
// which was built for the Pentium Pro (CMOV). -1 makes each file compile them itself, for the
// CPU this build targets; it is the switch libstdc++ uses for its own checked builds, and it
// only affects basic_string.
#include <bits/c++config.h>
#undef _GLIBCXX_EXTERN_TEMPLATE
#define _GLIBCXX_EXTERN_TEMPLATE -1

#endif
