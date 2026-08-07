#pragma once
// DeprecatedSuppress.h — 项目级 utility（v0.3 PR-4 / AYScene）
//
// 跨编译器 / 跨平台抑制 `[[deprecated]]` attribute 触发的编译 warning。
// 用途：豁免已知安全的调用点（facade fallback / bindBuiltinHostServices /
// 测试 fixture / SM 自身单例验证等），不污染 caller。
//
// 用法：
//   #include <DeprecatedSuppress.h>
//   AY_DEPRECATED_SUPPRESS_BEGIN
//   someCodeThatCallsDeprecated();
//   AY_DEPRECATED_SUPPRESS_END
//
// 为什么是项目级 macro：
//   * C++ 标准库没提供 deprecation suppress；MSVC / clang / gcc 各有 pragma
//   * 中央定义保证所有模块一致使用
//   * 命名 = `AY_*` 风格与项目惯例对齐（ay-naming-rules）
//   * 放在 AYApplication/include 而非 AYScene：SceneManager::instance() 的
//     deprecation 触发点大多是跨模块 caller（Editor / Application / 测试），
//     Application 作为 facade 提供者天然是 host-side utility 的家。

#if defined(_MSC_VER)
    // MSVC: __pragma 必须可在宏内展开（C4996 = deprecation warning）
    #define AY_DEPRECATED_SUPPRESS_BEGIN \
        __pragma(warning(push))         \
        __pragma(warning(disable : 4996))
    #define AY_DEPRECATED_SUPPRESS_END \
        __pragma(warning(pop))
#elif defined(__clang__)
    // clang: _Pragma 跨宏边界；-Wdeprecated-declarations 默认开
    #define AY_DEPRECATED_SUPPRESS_BEGIN \
        _Pragma("clang diagnostic push") \
        _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
    #define AY_DEPRECATED_SUPPRESS_END \
        _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
    // gcc: _Pragma 跨宏边界；-Wdeprecated-declarations 默认开
    #define AY_DEPRECATED_SUPPRESS_BEGIN \
        _Pragma("GCC diagnostic push")   \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
    #define AY_DEPRECATED_SUPPRESS_END \
        _Pragma("GCC diagnostic pop")
#else
    // 未知编译器：no-op（warning 仍触发；不阻断 build）
    #define AY_DEPRECATED_SUPPRESS_BEGIN
    #define AY_DEPRECATED_SUPPRESS_END
#endif