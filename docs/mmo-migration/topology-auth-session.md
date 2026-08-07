# GameMesh 拓扑（登录边界 / Push / LB 修正）

> 二进制 `world` = 逻辑服务 **GlobalService**（邮件等）。  
> Session/Auth、GlobalService、Redis、MySQL 标注为 **MVP 单点**。  
> `GameDB ×2` 只是访问层多实例，**不代表 MySQL HA**。  
> etcd 未启用时使用静态 `logic_addrs` / `session_addrs` / `world_addrs` / `gamedb_addrs` 降级。

```mermaid
flowchart TB
  Client["Client<br/>ProtoFraming + game.proto<br/>只连 VIP，不连 Logic/etcd"]
  VIP["公网 VIP:8081<br/>L4 LB"]

  subgraph Edge["Gateway ×2"]
    GW0["gw0<br/>GameTCP :8081<br/>HTTP管理 :8080 内网<br/>Push brpc :8181 内网"]
    GW1["gw1<br/>GameTCP :8083<br/>HTTP :8082 内网<br/>Push :8183 内网"]
  end

  subgraph SessBin["session 二进制 ×1 · MVP 单点<br/>同进程 · Auth/Session 逻辑与 proto 分离"]
    Auth["AuthService<br/>Login / VerifyToken / RefreshToken"]
    Sess["SessionService / MapRouter<br/>AcquireSession → 返回 Logic 路由<br/>MarkDisconnected ≠ Logout"]
  end

  subgraph Core["后端"]
    L0["GameLogic gl-0 :8201"]
    L1["GameLogic gl-1 :8202"]
    G["GlobalService ×1 · MVP 单点<br/>代码名 World :8301"]
    D0["GameDB ×2 :8501/:8502<br/>访问层多开≠MySQL HA"]
  end

  RedisAuth["Redis · MVP 单点<br/>Auth Token / TTL"]
  RedisSess["Redis · MVP 单点<br/>Session fence / ONLINE"]
  MySQL[(MySQL · MVP 单点<br/>账号与资产事实源)]
  Etcd["etcd 内网可选<br/>静态 *_addrs 降级"]

  Client -->|"TCP 长连接双向"| VIP
  VIP -->|"转发"| GW0
  VIP -->|"转发"| GW1

  GW0 -->|"Auth.Login / VerifyToken"| Auth
  GW1 -->|"Auth.Login / VerifyToken"| Auth
  GW0 <-->|"AcquireSession / MarkDisconnected / Reconnect"| Sess
  GW1 <-->|"AcquireSession / MarkDisconnected / Reconnect"| Sess

  Auth <-->|"Token / TTL"| RedisAuth
  Auth -->|"LookupAccount"| D0
  D0 --> MySQL
  Sess <-->|"Session 状态"| RedisSess

  GW0 -->|"粘性 BindPlayer/Dispatch<br/>按 Session.gamelogic_instance_id"| L0
  GW0 -->|"粘性"| L1
  GW1 -->|"粘性"| L0
  GW1 -->|"粘性"| L1
  GW0 & GW1 -->|"Forward 邮件等"| G

  L0 -->|"按 gateway_instance_id 选择目标<br/>PushBatch 非广播"| GW0
  L1 -->|"按 gateway_instance_id 选择目标<br/>PushBatch 非广播"| GW1

  L0 & L1 --> D0
  G --> D0

  GW0 & Auth & Sess & L0 & G & D0 -.->|"Register/Discover<br/>IServiceRegistry"| Etcd
```

## 登录调用链

```text
1. Client → VIP:8081 → Gateway：GameRequest.login
2. Gateway → AuthService.Login（查账号 / 发 access_token；不创建 Session）
3. Auth 失败 → 直接失败回包（不 AcquireSession）
4. Gateway → SessionService.AcquireSession
5. Session → Gateway：session_id, fence_token, gamelogic_instance_id,
                      map_instance_id, map_owner_epoch, route_version
6. Gateway → 指定 GameLogic：BindPlayer（粘性 instance）
7. Bind 失败 → Session.Logout 回滚
8. Gateway → Client：GameResponse.login（LoginSuccess）
```

后续命令由 Gateway 按粘性 `gamelogic_instance_id` 转发，并填写可信字段：
`player_id, session_id, fence_token, gamelogic_instance_id, map_instance_id, map_owner_epoch, route_version`。

## Push

GameLogic 只保存 `player_id / session_id / gateway_instance_id`，按 **target gateway** 调 `GatewayPushService.PushBatch`；旧 `session_id` 由 Gateway 拒绝。

## 断线

`MarkDisconnected` → `DISCONNECTED` + 重连宽限期；宽限内可从任意 Gateway `Reconnect`。仅主动退出 / 踢下线 / 宽限期超时 / 强制下线才 Logout。
