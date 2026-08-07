# GameMesh Windows Test Client

WPF 联调客户端（VS2022），连接 Gateway 游戏 TCP，覆盖本版账号 / 邮件 / 道具切片。

## 打开与构建

1. 用 **Visual Studio 2022** 打开 `GameMesh.WindowsTest.sln`
2. 工作负载：**.NET 桌面开发**（含 WPF）
3. 目标框架：`net8.0-windows`（需安装 .NET 8 SDK）
4. `F5` 运行；或命令行：

```bat
cd client\windowstest
dotnet restore GameMesh.WindowsTest.sln
dotnet build GameMesh.WindowsTest.sln -c Debug
dotnet run --project GameMesh.WindowsTest
```

## 协议

- 帧格式：`[uint32 BE len][GameRequest/GameResponse protobuf]`（与 `ProtoFraming` 一致）
- 源协议：`GameMesh.WindowsTest/Proto/game.proto`（构建时由 `Grpc.Tools` 生成 C#）
- 与仓库根目录 `proto/game.proto` 保持同步；改协议后请同步拷贝到本目录再编译

## 推荐联调步骤

1. 服务端：`./scripts/run_version_local.sh`（或本机已起的 gateway，默认游戏口 `29081`）
2. 客户端填写 Host/Port → **连接**
3. **注册** → 自动填入 `player_id` → **登录**（拿到 `session_token`）
4. **发放道具** / **发信** / **刷新列表** / **详情** / **领取附件**
5. **下线**

## 说明

- 发放 / 邮件等需登录后的 `session_token`
- 服务端暂无「拉取完整背包」接口；背包列表按 `GrantItemRsp.bag_total` 与邮件附件领取结果在客户端聚合显示
- 发信走 `MailDeliver`，`receiver_id` 为当前玩家（给自己发测试信）

## 目录

```
windowstest/
├── GameMesh.WindowsTest.sln
├── README.md
└── GameMesh.WindowsTest/
    ├── GameMesh.WindowsTest.csproj
    ├── App.xaml(.cs)
    ├── MainWindow.xaml(.cs)
    ├── Proto/game.proto
    ├── Net/ProtoFraming.cs
    ├── Net/GameTcpClient.cs
    └── Models/
```
