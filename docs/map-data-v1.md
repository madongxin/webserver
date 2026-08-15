# 地图数据 v1

服务器不读取 `.unity`、Prefab、FBX 或 NavMesh 二进制。权威网格是 Editor 导出的 JSON + 独立 SHA-256 sidecar。

## 文件

| 文件 | 说明 |
| --- | --- |
| `config/maps/map_manifest.json` | 模板清单 |
| `config/maps/map_1001.json` | 模板 1001（MainScene） |
| `config/maps/map_1001.json.sha256` | 文件 SHA-256（小写 hex） |

当前地图 hash：`ceef56586c5281dca4ce45340f511d0d577fd724b14131ae5a21d01ea7f41317`（以 sidecar 为准）。`data_version=1`。

## JSON 字段

- `schema_version`、`map_template_id`、`scene_name`、`data_version`
- `bounds_min` / `bounds_max`：`[x, y, z]`
- `aoi_cell_size`：AOI 格子（米）
- `nav_sample_step`：可行走采样步长（米）
- `grid_width` / `grid_height`
- `walkable_rle`：`[value, count, ...]`，0 不可走，1 可走
- `spawn_points[]`：`id`、`position`、`yaw`

## 坐标

Unity 水平面 **X/Z**，**Y 为高度**。服务器不交换轴。

```text
col = floor((x - min_x) / nav_sample_step)
row = floor((z - min_z) / nav_sample_step)
```

AOI 格子用 `aoi_cell_size`，与 nav 采样步长不是同一个概念。

模板 1001 默认出生点：`(-28.5, -0.244, -7.25)`，yaw `76.022`，必须在边界内且可走。

## EnterMap 校验

- 字段 5 `map_data_version`、字段 6 `map_data_sha256`、字段 7 `operation_id`
- 客户端哈希非空且与 sidecar 不一致 → `ERR_MAP_DATA_MISMATCH`，响应带回服务器 version/hash
- 空哈希：兼容旧客户端
- `map_instance_id=0`：加入 50 人公共池；指定实例满员 → `ERR_MAP_FULL`，不静默换图
- `operation_id` 按 `player_id` 幂等占位

## 运行时

`MapCatalog` 在 GameLogic 启动时加载 `config/maps`（可用 `map_data_dir` 覆盖）。实例共享不可变模板，不复制网格。`public_map_capacity=50`。
