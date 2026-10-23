# 605 人卡顿：源码核对后的原因

工作区基线是 `86391f0`。`git pull` 没有做：工作区有未提交修改。12:14 拉起的正式进程仍在跑，`/proc/2270/exe` 和 `/proc/2199/exe` 指向已被 13:21 链接覆盖的旧 inode。磁盘上的新 `gateway` / `gamelogic` 还没换上去。没有重启。

## 11:35–11:43 计数

Prometheus `query_range`，UTC `2026-10-23T03:35:00Z` 到 `03:43:00Z`。两台 Gateway 的 `gamemesh_reply_soft_drop_total` 和 `gamemesh_heartbeat_soft_drop_total` 在这个窗口里没有增加，差值是 0。

因此不能把「回包被软丢」写成这次约 52% 移动超时的主因。这项保持 **待验证**。监控截图里的内核发送队列接近 0，也不足以单独证明内核拥塞。

`72096 - 34103 - 37580 = 413` 在服务端没有按请求编号对上的轨迹。这 413 条是在途、取消还是跨窗口，**未知**。

## 代码里已经能证明的缺陷

1. `TcpReplySink::SendFrame` 和心跳分支在 `TrySendControl` 失败时只计数并返回。连接保持在线，客户端看不到这一帧。业务如果已经提交，不会再改成失败。11:35 窗口的软丢计数是 0，所以这条路径当时没有发生，但不能当它以后不会发生。

2. `GatewayConnRegistry::SendBySession` 原来调用 void 回调后无条件返回 true。回调在连接的 EventLoop 里 `TrySend` 失败时直接 return。可靠 ENTER/LEAVE 会在序号已经分配之后被当成已接受。

3. AOI 发布已经在独立线程。`SetAoiPushBacklog(0)` 在批次被取走时就清零，`PublishTaken` 里的同步 `PushBatch` 仍在跑。队列图不能表示 worker 正忙。没有把 AOI 再改成另一套异步。

4. 全量快照默认上限 128，地图可见上限是 200。密集视野超过 128 会 `ERR_SNAPSHOT_TOO_LARGE`，恢复不了周围的人。

5. 最新移动帧在内核未发送超过 1KB 时留在应用缓冲，并关掉 EPOLLOUT。内核队列自己下降不会再触发写。没有新的请求时，这帧会停住。

## 不在本仓库里的部分

Unity 主线程 Pump、迟到 `MoveRsp` 被当成 Push、机器人用 `GameResponse.seq` 更新 `last_server_seq`，都在客户端。本仓库没有 Luna 工程，这些没有改。它们仍可能解释机器人超时和 `ERR_AOI_RESYNC_REQUIRED`，但是 **待验证**。

## 没有跑的压测

605 人只心跳、多线 5Hz、同实例热点，都没有跑。不能写「2 秒内 99%」。
