#include "pch.h"

#include "ReactPackageProvider.h"
#if __has_include("ReactPackageProvider.g.cpp")
#include "ReactPackageProvider.g.cpp"
#endif

// Required: without this include AddAttributedModules has no registration to
// discover and the call itself is an undeclared identifier (C3861).
#include "SyncKvModule.h"

using namespace winrt::Microsoft::ReactNative;

namespace winrt::RNWSyncKv::implementation
{

void ReactPackageProvider::CreatePackage(IReactPackageBuilder const &packageBuilder) noexcept
{
    // AddAttributedModules discovers the REACT_MODULE(SyncKvModule, L"RNWSyncKv")
    // registration in SyncKvModule.h. The second argument opts the module into
    // TurboModule registration, which the new architecture requires.
    //
    // RNW_NEW_ARCH is defined by PropertySheets\ReactNativeArchitecture.props
    // whenever RnwNewArch == true. (Note: some published libraries test
    // `#ifdef RnwNewArch`, which is an MSBuild property name and never a
    // preprocessor define -- that check silently never fires.)
#ifdef RNW_NEW_ARCH
    AddAttributedModules(packageBuilder, true);
#else
    AddAttributedModules(packageBuilder);
#endif
}

} // namespace winrt::RNWSyncKv::implementation
