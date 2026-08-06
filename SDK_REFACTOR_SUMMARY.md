# COS C++ SDK 重构总结

## 1. 重构目标

本次重构针对旧工程的三个主要问题展开：CMake 版本过老、Poco/OpenSSL 等实现细节泄漏到 SDK 公共头文件、依赖库过重且需要手工复制到 `third_party`。重构优先保持原有 `CosAPI`、Request/Response 和异步接口的源码使用方式。

## 2. 主要改动

| 原实现 | 现在的实现 | 影响 |
| --- | --- | --- |
| Poco Net/NetSSL | libcurl easy API + OpenSSL TLS | HTTP、HTTPS、上传、下载、进度和取消逻辑集中在 `src/util/http_sender.cpp` |
| Poco JSON | SDK 内部轻量 JSON parser/writer | 只用于配置和断点文件；公共头不再包含 Poco JSON |
| Poco Thread/Task/ThreadPool | `std::thread`、互斥量、条件变量和有界执行器 | 异步任务和并发上传/下载不再依赖 Poco |
| Poco File | `std::filesystem` | 目录枚举和路径处理使用 C++17 标准库 |
| Poco DNS | `getaddrinfo` + SDK 内部 DNS 缓存 | 保留缓存、过期和轮询行为 |
| Poco Buffer | `std::vector` | 流拷贝不再依赖 Poco Buffer |
| Poco Checksum | SDK 内部 CRC32 | SelectObjectContent 事件校验不再依赖 Poco |
| OpenSSL 旧式 MD5/HMAC 调用 | OpenSSL EVP/HMAC API | OpenSSL 仍用于 TLS 和密码学，但实现不进入公共头文件 |
| 全局 include/link/硬编码库路径 | CMake target 依赖 | 依赖只在 `cossdk` target 的私有实现和导出包中声明 |

libcurl 的 easy interface 适合把请求、回调、超时和 TLS 配置封装在 SDK 内部；TLS 回调仍通过原来的 `SSLCtxCallback(void*, void*)` 保留，因此使用 OpenSSL 的现有业务代码不需要改动。

## 3. CMake 重构

根目录 `CMakeLists.txt` 现在要求 CMake 3.21+ 和 C++17，并使用：

- `find_package(CURL REQUIRED)`
- `find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)`
- `find_package(Threads REQUIRED)`
- `target_compile_features`、`target_include_directories` 和 `target_link_libraries`
- `GNUInstallDirs`、`CMakePackageConfigHelpers` 和导出 target

主要 target：

- `cos::cossdk`：默认静态库
- `cos::cossdk-shared`：`BUILD_SHARED_LIB=ON` 时生成的共享库

安装后可用：

```cmake
find_package(cos-cpp-sdk CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE cos::cossdk)
```

安装包的 `cos-cpp-sdk-config.cmake` 会自动查找 CURL、OpenSSL 和 Threads，避免旧版“复制头文件和库到 SDK 目录”的方式。该用法遵循 CMake 的 `find_package`/依赖导出机制，参见 [CMake find_package 文档](https://cmake.org/cmake/help/latest/command/find_package.html) 和 [CMake 依赖使用指南](https://cmake.org/cmake/help/latest/guide/using-dependencies/index.html)。

## 4. 公共头文件与接口兼容

- `cos_api.h` 不再 include Poco Task；仍保留 `Poco::TaskManager*&` 异步重载的声明，通过前置声明避免 SDK 头文件拉入 Poco。
- 旧异步重载仍可编译调用，但任务管理器参数不再被 SDK 使用，调用后置为 `nullptr`；新代码应使用无该参数的重载。
- `cos_config.h`、`data_process_req.h` 和 `json_util.h` 已移除 Poco JSON 类型。旧的 JSON 辅助名字保留为模板化的 duck-typing 接口，已有调用代码通常无需修改。
- RapidXML 仍用于 XML 协议数据，但公共头只做轻量前置声明；实际 `rapidxml.hpp`/`rapidxml_print.hpp` 在需要的 `.cpp` 中显式包含，消除了隐式传递依赖。
- Request/Response 类、`CosAPI` 操作签名、异步回调模型以及 XML/HTTP 协议字段尽量没有改变。
- 本次目标是源码/API 兼容，不承诺旧 Poco 构建产物与新构建产物之间的二进制 ABI 兼容。

## 5. 可靠性和工程性修复

- 异步执行器使用有界队列和固定数量 worker，线程数读取 `CosSysConfig::GetAsynThreadPoolSize()`。
- curl 回调覆盖流式上传、流式下载、响应头、进度、取消、响应体捕获、MD5 和 Content-Length 校验。
- TLS 校验、CA 文件、已有 SSL context 回调和连接/接收超时继续由 Request 配置控制。
- 文件上传/下载的并发槽位改为标准线程，保留原有滑动窗口生命周期。
- 目录上传改用 `std::filesystem::recursive_directory_iterator`。
- 复制/移动 object、断点 checkpoint、DNS 缓存和 SelectObjectContent CRC32 路径都已去除 Poco 运行时依赖。
- 修复 replication XML 解析对 RapidXML data-node 细节的依赖。

## 6. 构建验证

使用 CMake 4.3.2、Visual Studio 2026 Build Tools、vcpkg 的 CURL/OpenSSL/GTest 依赖完成验证：

1. 核心静态库 MSVC Release 构建通过。
2. `BUILD_UNITTEST=ON` 构建通过。
3. GTest 共 41 个测试全部通过。
4. `BUILD_DEMO=ON` 下所有 demo 构建通过。
5. `BUILD_SHARED_LIB=ON` 下静态库和共享库（含 Windows DLL export）均构建通过。
6. `cmake --install` 成功生成 headers、`cossdk.lib` 和 CMake package export。
7. 独立下游工程通过 `find_package(cos-cpp-sdk CONFIG REQUIRED)` 和 `cos::cossdk` 编译链接通过。

验证使用的依赖安装命令示例：

```shell
vcpkg install curl[openssl] openssl gtest --triplet x64-windows
```

## 7. 已知边界

- OpenSSL 没有从 SDK 中移除，因为 SDK 需要 HTTPS/TLS，并且要保持原有 `SSLCtxCallback` 能力；但是它已经不再通过公共 SDK 头文件耦合。
- `unittest` 中的旧网络集成测试仍然保留为显式可选目标，并可能需要 Poco 测试服务器；这不影响 SDK 核心库。
- 新版本要求 C++17。若业务必须停留在 C++11，需要保留旧版本或单独做兼容分支。
