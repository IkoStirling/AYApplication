# AYApplication

AYApplication 是引擎 Host 装配层，负责应用启动、子系统注册、GameLoop 接线、内建服务绑定和进程级清理。

## 公开接口

```cpp
#include <AYApplication.h>
#include <AYApplication/IApplication.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RegisterDefaultModules.h>
```

入口头文件位于模块根目录；稳定接口位于 `interface/AYApplication/`，实现辅助头位于 `include/AYApplication/`。

## 依赖

- 公开：AYCore、AYGameLoop
- 内部：AYAudio、AYDeviceSubSystem、AYScript、AYResource、AYTask、AYScene、AYRenderer、AYPhysics

Host 服务约束和模块装配表见 [design.md](design.md) 与 [docs/engine-host.md](docs/engine-host.md)。
