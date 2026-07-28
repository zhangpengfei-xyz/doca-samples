# sRDMA Driver 对接设计

本文描述 `vfio_adminq` 当前实现。Host 使用真实 `srdma.ko` 和 libibverbs provider；
DPU 使用 DOCA DevEmu PCI、一个 by-offset doorbell region、DPA completion、Comch 和
Gemini。旧的 UAR TLP/共享内存 IPC 方案已移除。

## 1. 目标与边界

当前实现覆盖：

- PCI `1e93:006a` endpoint、单 BAR0、129 个 MSI-X vector。
- AdminQ/AEQ DMA 初始化和控制面命令。
- UCTX、PD、MR、EQ、CQ、QP、GID、统计和健康检查。
- AdminQ、AEQ、CEQ、CQ、SQ、RQ 六类 doorbell 的 DPA completion。
- Host MSI-X table/PBA 和控制向量触发。

当前不实现 RoCE 数据面。SQ/RQ 可以生成 WQE、更新 DMA doorbell record 并敲 MMIO
doorbell，但后端只验证 completion 到达，不处理 WQE 或产生 CQE。

## 2. 组件分工

| 组件 | 职责 |
|---|---|
| `pci-fe` | 创建 PCI type/endpoint，配置 BAR、CFG transaction、DB region、MSI-X，处理 plug/unplug 和 Gemini server |
| `dev-be` | attach endpoint，运行 Gemini client、AdminQ DMA、资源表、MSI-X object、DPA/Comch doorbell pipeline |
| DPA `doorbell_dev.c` | 等待 DB completion、读取 16-bit value、ack 并经 Comch 发给 Arm |
| `srdma.ko` | 枚举设备、初始化 AdminQ/AEQ、执行内核 verbs、写控制类 DB |
| sRDMA provider | mmap 共享 DB page，构造用户态 WQE/DB record，写 CQ/SQ/RQ DB |

`pci-fe` 不消费业务 doorbell；`dev-be` 不再依赖 `srdma_uar_ipc`。

## 3. BAR0 布局

DOCA BAR0 配置为 256 KiB：

| BAR0 offset | size | 类型 | 用途 |
|---:|---:|---|---|
| `0x0000` | `0x1000` | TLP transaction | CFG register、AdminQ/AEQ 地址、状态和诊断区 |
| `0x1000` | `0x1000` | MSI-X table | 129 个 vector 的 addr/data/vector-control |
| `0x2000` | `0x1000` | MSI-X PBA | pending bitmap |
| `0x8000` | `0x1000` | by-offset DB region | 所有 sRDMA doorbell |

DB region 参数为：

- `log_db_size = 1`：每个 doorbell 为 2 字节。
- `log_db_stride_size = 3`：相邻 hardware DB ID 间隔 8 字节。
- endpoint `num_db = 33`：最大使用 hardware DB ID 为 32。

BF3 当前 custom TLP PCI type 要求该 DB region 位于 `0x8000`；实机探测中
`0x10000` 无法启动 type。

## 4. Doorbell ABI

六类 DB 共用同一个 4 KiB region。Host 的所有 UAR context 都 mmap 同一个物理 DB
page；队列隔离由 AdminQ 分配的 resource handle 和原有 DMA doorbell record 提供，
不再通过每个 context 独占 4 KiB UAR page 实现。

| 语义 ID | 类型 | BAR0 offset | hardware DB ID | 16-bit value |
|---:|---|---:|---:|---|
| 0 | AdminQ | `0x8000` | 0 | Admin TX producer index |
| 1 | AEQ | `0x8008` | 1 | AEQ consumer index |
| 2 | CEQ | `0x8040` | 8 | EQ handle |
| 3 | CQ | `0x8080` | 16 | CQ handle |
| 5 | RQ | `0x80c0` | 24 | QP handle |
| 4 | SQ | `0x8100` | 32 | QP handle |

CQ/SQ/RQ 的完整 CI/PI、command sequence 和 soft key 仍先写入 64-byte DMA doorbell
record，再用内存屏障保证 DB record/WQE 对 Host MMIO write 可见。MMIO 只传 handle，
后端后续数据面实现可按 handle 找到资源和对应 DB record。

Host 内核和 provider 必须使用 16-bit little-endian MMIO：内核为 `writew()`，provider
为 `srdma_mmio_write16_le()`。不能再使用原先的 32/64-bit packed by-data 编码。

## 5. DPA completion 链路

`dev-be` attach endpoint 后执行：

1. 创建 DPA context、DPA thread 和一个 DevEmu DB completion。
2. 创建 Comch MsgQ；DPA producer 将消息发给 Arm consumer。
3. 对六个 semantic DB 分别调用 `doca_devemu_pci_ep_create_db_on_dpa()`。
4. 创建时传入表中的 hardware DB ID，并把 semantic ID 放入 `user_data`。
5. RPC 将六个 DB handle 绑定到同一个 completion，然后逐个 start。
6. DPA thread 等待 completion，读取 `user_data` 和 16-bit DB value，ack 后发 Comch。
7. Arm consumer 按 semantic ID 分发；AdminQ 调用 `process_adminq_pi()`，其余类型当前
   记录 completion，供控制面和 doorbell smoke 验证。

每个 DB start 时可能产生一次 value=0 的初始 completion。后端只过滤每类 DB 的第一
个初始零值，避免把它误认为 Host 请求；之后的零值（例如 AEQ CI）仍正常上报。

## 6. AdminQ 与 MSI-X

Host probe 把 Admin TX/RX 和 AEQ DMA 地址写入 CFG transaction region，然后写
`DEV_CTRL.START`。`pci-fe` 把 START 参数通过 Gemini 发给 `dev-be`，后端建立 remote
mmap 并 arm AdminQ。

Host 写 AdminQ DB 后，DPA completion 经 Comch 到 Arm，后端 DMA 读取新的 SQE、执行
命令、DMA 写回 RX entry，再通过 control MSI-X object raise vector 0。

MSI-X addr/data 不通过 doorbell value 或 Gemini 消息显式传递。Host 对 MSI-X table 的
写入由 DevEmu endpoint 管理；`dev-be` 用 endpoint 创建 vector object，DOCA 在 raise
时使用该 vector 当前 table entry 的 addr/data，并遵守 mask/PBA 状态。

## 7. 生命周期顺序

启动顺序：

1. 启动 `pci-fe serve`。
2. 启动 `dev-be serve`，等待 Gemini 连接。
3. 执行 `pci-fe plug`。
4. Host rescan 后加载 `srdma.ko`。
5. Host probe 完成，endpoint 从 `PRESENT_STOPPED` 进入 `STARTED`。

销毁时应先让 Host 卸载 `srdma.ko` 并移除 PCI function，再执行 DPU unplug，最后停止
两个 DPU 进程。不要在 Host function 仍存在时直接销毁 endpoint。

## 8. 构建与验收

DPU：

```bash
meson compile -C applications/build \
  doca_vfio_adminq_pci_fe doca_vfio_adminq_dev_be \
  vfio_adminq_srdma_control_test
meson test -C applications/build vfio_adminq_srdma_control --print-errorlogs
```

Host driver 必须显式使用与当前已加载 `ib_core` 匹配的 OFED `Module.symvers`。详见
本目录 `README.md` 的 `OFA_DIR` 说明。

完整控制面和六类 DB 验收：

```bash
./srdma_ctrl_test --device srdma_0 --port 1 --vector 1 \
  --gid-cycle 2001:db8:46::1/128 --doorbell-wqes
```

通过标准：

- 测试结束为 `PASS=59 WARN=0 FAIL=0`。
- `pci-fe status` 为 `STARTED`。
- `dev-be` 日志至少出现 semantic `db_id=0..5`。
- SQ/RQ smoke 只检查 DPA completion，不等待数据面 CQE。
