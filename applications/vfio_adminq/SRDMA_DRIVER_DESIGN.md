# sRDMA Driver 对接设计

本文描述 `vfio_adminq` 当前实现。Host 使用真实 `srdma.ko` 和 libibverbs provider；
DPU 使用 DOCA DevEmu PCI、一个 by-offset doorbell region、DPA completion、Comch 和
Gemini。旧的 UAR TLP ring IPC 已移除；MSI-X 配置使用 io-engine 风格的 SCAN memfd
共享区。

## 1. 目标与边界

当前实现覆盖：

- PCI `1e93:006a` endpoint、单 BAR0、最多 128 个 MSI-X vector；默认全部暴露。
- AdminQ/AEQ DMA 初始化和控制面命令。
- UCTX、PD、MR、EQ、CQ、QP、GID、统计和健康检查。
- AdminQ doorbell 的 DPA completion。
- Host MSI-X table/PBA 和控制向量触发。

当前不实现 RoCE 数据面，也不启用 AEQ/CEQ/CQ/SQ/RQ MMIO doorbell。DOCA 单 BAR
设备跳过 EQ kick 和内核 GSI CQ notify MMIO；RQ 只更新 DMA DB record；SQ post 和
用户态 CQ notify 返回 `EOPNOTSUPP`。

## 2. 组件分工

| 组件 | 职责 |
|---|---|
| `pci-fe` | 创建 PCI type/endpoint，配置 BAR/transaction/DB region，软件处理 MSI-X table/PBA，处理 plug/unplug 和 Gemini server |
| `dev-be` | attach endpoint，运行 Gemini client、AdminQ DMA、资源表、vector 0 MSI-X DMA 和 DPA/Comch doorbell pipeline |
| DPA `doorbell_dev.c` | 等待 DB completion、读取 16-bit value、ack 并经 Comch 发给 Arm |
| `srdma.ko` | 枚举设备、初始化 AdminQ/AEQ、执行控制面 verbs、写 AdminQ DB |
| sRDMA provider | mmap 共享 DB page；数据面 doorbell 当前禁用 |

`pci-fe` 不消费业务 doorbell；`dev-be` 不再依赖 `srdma_uar_ipc`。

## 3. BAR0 布局

布局与 io-engine 当前 BF3 SRDMA 实现保持一致：DOCA BAR0 aperture 为 1 MiB，Host PCI
config 报告 64 KiB BAR0。

| BAR0 offset | size | 类型 | 用途 |
|---:|---:|---|---|
| `0x0000` | `0x1000` | TLP transaction | CFG register、AdminQ/AEQ 地址、状态和诊断区 |
| `0x1000` | `0x0800` | TLP transaction | 软件 MSI-X table，128 entries x 16 bytes |
| `0x1800` | `0x0010` | TLP transaction | 软件 MSI-X PBA，128 bits |
| `0x1810` | `0x67f0` | TLP transaction | 保留，读取为 0、写入忽略 |
| `0x8000` | `0x1000` | by-offset DB region | AdminQ doorbell |

DOCA 只配置 `0x0000..0x7fff` transaction region 和 `0x8000..0x8fff` DB region，
不调用 DOCA MSI-X table/PBA region API，也不创建 DOCA MSI-X vector object。
MSI-X (`0x40/0x0c`) 和 PCIe (`0x50/0x3c`) capability 也完全由 `pci-fe` 软件处理：
PCI type 不调用 `doca_devemu_pci_tlp_type_set_pci_cap_conf()` 注册它们，config TLP
completion 的 `is_cap_id_valid` 固定为 0。否则 Host 驱动写 MSI-X Enable 时，DOCA 会
把该 capability 当作 native MSI-X 元数据处理，而 type 又没有 native table/PBA/vector，
形成不完整的硬件模型。

DB region 参数为：

- `log_db_size = 1`：每个 doorbell 为 2 字节。
- `log_db_stride_size = 3`：相邻 hardware DB ID 间隔 8 字节。
- endpoint `num_db = 1`：仅创建 hardware DB ID 0。

BF3 当前 custom TLP PCI type 要求该 DB region 位于 `0x8000`；实机探测中
`0x10000` 无法启动 type。

## 4. Doorbell ABI

Host 的所有 UAR context 都 mmap 同一个 4 KiB DB page。当前只定义并启用 AdminQ
doorbell；其余 doorbell 的 DOCA ABI 留到数据面实现时确定。

| 语义 ID | 类型 | BAR0 offset | hardware DB ID | 16-bit value |
|---:|---|---:|---:|---|
| 0 | AdminQ | `0x8000` | 0 | Admin TX producer index |
AdminQ 在 DOCA 单 BAR模式下使用内核 `writew()`。标准和 legacy BAR 设备仍使用原有
32/64-bit packed doorbell ABI，不受本控制面适配影响。

## 5. DPA completion 链路

`dev-be` attach endpoint 后执行：

1. 创建 DPA context、DPA thread 和一个 DevEmu DB completion。
2. 创建 Comch MsgQ；DPA producer 将消息发给 Arm consumer。
3. 为 AdminQ 调用 `doca_devemu_pci_ep_create_db_on_dpa()`。
4. 创建时传入 hardware DB ID 0，并把 AdminQ semantic ID 放入 `user_data`。
5. RPC 将 DB handle 绑定到 completion，然后 start。
6. DPA thread 等待 completion，读取 `user_data` 和 16-bit DB value，ack 后发 Comch。
7. Arm consumer 调用 `process_adminq_pi()`。

DB start 时可能产生一次 value=0 的初始 completion。后端过滤这一次初始零值，避免
把它误认为 Host AdminQ 请求。

## 6. AdminQ 与 MSI-X

Host probe 把 Admin TX/RX 和 AEQ DMA 地址写入 CFG transaction region，然后写
`DEV_CTRL.START`。`pci-fe` 把 START 参数通过 Gemini 发给 `dev-be`，后端建立 remote
mmap 并 arm AdminQ。

Host 写 AdminQ DB 后，DPA completion 经 Comch 到 Arm，后端 DMA 读取新的 SQE、执行
命令并 DMA 写回 RX entry。`pci-fe` 按 io-engine 的交互方式，把 Host 写入 vector 0
table 的 address/data 以及 per-vector mask、MSI-X Enable/Function Mask 汇总到共享
`BES2SRDMACfg.msix_vec0`。`dev-be` 按 `srdma-doca-demo/host` 的方式读取稳定快照，检查
address 和 control，然后通过 endpoint remote mmap 把 little-endian `msix_data` 直接
DMA 写到 `msix_addr`。写 MSI-X 前必须先完成 RX entry body 和 owner/header DMA。

PBA 仍作为 Host 可见 PCI ABI 的只读软件区域保留并在复位时清零，但不再用作
`dev-be` 到 `pci-fe` 的通知通道。`pci-fe` 不启用 ACG，也不代发 MSI-X Memory Write。

MSI-X capability 的 table size 和 `SRDMA_BFA_PCI_MAX_VECTORS` 都来自启动参数
`--num-msix`，范围为 `1..128`。backing table 始终保留 128 entries，因此切换向量数
不改变共享 ABI 或 BAR 布局。

Host 的 `pci_alloc_irq_vectors()` 会形成 MSI-X mask/message table 的突发 TLP。TLP
callback 不进行同步日志 I/O，并立即完成 request；主 PE loop 最大 poll 间隔为 1 ms。
fatal callback 只设置恢复标志，channel 的 stop/destroy/recreate 在 callback 返回后
执行，避免在 DOCA 回调上下文中重入生命周期操作。

`pci-fe` 启动时创建 12 KiB memfd。共享内存及 `GEMINI_SCAN` payload 使用 io-engine ABI：

| SCAN 字段 | 当前值 | 含义 |
|---|---:|---|
| `srdma_config.offset/length` | `0x0000/0x1000` | `BES2SRDMACfg[]` 配置页；每个元素为 80 bytes |
| `srdma_config.db_offset` | `0x1000` | SRDMA 共享 doorbell 页 |
| `srdma_config.msix_offset/msix_length` | `0/0` | io-engine 保留字段；后端不得依赖 |

`dev-be` 在 HELLO 后发送 `GEMINI_SCAN`，`pci-fe` 用 `SCM_RIGHTS` 传递 memfd；后端按
PLUG 的 `rvf_id` 从 config 页定位 80-byte `BES2SRDMACfg`。该元素只包含 16-byte
`msix_vec0` 和 64-byte `diag_regs`，不再混入私有 magic/version/table 字段。Host 对
table 的 TLP 写入由 `pci-fe` 的内部 backing storage 处理，并把 vector 0 的
address/data 以及 per-vector mask、MSI-X Enable/Function Mask 汇总到 config 页的
`msix_vec0`。内部 table/PBA 不通过 SCAN 暴露，`dev-be` 只读映射并校验 config 范围，
因此也可直接连接未填写扩展 MSI-X 字段的 io-engine。

后端必须先完成 CQE body 和 owner/header DMA，再读取稳定的 `msix_vec0` 快照并发送
MSI-X DMA。若 address 为 0、未对齐，或 vector/function 被 mask/disable，本次通知
返回错误且不提交 DMA。

每次 DOCA DMA 都受 `--dma-timeout-ms` 约束，默认 5000 ms。超时任务的 user data 和
buffer reference 使用 detached 生命周期，待 completion/error callback 回收；DMA
context 在 buffer inventory 和 mmap 之前停止并销毁，避免超时清理时出现悬空回调。

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

完整控制面验收：

```bash
./srdma_ctrl_test --device srdma_0 --port 1 --vector 1 \
  --gid-cycle 2001:db8:46::1/128
```

通过标准：

- 测试结束为 `WARN=0 FAIL=0`。
- `pci-fe status` 为 `STARTED`。
- `dev-be` 日志出现 AdminQ semantic `db_id=0`，不应出现其他 DB ID。
