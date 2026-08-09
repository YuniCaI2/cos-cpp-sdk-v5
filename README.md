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

## 连接复用（KeepAlive）

SDK 默认每个请求新建一条连接。开启 `keepalive_mode` 后，请求结束时 libcurl 的 easy 句柄会被归还到进程级句柄池，下一个请求取回同一句柄，从而复用已经建立好的 TCP/TLS 连接，同时启用 TCP keepalive 探活。为保持向后兼容，该开关默认关闭。

在 `config.json` 中开启：

```json
"keepalive_mode": true,
"keepalive_idle_time": 20,
"keepalive_interval_time": 5,
"CurlHandlePoolSize": 64
```

或用代码设置：

```cpp
CosSysConfig::SetKeepAlive(true);
CosSysConfig::SetCurlHandlePoolSize(64);
CosSysConfig::SetKeepIdle(20);
CosSysConfig::SetKeepIntvl(5);
```

| 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `keepalive_mode` | `false` | 开启句柄池复用连接，并启用 TCP keepalive |
| `CurlHandlePoolSize` | `64` | 空闲句柄缓存上限，最小为 1，仅在 `keepalive_mode` 为 `true` 时生效 |
| `keepalive_idle_time` | `20` | TCP keepalive 空闲多久后开始探活，单位 s |
| `keepalive_interval_time` | `5` | TCP keepalive 探活间隔，单位 s |

`CurlHandlePoolSize` 需要不小于并发线程数，否则句柄归还时会因为超出上限而被销毁，连接就复用不上。行为语义（哪些请求不进池、空闲连接如何持有）见 [`cos-cpp-sdk.md`](cos-cpp-sdk.md)。

### 实测效果

在某内网接入点上用 `HeadBucket` 压测（https，8 线程 × 50 请求）：吞吐从 29 QPS 升到 120 QPS，平均延迟从 263 ms 降到 58 ms。并发扫描下吞吐上限从约 147 QPS 升到约 790 QPS，峰值出现在 64 并发。

绝对值与具体链路强相关，可迁移的结论有两点。一是复用把每请求的 TCP+TLS 握手成本摊掉，尾延迟的改善比均值更明显。二是不复用时每个请求消耗一个临时端口，Windows 上 TIME_WAIT 要挂约 120 s，按 147 QPS 稳态需要约 17600 个端口，已经超过默认可用的 16384 个——也就是说不复用会先撞上临时端口耗尽，而不是带宽或服务端限流。

### 验证

```shell
cmake -S . -B build -DBUILD_UNITTEST=ON
cmake --build build --target cos-cpp-sdk-unit-tests
build/unittest/cos-cpp-sdk-unit-tests --gtest_filter=HttpSenderReuseTest.*:CurlHandlePoolTest.*
```

`HttpSenderReuseTest` 会起一个回环 HTTP 服务器并统计 TCP accept 次数：开启复用时 5 个请求只 accept 1 次，关闭时 accept 5 次，不需要凭证和外网。

`demo/keepalive_bench_demo/` 是对真实存储桶的压测程序，凭证从环境变量读取，会自动跑开关两种情况并给出对比。

## API 兼容性

- 原有请求、响应、`CosAPI` 操作接口和 XML 数据格式尽量保持不变。
- 原有无任务管理器参数的异步接口继续可用，并由 SDK 内部的有界 `std::thread` 执行器调度。
- 为兼容旧源码，带 `Poco::TaskManager*&` 的异步重载仍保留；该参数现在只作为兼容占位，不再创建或使用 Poco，调用后会被置为 `nullptr`。新代码应使用不带该参数的重载。
- SDK 公共头文件只暴露 RapidXML 的轻量前置声明；实际 XML 实现留在 SDK 编译单元中。现有直接使用 RapidXML 的源码仍可继续包含 SDK 随附的头文件。
- 由于底层依赖和异步实现已更换，本次重构以源码/API 兼容为目标，不承诺旧 Poco 版本产生的二进制 ABI 兼容。

完整重构说明见 [`SDK_REFACTOR_SUMMARY.md`](SDK_REFACTOR_SUMMARY.md)，接口参考见 [`cos-cpp-sdk.md`](cos-cpp-sdk.md)，版本记录见 [`CHANGELOG.md`](CHANGELOG.md)。
