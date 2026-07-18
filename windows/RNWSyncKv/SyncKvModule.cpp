#include "pch.h"
#include "SyncKvModule.h"

// All logic is header-inline (attributed REACT_MODULE template). This .cpp
// exists so the module is part of a translation unit the vcxproj compiles; the
// REACT_MODULE macro's static registration is emitted here and picked up by
// AddAttributedModules() in ReactPackageProvider.cpp.
