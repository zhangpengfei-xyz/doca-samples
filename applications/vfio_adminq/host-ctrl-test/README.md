# sRDMA Host 控制面测试

`srdma_ctrl_test` 是一个最小 libibverbs 程序，用于同时验证 Host `srdma.ko`
和 `vfio_adminq` 后端的控制面及 AdminQ doorbell 路径。

## 覆盖范围

- 打开/关闭 context：`ALLOC_UCTX`、`DEALLOC_UCTX`。
- PD：`ALLOC_PD`、`DEALLOC_PD`。
- MR：`REG_MR`、`DEREG_MR`。
- EQ/CQ：创建 CQ 时惰性触发 `CREATE_EQ`，并测试 `CREATE_CQ`、`DESTROY_CQ`。
- RC QP：创建、查询、RESET→INIT→RTR→RTS→ERR→RESET 和销毁。
- GID：枚举 GID；可选临时地址循环验证 `ADD_GID`、`DEL_GID`。
- 统计：读取 RDMA hw counters，触发 `GET_STATS` 和三个扩展 opcode。
- 健康：等待驱动主动 `HEALTH_CHECK`，并运行 devlink `fw` reporter 的
  `show` 和 `diagnose`。
- 后端核对：比较测试前后的
  `/sys/kernel/debug/srdma/<BDF>/commands/<OP>/{n,failed}`。

程序不会调用 `ibv_post_send()`、`ibv_post_recv()`、`ibv_req_notify_cq()` 或
`ibv_poll_cq()`，因此不依赖当前尚未实现的数据面及 CQ/SQ/RQ doorbell。

为避免中断 Host 管理连接，程序不会主动 flap netdev，所以 `NETDEV_UP/DOWN` 不在
单次测试范围；它们应在独立维护窗口通过现有驱动生命周期测试验证。同理，CEQ
只在驱动移除时执行 `DESTROY_EQ`，本程序不会为测试该 opcode 卸载驱动。

## Host 编译

需要安装与驱动 ABI 匹配的 sRDMA libibverbs provider，以及 `libibverbs-dev`、
`rdma-core`、`iproute2` 和 `devlink`。

```bash
cd /root/ByteDance/doca-samples/applications/vfio_adminq/host-ctrl-test
make
```

如果从 BF3 复制到 Host：

```bash
scp -r /root/ByteDance/doca-samples/applications/vfio_adminq/host-ctrl-test \
    192.168.0.100:/root/
ssh 192.168.0.100 'cd /root/host-ctrl-test && make'
```

## 运行

确保后端已经启动、Host 已枚举 `1e93:006a`、`srdma.ko` 已加载，且 debugfs
挂载在 `/sys/kernel/debug`。完整控制面测试建议显式给出一个未被使用的测试地址：

```bash
sudo ./srdma_ctrl_test --device srdma_0 \
    --gid-cycle 198.18.0.1/32
```

`--gid-cycle` 会在 sRDMA 绑定的 netdev 上临时添加地址，等待 `ADD_GID`，随后立即
删除并等待 `DEL_GID`。只有程序确实添加成功时才执行删除；不要传入已有业务地址。
不指定此参数时只枚举现有 GID，并将 ADD/DEL 生命周期标为 WARN。

若后端尚未实现 INIT→RTR 等状态转换，可先定位基础资源问题：

```bash
sudo ./srdma_ctrl_test --device srdma_0 --basic-qp
```

成功标准是最终 `FAIL=0`。每个 verbs 操作成功仍不够；对应 AdminQ `n` 必须增加且
`failed` 不得增加。CEQ 在驱动内按 vector 惰性创建并一直保留到设备移除，因此重复
运行时 `CREATE_EQ` 允许复用已经存在的 EQ；`DESTROY_EQ` 不属于单次程序生命周期。
