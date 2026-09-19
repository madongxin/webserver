# Unity / Luna：主城传送门进副本 Cursor 提示词

把本文件整份贴进 **客户端（luna）仓库** 的 Cursor 对话，直接执行。不要改服务器仓库。不要 commit / push，除非用户明确要求。

服务器协议事实源：`webserver/docs/protocol/export/`（`game.proto` + `game.desc` + `protocol_manifest.json`）。客户端必须导入这一份，禁止用旧 schema 或反向覆盖服务器 proto。

---

## 你的角色

你在改 Unity 客户端。目标：进 1001 主城后，在重生点旁边看到一个传送门；走近后进入副本；副本场景复用 1001 已有资源；副本里同一位置的传送门送回主城。

## 不可违反

1. 外网只连 Gateway TCP。不要连 Session / GameLogic / GameDB。不要引入 gRPC。
2. `package game`，C# namespace `GameMesh.Protocol`。Hello 必须带当前 `schema_sha256`，否则 `ERR_SCHEMA_MISMATCH`。
3. 不要对模板 **2102** 发公网 `CreateDungeon`。进本/回城只发 `InteractPortal`。
4. 不要把 1001 改成 LINE。不要改登录密码路径。不要在客户端伪造玩家属性。
5. 水平面 Unity **X/Z**，**Y 为高度**。不要交换轴。
6. 未改协议前不要自己发明 RPC。本轮协议已追加字段，必须先同步 export。

---

## 先同步协议

1. 从服务器拿到最新 `docs/protocol/export/`：
   - `game.proto`
   - `game.desc`
   - `protocol_manifest.json`
   - `game.proto.sha256` / `game.desc.sha256`
2. 用现有流程重新生成 C# protobuf（保持 `csharp_namespace = GameMesh.Protocol`）。
3. Hello 校验 `protocol_manifest.json` 的 `schema_sha256` 必须与 `ServerHelloRsp.schema_sha256` 一致。
4. 确认生成代码里有：
   - `GameRequest.InteractPortal` field **80**
   - `GameResponse.InteractPortal` field **81**
   - `MapManifestEntry.SceneName=4` / `Kind=5` / `VisualMapTemplateId=6` / `Portals=7`
   - `PortalDef`、`InteractPortalReq`、`InteractPortalRsp`

---

## 资源

1. 打开 Unity Asset Store，搜索免费 **Portal**（传送门 / 魔法门 / 石门均可）。下载并导入到工程。只选免费资源。
2. 做成 Prefab，例如 `Assets/GameMesh/Prefabs/SpawnPortal.prefab`。不要改主城地形、NavMesh、1001 网格 JSON。
3. **副本不新做场景。** 模板 2102 的 `scene_name=MainScene`，`visual_map_template_id=1001`，网格 SHA-256 与 1001 相同。进 2102 时继续加载现有 MainScene / 1001 资源。可以加一层轻量滤镜、UI 标题「副本」或传送门特效，禁止复制一份地图。

---

## 服务器已冻结的数据

| 项 | 值 |
| --- | --- |
| 主城 | `map_template_id=1001`，`kind=LEGACY_POOL`，`scene=MainScene` |
| 出生点 | `(-28.5, -0.244, -7.25)`，yaw `76.022` |
| 传送门 | `(-22.5, -0.244, -7.25)`，yaw `76.022`，半径 **3m** |
| 进本 portal_id | `spawn_to_dungeon` → 模板 **2102** |
| 回城 portal_id | `dungeon_to_spawn` → 模板 **1001** |
| 副本 | `2102`，`kind=DUNGEON`，人数硬顶 5，空本 30s 关闭 |
| 2102 资源 | 与 1001 同一份 `map_1001.json` SHA-256 |

Hello `maps[]` 会下发上述 portals。以 Hello 为准；若旧服没有 `portals` 字段，用上表冻结坐标兜底。

玩家出生点距传送门约 6m，不会一进图就触发。

---

## 必须实现的客户端行为

### 1. Hello 后缓存地图与传送门

对 `ServerHelloRsp.maps` 建字典：

- `map_template_id` → `{sha256, data_version, scene_name, kind, visual_map_template_id, portals[]}`
- 进图时 `EnterMapReq.map_data_sha256` / `map_data_version` 用该模板 Hello 条目，不要写死旧 hash。
- 加载场景：`visual_map_template_id != 0` 则用它的场景（2102 → 1001/MainScene），否则用 `scene_name`。

### 2. 进 1001 后生成传送门

`EnterMap` 成功且当前模板有 `portals`：

- 在每个 portal 的 `position` 实例化传送门 Prefab。
- 朝向用 `yaw`（绕 Y）。
- 加 Trigger Collider，半径用 `trigger_radius`（默认 3）。水平距离只算 X/Z。
- 切图/离图时销毁旧传送门，按新图 Hello 条目重建。

### 3. 靠近 → `InteractPortal`

玩家进入 Trigger（或本机位置与门 X/Z 距离 ≤ `trigger_radius`）：

```text
GameRequest.seq = 下一个请求序号
GameRequest.session_token = 当前 fence token
InteractPortalReq:
  player_id        = 登录后的权威 player_id
  realm_id         = 当前 realm（没有就 1）
  portal_id        = "spawn_to_dungeon" 或 "dungeon_to_spawn"
  operation_id     = 每次尝试唯一（例如 guid）；重试同一进入沿用同一个
  map_data_version = 目标模板 Hello.data_version
  map_data_sha256  = 目标模板 Hello.sha256
```

成功：`GameResponse.ok=true` 且 `interact_portal.ok=true`。把 `InteractPortalRsp` **完全按现有 EnterMap 成功路径应用**（切场景、刷 self / aoi_snapshot、更新 map_instance_id、owner、kind）。

失败只看 `error_code`，不要解析 `message` 文本：

| error_code | 客户端动作 |
| --- | --- |
| `ERR_PORTAL_TOO_FAR` | 忽略，人走开再靠近 |
| `ERR_PORTAL_UNKNOWN` | 用 Hello 重建传送门，不要重试错 ID |
| `ERR_PORTAL_REQUIRED` | 你误发了 CreateDungeon(2102)，改成 InteractPortal |
| `ERR_SESSION_EXPIRED` | 重新 Login，不要当缺线 |
| `ERR_MAP_DATA_MISMATCH` | 用响应里的服务器 hash 更新本地地图数据 |
| `ERR_DUNGEON_NOT_FOUND` | 本已关，回到 1001 后再走一次门 |
| `ERR_RATE_LIMITED` / `ERR_OVERLOADED` | 按 jitter 退避 |

加 1.5s 冷却，避免 Trigger 连发。请求未返回前不要发第二次。

### 4. 副本内回城

2102 加载同一 MainScene 后，在 `dungeon_to_spawn` 再放一座门。走近发 `InteractPortal(portal_id=dungeon_to_spawn)`。成功后按 EnterMap 回到 1001 公共池。

### 5. 不要做的事

- 不要 `CreateDungeon(map_template_id=2102)`。
- 不要 `EnterMap(2102, map_instance_id=0)`。
- 不要在客户端把人直接 `transform` 到副本坐标而不等服务器响应。
- 不要用 2101 `PartyDungeon` 小方块图。那是组队测试图，不是本功能。
- 不要为 2102 新建 Unity 场景或再导一份网格。

---

## 联调

- 入口：VIP `10.0.0.2:8081`（备用 8083）。
- 流程：Hello → Register/Login → EnterMap(1001) → 走到传送门 → InteractPortal → 确认 `kind=DUNGEON` 且 `map_template_id=2102` → 再进回城门 → `map_template_id=1001`。
- 两人不会进同一个 2102 实例（每人一次 InteractPortal 开自己的本）。
- 空本约 30s 后服务器关闭；再进门会开新实例。

## 完成定义

同时满足才算完成：

1. 1001 出生点旁能看到 Asset Store 传送门模型。
2. 走近后进入副本，场景仍是 1001/MainScene 资源。
3. `InteractPortalRsp` 驱动权威位置 / AOI，不靠本地传送。
4. 副本内传送门能回 1001。
5. Hello schema 与服务器 export 一致，正式服不会 `ERR_SCHEMA_MISMATCH`。
6. 工程能在 Editor 里走通上述路径；不要只改文档。
