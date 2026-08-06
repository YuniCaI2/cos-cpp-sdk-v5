# Tencent Cloud COS C++ SDK

这是 Tencent Cloud COS C++ SDK v5 的现代化构建版本。SDK 保留原有请求、响应和 `CosAPI` 接口，传输层改为 libcurl，密码学实现使用 OpenSSL 兼容的 EVP 接口（通过 BoringSSL），任务调度使用 C++ 标准库。

## 依赖与要求

- CMake 3.21 或更高版本
- 支持 C++17 的编译器
- git submodule：`third_party/boringssl`、`third_party/curl`、`third_party/googletest`

SDK 的公共头文件不再包含 OpenSSL 或 libcurl 头文件；二者仅作为实现依赖随 submodule 一并编译。

## 从源码构建

```shell
git submodule update --init --recursive
cmake -S . -B build -DBUILD_DEMO=OFF -DBUILD_UNITTEST=OFF
cmake --build build --config Release
```

当前钉住的第三方版本见 `third_party/`：BoringSSL `0.20250818.0`、libcurl 8.12.1、GoogleTest 1.15.2。

可用的 CMake 选项：

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `BUILD_DEMO` | `OFF` | 构建 `demo/` 下的示例程序 |
| `BUILD_UNITTEST` | `OFF` | 构建 GTest 单元测试 |
| `BUILD_SHARED_LIB` | `OFF` | 额外构建共享库 |
| `ENABLE_COVERAGE` | `OFF` | GCC/Clang 下启用覆盖率 |
| `USE_OPENSSL_MD5` | `OFF` | 已废弃，仅保留旧脚本兼容性 |

## 构建 demo 与测试

```shell
git submodule update --init --recursive
cmake -S . -B build -DBUILD_DEMO=ON -DBUILD_UNITTEST=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows（Ninja）示例：

```powershell
git submodule update --init --recursive
cmake --preset default
cmake --build --preset default
ctest --preset default
```

demo 需要在 `demo/config.json` 中填写 COS 凭证和地域。网络集成测试仍然是可选项，使用 `-DBUILD_LEGACY_INTEGRATION_TESTS=ON` 时还需要自行提供对应的 Poco 测试环境；SDK 本身不再依赖 Poco。

## 安装

```shell
cmake --install build --config Release --prefix /opt/cos-cpp-sdk
```

会安装公共头文件和 SDK 库。推荐下游工程直接把本仓库作为 `add_subdirectory` / submodule 使用 `cos::cossdk`。

## API 兼容性

- 原有请求、响应、`CosAPI` 操作接口和 XML 数据格式尽量保持不变。
- 原有无任务管理器参数的异步接口继续可用，并由 SDK 内部的有界 `std::thread` 执行器调度。
- 为兼容旧源码，带 `Poco::TaskManager*&` 的异步重载仍保留；该参数现在只作为兼容占位，不再创建或使用 Poco，调用后会被置为 `nullptr`。新代码应使用不带该参数的重载。
- SDK 公共头文件只暴露 RapidXML 的轻量前置声明；实际 XML 实现留在 SDK 编译单元中。现有直接使用 RapidXML 的源码仍可继续包含 SDK 随附的头文件。
- 由于底层依赖和异步实现已更换，本次重构以源码/API 兼容为目标，不承诺旧 Poco 版本产生的二进制 ABI 兼容。

完整重构说明见 [`SDK_REFACTOR_SUMMARY.md`](SDK_REFACTOR_SUMMARY.md)，接口参考见 [`cos-cpp-sdk.md`](cos-cpp-sdk.md)，版本记录见 [`CHANGELOG.md`](CHANGELOG.md)。
