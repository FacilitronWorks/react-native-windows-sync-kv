#pragma once

#include "ReactPackageProvider.g.h"

using namespace winrt::Microsoft::ReactNative;

namespace winrt::RNWSyncKv::implementation
{

struct ReactPackageProvider : ReactPackageProviderT<ReactPackageProvider>
{
    ReactPackageProvider() = default;

    void CreatePackage(IReactPackageBuilder const &packageBuilder) noexcept;
};

} // namespace winrt::RNWSyncKv::implementation

namespace winrt::RNWSyncKv::factory_implementation
{

struct ReactPackageProvider : ReactPackageProviderT<ReactPackageProvider, implementation::ReactPackageProvider> {};

} // namespace winrt::RNWSyncKv::factory_implementation
