# DOCA Samples Run Guide

本文档记录本仓库中 DOCA applications 和 samples 的运行方式。新增条目时，按
`applications/<name>` 或 `samples/<group>/<name>` 增加独立章节；系统权限相关规则统一放在附录。

## 1. 通用环境

### 1.1 角色与路径

| 角色 | 说明 | 仓库路径 |
| --- | --- | --- |
| DPU | BlueField-3 DPU | `/root/ByteDance/doca-samples` |
| Host | BF3 对端 x86 host | `/root/ByteDance/doca-samples` |

按 BF3 DPU 类型选择 Host 登录方式和 PCI 地址。先在 DPU 上执行：

```bash
lspci -Dnn -vv -s 0000:03:00.0 | egrep "Product Name"
```

如果 `Product Name` 中包含 `200GbE`，使用 200GbE 环境：

```bash
HOST_SSH='ssh -o ProxyJump=10.249.181.55 192.168.161.251'
```

| 位置 | 用途 | PCI 地址 |
| --- | --- | --- |
| DPU | DOCA device | `0000:03:00.0` |
| DPU | DOCA ibdev | `mlx5_bond_0` |
| DPU | Host PF representor | `0000:5c:00.0` / `pf0hpf` |
| Host | DOCA device | `0000:5c:00.0` |

如果 `Product Name` 中包含 `100GbE`，使用 100GbE 环境：

```bash
HOST_SSH='ssh 192.168.0.100'
```

| 位置 | 用途 | PCI 地址 / 名称 |
| --- | --- | --- |
| DPU | DOCA device | `0000:03:00.0` |
| DPU | DOCA ibdev | `mlx5_bond_0` |
| DPU | Host PF representor | `0000:3f:00.0` / `pf0hpf` |
| Host | DOCA device | `0000:3f:00.0` |

### 1.2 设备复查命令

复查命令：

```bash
/opt/mellanox/doca/tools/doca_caps --list-devs
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
```

### 1.3 Host BMC/IPMI

Host BMC/IPMI 访问方式与 DPU 类型无关，100GbE/200GbE 环境统一使用：

```bash
BMC_HOST=192.168.1.10
BMC_USER=toutiao
BMC_PASS='toutiao!@#'
```

检查 Host 电源状态：

```bash
ipmitool -I lanplus -H "$BMC_HOST" -U "$BMC_USER" -P "$BMC_PASS" power status
```

## 2. applications/dma_copy

### 2.1 概述

`applications/dma_copy` 使用 DOCA DMA 和 DOCA Comch 在 Host 与 DPU 之间复制文件。两端运行同名二进制：

```bash
/root/ByteDance/doca-samples/applications/build/dma_copy/doca_dma_copy
```

关键规则：

- DPU 端先启动，作为 Comch server 等待 Host client。
- `-f` 指定两端各自的文件路径；文件存在的一端是源端，文件不存在的一端是目标端。
- 两端不能同时存在该文件，也不能两端都不存在。
- DPU 端需要 `-r <representor-pci>`，Host 端不需要。

### 2.2 参数

| 参数 | 说明 |
| --- | --- |
| `-p, --pci-addr` | 本端 DOCA Comch device PCI 地址 |
| `-r, --rep-pci` | DPU 端使用的 representor PCI 地址 |
| `-f, --file` | 源文件或目标文件路径 |
| `-l, --log-level` | 日志级别；调测建议 `60` |

### 2.3 Host -> DPU

变量约定：

```bash
DPU_DEV=0000:03:00.0
DPU_REP=0000:5c:00.0
HOST_DEV=0000:5c:00.0
DPU_DST=/tmp/doca_dma_h2d_dst.txt
HOST_SRC=/tmp/doca_dma_h2d_src.txt
HOST_SSH='ssh -o ProxyJump=10.249.181.55 192.168.163.201'
```

准备文件：

```bash
$HOST_SSH "printf 'hello from host\n' > $HOST_SRC"
rm -f "$DPU_DST"
```

DPU 端先启动：

```bash
cd /root/ByteDance/doca-samples
./applications/build/dma_copy/doca_dma_copy \
  -p "$DPU_DEV" \
  -r "$DPU_REP" \
  -f "$DPU_DST" \
  -l 60
```

Host 端后启动：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./applications/build/dma_copy/doca_dma_copy \
    -p $HOST_DEV \
    -f $HOST_SRC \
    -l 60"
```

校验：

```bash
sha256sum "$DPU_DST"
$HOST_SSH "sha256sum $HOST_SRC"
```

### 2.4 DPU -> Host

变量约定：

```bash
DPU_DEV=0000:03:00.0
DPU_REP=0000:5c:00.0
HOST_DEV=0000:5c:00.0
DPU_SRC=/tmp/doca_dma_d2h_src.txt
HOST_DST=/tmp/doca_dma_d2h_dst.txt
HOST_SSH='ssh -o ProxyJump=10.249.181.55 192.168.163.201'
```

准备文件：

```bash
printf 'hello from dpu\n' > "$DPU_SRC"
$HOST_SSH "rm -f $HOST_DST"
```

DPU 端先启动：

```bash
cd /root/ByteDance/doca-samples
./applications/build/dma_copy/doca_dma_copy \
  -p "$DPU_DEV" \
  -r "$DPU_REP" \
  -f "$DPU_SRC" \
  -l 60
```

Host 端后启动：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./applications/build/dma_copy/doca_dma_copy \
    -p $HOST_DEV \
    -f $HOST_DST \
    -l 60"
```

校验：

```bash
sha256sum "$DPU_SRC"
$HOST_SSH "sha256sum $HOST_DST"
```

### 2.5 常见错误

| 日志/现象 | 处理方式 |
| --- | --- |
| `File was found on both Host and DPU` | 删除目标端文件，只保留源端文件 |
| `File was not found on both Host and DPU` | 在源端创建文件 |
| `Memory range isn't aligned to 64B` | 性能提示，可忽略 |

## 3. applications/vnet_pci_dev

### 3.1 概述

`applications/vnet_pci_dev` 在 DPU 侧通过 DOCA DevEmu/TLP 创建 VirtIO Net PCI 设备，由 Host 在启动阶段枚举为
virtio-net 网卡。

关键规则：

- DPU 端必须先启动 `vnet_pci_dev` 并保持运行。
- 该程序不支持 Host 已经运行后再在线插入设备；不要用 Host 侧 `echo 1 > /sys/bus/pci/rescan` 作为主要测试流程。
- DPU 进程启动后，通过 BMC/IPMI 对 Host 执行 power cycle；Host 在启动 PCI 扫描阶段枚举虚拟 PCI bridge 和 virtio-net endpoint。
- Host 重启完成前不要停止 DPU 端进程，否则 Host 侧设备会消失或初始化失败。

### 3.2 参数

| 参数 | 说明 |
| --- | --- |
| `-p, --pci-addr` | DPU 侧 DOCA device PCI 地址，100GbE/200GbE 当前均为 `0000:03:00.0` |
| `-n, --num-ep` | 创建的 endpoint 数量；单网卡测试使用 `1` |
| `-H, --hotplug-mode` | hotplug 模式开关；启动期枚举测试使用 `0`，即 static mode |
| `-q, --max-queue-pairs` | VirtIO queue pair 数量；基础连通性测试使用 `1` |
| `-z, --queue-size` | VirtQueue size；基础测试使用 `1024` |
| `-m, --mac-addr` | Host 侧 virtio-net MAC 地址 |
| `-t, --mtu` | MTU，默认测试使用 `1500` |
| `-s, --speed` | 上报链路速率，单位 Mbps |
| `-l, --log-level` | 应用日志级别；调测建议 `60` |
| `--sdk-log-level` | DOCA SDK 日志级别；调测建议 `40` |

### 3.3 Host 网络准备

在 100GbE 环境中，Host 启动后 virtio-net 可能占用 `eth0`。为避免原 Host 管理网卡配置被 `eth0` 名称抢占，测试前在
Host 内将 `/etc/network/interfaces` 中的 `eth0` 改为原 Host PF 的稳定接口名：

```bash
$HOST_SSH "sed -r -i -e 's/eth0/enp63s0f0np0/' /etc/network/interfaces"
```

改完后再按下面步骤启动 DPU 进程并重启 Host。

### 3.4 运行步骤

变量约定：

```bash
DPU_DEV=0000:03:00.0
VNET_MAC=52:54:00:12:34:56
```

DPU 端先启动并保持运行：

```bash
cd /root/ByteDance/doca-samples
./applications/build/vnet_pci_dev/doca_vnet_pci_dev \
  -p "$DPU_DEV" \
  -n 1 \
  -H 0 \
  -q 1 \
  -z 1024 \
  -m "$VNET_MAC" \
  -t 1500 \
  -s 100000 \
  -l 60 \
  --sdk-log-level 40
```

如需创建 3 个 virtio-net endpoint，将 `-n 1` 改为 `-n 3`。

等待 DPU 日志出现：

```text
VNet device ready - waiting for host PCIe enumeration...
```

然后通过 BMC 重启 Host：

```bash
ipmitool -I lanplus -H "$BMC_HOST" -U "$BMC_USER" -P "$BMC_PASS" power cycle
```

等待约 3 分钟，Host 启动完成后继续验证。完成 3.3 中的 `sed` 修改后，Host 管理 IP 应继续由
`enp63s0f0np0` 承载；新枚举的 virtio-net 通常为 `eth0`。

### 3.5 校验

Host 侧检查 PCI 设备：

```bash
$HOST_SSH "lspci -Dnn | grep -i '1af4\|virtio'"
```

100GbE 本地调测的期望输出包含：

```text
0000:46:00.0 PCI bridge [0604]: Red Hat, Inc. Device [1af4:10f1]
0000:47:00.0 PCI bridge [0604]: Red Hat, Inc. Device [1af4:10f1]
0000:48:00.0 Ethernet controller [0200]: Red Hat, Inc. Virtio 1.0 network device [1af4:1041]
```

Host 侧检查驱动和网卡：

```bash
$HOST_SSH "lspci -Dnnk -s 0000:48:00.0"
$HOST_SSH "ip -br addr"
$HOST_SSH "ip -o link show eth0"
```

期望状态：

- `0000:48:00.0` 使用 `virtio-pci` 驱动。
- Host `eth0` 为 `UP,LOWER_UP`。
- Host `eth0` MAC 为 DPU 启动参数 `-m` 指定值，例如 `52:54:00:12:34:56`。
- Host 管理 IP `192.168.0.100/24` 仍在 `enp63s0f0np0` 上，而不是被 virtio-net `eth0` 抢占。

给 virtio 链路配置临时测试 IP 并做连通性检查：

```bash
ip addr replace 192.168.100.1/24 dev en3f0pf0sf1000
ip link set en3f0pf0sf1000 up

$HOST_SSH "ip addr replace 192.168.100.2/24 dev eth0 && sudo ip link set eth0 up"
$HOST_SSH "ping -c 3 -W 2 192.168.100.1"
ping -I en3f0pf0sf1000 -c 3 -W 2 192.168.100.2
```

双向 ping 应为 `0% packet loss`。

### 3.6 三 endpoint 设备关系

创建 3 个 endpoint 后，DPU 侧会出现 3 组 SF 相关接口。
`en3f0pf0sf<N>` 是 SF representor，位于 PF0 的 embedded switch 上。
`enp3s0f0s<N>` 是对应 SF function 自己的 netdev，挂在 `mlx5_core.sf.*` auxiliary 设备下。

当前 100GbE 本地调测环境中的对应关系如下：

| Endpoint | SF 编号 | DPU representor | SF function netdev |
| --- | --- | --- | --- |
| 0 | `1000` | `en3f0pf0sf1000` | `enp3s0f0s1000` |
| 1 | `1001` | `en3f0pf0sf1001` | `enp3s0f0s1001` |
| 2 | `1002` | `en3f0pf0sf1002` | `enp3s0f0s1002` |

用 `devlink` 查看 DPU 侧 port 和 SF 关系：

```bash
devlink port show
```

期望能看到 SF representor 侧信息：

```text
pci/0000:03:00.0/163872: type eth netdev en3f0pf0sf1000 flavour pcisf controller 0 pfnum 0 sfnum 1000
pci/0000:03:00.0/163873: type eth netdev en3f0pf0sf1001 flavour pcisf controller 0 pfnum 0 sfnum 1001
pci/0000:03:00.0/163874: type eth netdev en3f0pf0sf1002 flavour pcisf controller 0 pfnum 0 sfnum 1002
```

同时也应能看到 SF function netdev 侧 auxiliary 设备：

```text
auxiliary/mlx5_core.sf.2/5898240: type eth netdev enp3s0f0s1000 flavour virtual
auxiliary/mlx5_core.sf.3/5963776: type eth netdev enp3s0f0s1001 flavour virtual
auxiliary/mlx5_core.sf.4/6029312: type eth netdev enp3s0f0s1002 flavour virtual
```

### 3.7 常见错误

| 日志/现象 | 处理方式 |
| --- | --- |
| Host 已运行时在线 rescan 只能看到 bridge 或枚举失败 | 这是非预期流程；保持 DPU 进程运行后对 Host 执行 BMC power cycle |
| Host 重启后 SSH 不通 | 先确认 `/etc/network/interfaces` 已将原 `eth0` 改为 `enp63s0f0np0`，避免 virtio-net 抢占 `eth0` 影响管理网 |
| `lspci` 看不到 `1af4:1041` endpoint | 确认 DPU 进程在 Host power cycle 前已启动并保持运行 |
| DPU 日志没有 Host config read/write TLP | 确认 Host 确实从 BMC 重启，且不是只做了在线 PCI rescan |

## 4. applications/vblk_pci_dev

### 4.1 概述

`applications/vblk_pci_dev` 在 DPU 侧通过 DOCA DevEmu/TLP 创建 VirtIO Block PCI 设备，由 Host 在启动阶段枚举为
virtio-blk 块设备。

关键规则：

- DPU 端必须先启动 `vblk_pci_dev` 并保持运行。
- 基础测试使用 static mode，即 `-H 0`；不要把 Host 侧在线 `echo 1 > /sys/bus/pci/rescan` 作为主要测试流程。
- DPU 进程启动后，通过 BMC/IPMI 对 Host 执行 power cycle；Host 在启动 PCI 扫描阶段枚举虚拟 PCI bridge 和
  virtio-blk endpoint。
- `vblk_pci_dev` 与 `vnet_pci_dev` 会竞争同类 DevEmu/TLP 资源；启动前确认没有其他 vnet/vblk emulation 进程占用资源。
- 当前源码读写后端仍是占位实现，适合验证 PCI 枚举、virtio-pci 绑定、请求收发和容量通知；不要按真实磁盘做写后读
  一致性校验。

### 4.2 参数

| 参数 | 说明 |
| --- | --- |
| `-d, --emulation-manager` | DPU 侧 emulation manager mlx5 ibdev 名称；当前 100GbE/200GbE 环境为 `mlx5_bond_0` |
| `-n, --num-ep` | 创建的 endpoint 数量；单盘测试使用 `1` |
| `-H, --hotplug-mode` | hotplug 模式开关；启动期枚举测试使用 `0`，即 static mode |
| `-q, --num-queues` | 每个 endpoint 的 virtio queue 数量；基础测试使用 `1` |
| `--io-ctx-mask` | IO context CPU mask；基础测试使用 `0x1`，即 core 0 |
| `--tlp-core-idx` | TLP 线程 CPU core；不能包含在 `--io-ctx-mask` 中，基础测试使用 `15` |
| `--offload-engine-core-idx` | offload engine CPU core；必须包含在 `--io-ctx-mask` 中，基础测试使用 `0` |
| `--provider` | 数据路径 provider，`DPA` 或 `DPU`；基础测试优先使用默认/显式 `DPA` |
| `-l, --log-level` | 应用日志级别；调测建议 `60` |
| `--sdk-log-level` | DOCA SDK 日志级别；调测建议 `40` |

### 4.3 运行步骤

变量约定：

```bash
DPU_IBDEV=mlx5_bond_0
```

启动前确认没有残留的 vnet/vblk emulation 进程占用 DevEmu/TLP 资源；如有，先停止残留进程。

DPU 端启动并保持运行：

```bash
cd /root/ByteDance/doca-samples
./applications/build/vblk_pci_dev/doca_vblk_pci_dev \
  -d "$DPU_IBDEV" \
  -n 1 \
  -H 0 \
  -q 1 \
  --io-ctx-mask 0x1 \
  --tlp-core-idx 15 \
  --offload-engine-core-idx 0 \
  --provider DPA \
  -l 60 \
  --sdk-log-level 40
```

等待 DPU 日志出现：

```text
VBlk device initialized successfully (1 EPs)
All is ready (1 EPs, 1 queues/EP), running progress loop
```

然后通过 BMC 重启 Host：

```bash
ipmitool -I lanplus -H "$BMC_HOST" -U "$BMC_USER" -P "$BMC_PASS" power cycle
```

DPU 日志应看到 Host reset 和 PCI 枚举过程，例如：

```text
PERST# is asserted (enters reset)
PERST# is deasserted (released from reset)
Device BDF set: 46:00.0 (bridge)
Device BDF set: 47:00.0 (bridge)
Device BDF set: 48:00.0 (endpoint)
```

等待 Host 启动完成后继续验证。

### 4.4 校验

Host 侧检查 PCI 设备和驱动：

```bash
$HOST_SSH "lspci -Dnn | grep -i '1af4\|virtio'"
$HOST_SSH "lspci -Dnnk -s 0000:48:00.0"
```

100GbE 本地调测的期望输出包含：

```text
0000:46:00.0 PCI bridge [0604]: Red Hat, Inc. Device [1af4:10f1]
0000:47:00.0 PCI bridge [0604]: Red Hat, Inc. Device [1af4:10f1]
0000:48:00.0 Non-Volatile memory controller [0108]: Red Hat, Inc. Virtio 1.0 block device [1af4:1042]
Kernel driver in use: virtio-pci
```

Host 侧检查块设备：

```bash
$HOST_SSH "lsblk -o NAME,TYPE,SIZE,MODEL,SERIAL"
$HOST_SSH "udevadm info --query=all --name=/dev/vda | egrep 'ID_SERIAL|ID_PATH|DEVPATH' || true"
$HOST_SSH "blockdev --getsize64 /dev/vda"
```

期望状态：

- Host 新增 `/dev/vda`。
- `SERIAL` 或 `ID_SERIAL` 为 `vblk_bdev0`。
- 默认容量为 `1073741824` 字节，即 1GiB。
- `0000:48:00.0` 使用 `virtio-pci` 驱动。

做最小 I/O smoke test：

```bash
$HOST_SSH "dd if=/dev/vda of=/dev/null bs=4K count=16 iflag=direct status=none && echo read_ok"
$HOST_SSH "dd if=/dev/zero of=/dev/vda bs=4K count=16 oflag=direct status=none && sync && echo write_ok"
```

`read_ok` 和 `write_ok` 表示 virtio-blk 请求能成功完成。由于当前读写后端没有接真实 bdev，不要使用写后读内容一致性作为
通过条件。

### 4.5 运行时容量测试

DPU 侧 `vblk_pci_dev` stdin 支持 `cap <GB>`。例如设置 2GiB：

```text
cap 2
```

DPU 日志应出现：

```text
Block device capacity updated: 2147483648 bytes (4194304 sectors)
```

Host 侧检查容量：

```bash
$HOST_SSH "blockdev --rereadpt /dev/vda || true"
$HOST_SSH "blockdev --getsize64 /dev/vda"
$HOST_SSH "lsblk -o NAME,TYPE,SIZE,MODEL,SERIAL /dev/vda"
```

期望 `/dev/vda` 更新为 `2147483648` 字节，即 2GiB。

### 4.6 常见错误

| 日志/现象 | 处理方式 |
| --- | --- |
| `tlp_channel_start_cb failed: Failed to create VAR` | 检查并停止残留 `doca_vnet_pci_dev`/`doca_vblk_pci_dev`，同类 DevEmu/TLP 资源不能被多个进程同时占用 |
| Host 已运行时在线 rescan 只能看到 bridge 或枚举失败 | 保持 DPU 进程运行后对 Host 执行 BMC power cycle |
| `lspci` 看不到 `1af4:1042` endpoint | 确认 DPU 进程在 Host power cycle 前已启动并保持运行，且 DPU 日志出现 Host config read/write TLP |
| Host 重启后 SSH 不通 | 确认 Host 管理 IP 仍在物理 PF/稳定接口上；100GbE 环境实测为 `192.168.0.100/24` 在 `eth1` |
| 写入 `/dev/vda` 后读回内容不一致 | 当前样例读写后端仍是占位实现，这是预期限制；只用 direct read/write 成功返回作为 smoke test |
| `--provider DPA` 初始化失败 | 改用 `--provider DPU` 复测，并保留完整 DOCA 日志继续定位 |

## 5. applications/nvme_emulation

### 5.1 概述

`applications/nvme_emulation` 在 DPU 侧运行 SPDK target，并通过 DOCA DevEmu PCI Generic API 创建 NVMe PCIe
function。Host 侧会在线枚举为 NVMe 控制器和块设备。

关键规则：

- DPU 端运行 `doca_nvme_emulation` 后，后续配置通过 SPDK RPC 完成。
- DOCA 相关 RPC 由 `applications/nvme_emulation/rpc_nvmf_doca.py` 提供，调用 `spdk_rpc.py` 时需要设置
  `PYTHONPATH` 并加载 `--plugin rpc_nvmf_doca`。
- 当前 100GbE 本地环境已有 1GB hugepage mount `/dev/hugepages`；启动参数应使用 `--huge-dir /dev/hugepages`。
- Host 侧基础 I/O 测试只操作新枚举的 64MB malloc 后端测试盘，不要误写真实 NVMe 盘。
- 正常清理顺序是：Host 先移除 PCI function，DPU 再 remove listener，确认 QP 归零后再 destroy function、
  delete subsystem/bdev，最后停止 target。不要在 Host NVMe controller 还活着时直接 destroy function。

### 5.2 参数与变量

`doca_nvme_emulation` 使用标准 SPDK app 参数，DOCA emulation manager、function 和 listener 通过 RPC 配置。

| 参数/变量 | 说明 |
| --- | --- |
| `-m` | SPDK/DPDK core mask；基础测试使用 `0x3` |
| `-s` | DPDK memory size，单位 MB；基础测试使用 `1024` |
| `--huge-dir` | hugetlbfs mount；当前本地调测使用 `/dev/hugepages` |
| `-r` | SPDK RPC socket；建议使用 `/var/tmp/doca_nvme.sock` |
| `DEV_NAME` | DOCA emulation manager 名称；当前 100GbE 本地调测为 `mlx5_bond_0` |
| `NQN` | SPDK NVMe-oF subsystem NQN；示例使用 `nqn.2016-06.io.spdk:cnode1` |
| `VUID` | `nvmf_doca_create_function` 返回的 emulated function 标识，后续作为 DOCA listener 地址 |

### 5.3 启动前检查

DPU 侧确认没有残留 target 进程和旧 SPDK RPC socket：

```bash
ps -eo pid,ppid,stat,comm,args | grep '[d]oca_nvme_emulation' || true
rm -f /var/tmp/doca_nvme.sock
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
```

Host 侧确认没有前次 emulated NVMe 残留：

```bash
$HOST_SSH "sh -c 'lspci -Dnn | egrep -i \"15b3:6001|NVMe SNAP\" || true; \
  ls -l /dev/nvme2 /dev/nvme2n1 2>/dev/null || true;"
```

干净状态下，Host 不应看到 `15b3:6001` 或 `/dev/nvme2n1`。

### 5.4 启动 target

DPU 端需要让 `doca_nvme_emulation` 在整个 Host 枚举和 I/O 测试期间持续运行。推荐使用 `setsid` 做长时间/非交互
运行：进程脱离当前 shell/session，stdin 绑定 `/dev/null`，stdout/stderr 写入日志文件。

不要只用普通 `cmd &` 后台运行；shell/SSH 退出时进程可能收到 `SIGHUP`，造成 target 退出但 socket 文件残留。

```bash
cd /root/ByteDance/doca-samples
mkdir -p /tmp/doca_nvme_run
rm -f /var/tmp/doca_nvme.sock /tmp/doca_nvme_run/doca_nvme.log /tmp/doca_nvme_run/doca_nvme.pid

setsid ./applications/build/nvme_emulation/doca_nvme_emulation \
  -m 0x3 \
  -s 1024 \
  --huge-dir /dev/hugepages \
  -r /var/tmp/doca_nvme.sock \
  -L nvmf \
  -L bdev \
  -L nvme \
  </dev/null >/tmp/doca_nvme_run/doca_nvme.log 2>&1

echo $! > /tmp/doca_nvme_run/doca_nvme.pid
```

后台启动后检查进程、socket 和日志：

```bash
cat /tmp/doca_nvme_run/doca_nvme.pid
ps -p "$(cat /tmp/doca_nvme_run/doca_nvme.pid)" -o pid,ppid,stat,comm,args
ss -xl | grep doca_nvme
tail -80 /tmp/doca_nvme_run/doca_nvme.log
```

启动日志中可能出现如下 mlx5 accel 初始化错误：

```text
Failed to create PSV memory pool
Failed to init accel module mlx5, ignoring it
```

只要 SPDK reactor 继续运行、RPC socket 监听正常，这个错误不影响本节基础 NVMe emulation smoke test。

另开 DPU 终端设置 SPDK_RPC 变量并检查连通性：

```bash
cd /root/ByteDance/doca-samples

SPDK_RPC='env PYTHONPATH=/root/ByteDance/doca-samples/applications/nvme_emulation /usr/bin/spdk_rpc.py -s /var/tmp/doca_nvme.sock --plugin rpc_nvmf_doca'

$SPDK_RPC spdk_get_version
$SPDK_RPC framework_get_reactors
$SPDK_RPC nvmf_doca_get_managers
```

期望 `nvmf_doca_get_managers` 输出包含：

```json
[
  {
    "name": "mlx5_bond_0"
  }
]
```

### 5.5 SPDK RPC 配置 NVMe 设备

变量约定：

```bash
NQN='nqn.2016-06.io.spdk:cnode1'
DEV_NAME='mlx5_bond_0'
```

创建 DOCA transport、64MB malloc bdev、subsystem 和 namespace：

```bash
$SPDK_RPC nvmf_create_transport -t doca
$SPDK_RPC bdev_malloc_create 64 512 -b Malloc0
$SPDK_RPC nvmf_create_subsystem "$NQN" -a -s DOCA000000000001
$SPDK_RPC nvmf_subsystem_add_ns "$NQN" Malloc0
```

创建 emulated function，并记录返回的 VUID：

```bash
$SPDK_RPC nvmf_doca_create_function -d "$DEV_NAME"
```

本地 100GbE 调测返回示例：

```json
{
  "Created a function with vuid": "MT2529603G38GES2D0F0"
}
```

将该 VUID 作为 listener 地址执行 hotplug：

```bash
VUID=MT2529603G38GES2D0F0

$SPDK_RPC nvmf_subsystem_add_listener "$NQN" -t doca -a "$VUID"
$SPDK_RPC nvmf_doca_list_functions -d "$DEV_NAME"
$SPDK_RPC nvmf_get_subsystems
```

本地 100GbE 调测中，`nvmf_doca_list_functions` 输出包含：

```json
[
  {
    "Function VUID: ": "MT2529603G38GES2D0F0",
    "PCI Address: ": "0000:41:00.0"
  }
]
```

`nvmf_get_subsystems` 中应看到 `transport/trtype` 为 `DOCA`，`traddr` 为上面的 VUID，namespace 后端为 `Malloc0`。

### 5.6 Host 侧校验与 fio

Host 侧检查新 PCI 设备。100GbE 本地调测中，emulated NVMe endpoint 为 `0000:41:00.0`：

```bash
$HOST_SSH "lspci -Dnnk -s 0000:41:00.0"
```

期望输出包含：

```text
0000:41:00.0 Non-Volatile memory controller [0108]: Mellanox Technologies NVMe SNAP Controller [15b3:6001]
Kernel driver in use: nvme
```

Host 侧检查块设备：

```bash
$HOST_SSH "nvme list"
$HOST_SSH "blockdev --getsize64 /dev/nvme2n1"
$HOST_SSH "lsblk /dev/nvme2n1"
```

本地 100GbE 调测结果：

```text
/dev/nvme2n1  DOCA000000000001  SPDK bdev Controller  67.11 MB / 67.11 MB
67108864
```

做 identify 检查：

```bash
$HOST_SSH "nvme id-ctrl /dev/nvme2 | head -80"
$HOST_SSH "nvme id-ns /dev/nvme2n1 | head -80"
```

期望关键信息：

- controller serial number 为 `DOCA000000000001`。
- model 为 `SPDK bdev Controller`。
- `subnqn` 为 `nqn.2016-06.io.spdk:cnode1`。
- namespace `nsze/ncap/nuse` 为 `0x20000`，对应 64MB、512B block。

使用 `fio` 做 32MB raw block 写入和校验读，只操作新枚举的 `/dev/nvme2n1`：

```bash
$HOST_SSH "fio \
  --name=doca_nvme_fio_verify \
  --filename=/dev/nvme2n1 \
  --direct=1 \
  --ioengine=libaio \
  --rw=write \
  --bs=4k \
  --iodepth=8 \
  --numjobs=1 \
  --size=32M \
  --verify=crc32c \
  --do_verify=1 \
  --verify_fatal=1 \
  --group_reporting"
```

期望 `fio` 返回 `err=0`，并看到读写各 32MiB。100GbE 本地调测示例：

```text
fio-3.33
doca_nvme_fio_verify: (groupid=0, jobs=1): err= 0
READ:  io=32.0MiB
WRITE: io=32.0MiB
issued rwts: total=8192,8192,0,0
```

fio 完成后，DPU 侧可查看 SPDK 统计：

```bash
$SPDK_RPC bdev_get_iostat -b Malloc0
$SPDK_RPC nvmf_get_stats
```

期望 `Malloc0` 统计中至少包含 32MiB 写入和校验读，`nvmf_get_stats` 中 `pending_bdev_io` 为 `0`，且无 `io_error`。

### 5.7 正常关停与清理

清理顺序很重要。必须先让 Host NVMe driver 释放 controller 和队列，再让 DPU 侧销毁 listener/function。

Host 侧确认没有用户态 I/O 进程，然后移除 PCI function：

```bash
$HOST_SSH "sh -c 'fuser -v /dev/nvme2n1 2>/dev/null || true; \
  ps -eo pid,stat,comm,args | egrep \"fio|nvme\" | grep -v egrep || true; \
  lspci -Dnnk -s 0000:41:00.0 || true'"

$HOST_SSH "sh -c 'echo 1 > /sys/bus/pci/devices/0000:41:00.0/remove; \
  sleep 2; \
  lspci -Dnnk -s 0000:41:00.0 || true; \
  ls -l /dev/nvme2 /dev/nvme2n1 2>/dev/null || true; \
  nvme list 2>/dev/null | egrep \"DOCA000000000001|nvme2\" || true'"
```

期望 `0000:41:00.0`、`/dev/nvme2`、`/dev/nvme2n1` 都消失。

DPU 侧 remove listener，并确认当前 QP 归零：

```bash
$SPDK_RPC nvmf_subsystem_remove_listener "$NQN" -t doca -a "$VUID"
sleep 3
$SPDK_RPC nvmf_get_stats
$SPDK_RPC nvmf_get_subsystems
```

期望 `nvmf_get_stats` 中 `current_admin_qpairs=0`、`current_io_qpairs=0`、`pending_bdev_io=0`，并且 subsystem
的 `listen_addresses` 为空。

DPU 侧销毁 function，删除 subsystem 和 bdev：

```bash
$SPDK_RPC nvmf_doca_destroy_function -d "$DEV_NAME" -v "$VUID"
$SPDK_RPC nvmf_doca_list_functions -d "$DEV_NAME"

$SPDK_RPC nvmf_delete_subsystem "$NQN"
$SPDK_RPC bdev_malloc_delete Malloc0

$SPDK_RPC nvmf_get_subsystems
$SPDK_RPC bdev_get_bdevs
```

期望：

- `nvmf_doca_list_functions` 返回 `[]`。
- `nvmf_get_subsystems` 只剩 discovery subsystem。
- `bdev_get_bdevs` 返回 `[]`。

停止 target 并清理 socket：

```bash
$SPDK_RPC spdk_kill_instance SIGTERM || true
sleep 3
ps -eo pid,ppid,stat,comm,args | grep '[d]oca_nvme_emulation' || true
rm -f /var/tmp/doca_nvme.sock
```

如果按上述后台方式启动且 RPC 已不可用，可用 pidfile 做兜底停止：

```bash
kill -TERM "$(cat /tmp/doca_nvme_run/doca_nvme.pid)" 2>/dev/null || true
sleep 3
ps -p "$(cat /tmp/doca_nvme_run/doca_nvme.pid)" -o pid,ppid,stat,comm,args 2>/dev/null || true
rm -f /var/tmp/doca_nvme.sock /tmp/doca_nvme_run/doca_nvme.pid
```

退出日志中可能出现多条：

```text
doca_devemu_pci_db_stop failed: Doorbell is already stopped
```

只要后续日志出现 `Destroyed PCI dev poll group`、`Shut down controller`、hotplug state `POWER_OFF`，且 target 正常退出，
该重复 stop 日志可按非致命清理日志处理。

### 5.8 最终确认

DPU 侧确认 emulated function 和 target 进程无残留：

```bash
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
ps -eo pid,ppid,stat,comm,args | grep '[d]oca_nvme_emulation' || true
ls -l /var/tmp/doca_nvme.sock 2>&1 || true
```

正常清理后，`doca_caps --list-rep-devs` 不应再包含本次 VUID `MT2529603G38GES2D0F0` / representor
`0000:41:00.0`，`doca_nvme_emulation` 进程和 `/var/tmp/doca_nvme.sock` 都应消失。

Host 侧确认无 emulated NVMe 残留：

```bash
$HOST_SSH "sh -c 'lspci -Dnn | egrep -i \"15b3:6001|NVMe SNAP\" || true; \
  ls -l /dev/nvme2 /dev/nvme2n1 2>/dev/null || true; \
  nvme list 2>/dev/null | egrep \"DOCA000000000001|nvme2\" || true; \
  ps -eo pid,stat,comm,args | egrep \"fio|nvme list|remove\" | grep -v egrep || true'"
```

正常清理后，上述命令不应输出 emulated NVMe 设备或卡住的测试进程。

### 5.9 常见错误

| 日志/现象 | 处理方式 |
| --- | --- |
| RPC 报 `Connection refused` | 确认 `doca_nvme_emulation` 进程仍在运行，且 `/var/tmp/doca_nvme.sock` 是正在监听的 socket；崩溃后 socket 文件可能残留 |
| Host `nvme list` 卡住 | 确认 DPU target 是否仍在；target crash 后 Host NVMe driver 可能等待后端响应 |
| 清理时 target abort 或 Host 残留 `0000:41:00.0` | 检查是否跳过了 Host sysfs remove 或没有等待 QP 归零；按 5.7 顺序重做 |
| `nvmf_doca_destroy_function` 报 representor 仍关联 emulated device | Host 侧 controller 尚未释放或 DPU listener/QP 未完全停止；先 Host `remove`，再 `nvmf_subsystem_remove_listener`，确认 `current_*_qpairs=0` 后重试 |

## 6. samples/doca_devemu generic PCI samples

### 6.1 适用范围与公共约定

本章覆盖一组基于 `devemu_pci_type_config.h` 的 generic PCI DevEmu samples。它们分两类：

| 类别 | sample | 目的 | 端侧 |
| --- | --- | --- | --- |
| endpoint 管理 | `devemu_pci_device_list` | 列出当前 generic emulated PCI devices，并打印 VUID/representor PCI | DPU only |
| endpoint 管理 | `devemu_pci_device_hotplug` | 创建并 hotplug 新 generic endpoint；传入 VUID 时 hot-unplug 旧 endpoint | DPU only |
| 功能验证 | `devemu_pci_device_db` | Host 写 BAR doorbell，DPU 收 doorbell value | DPU + Host |
| 功能验证 | `devemu_pci_device_msix` | DPU raise MSI-X，Host eventfd 收中断 | DPU + Host |
| 功能验证 | `devemu_pci_device_dma` | DPU 通过 DMA 读写 Host 暴露的 buffer | DPU + Host |
| 功能验证 | `devemu_pci_device_stateful_region` | Host 写 stateful region，DPU 收 write event | DPU + Host |
| 功能验证 | `devemu_pci_device_tlp_handler` | DPU 处理 raw PCIe TLP，Host 访问 transaction region | DPU + Host |

公共规则：

- DB/DMA/stateful region/MSI-X 都依赖一个已经 hotplug 且处于 power-on 状态的 generic emulated PCI endpoint，并通过
  `-u "$EMU_VUID"` 指定该 endpoint。
- `EMU_VUID` 是运行时 emulated device 的对象标识，不是 BF3 硬件固定值；BF3/DPU 重启后可能不存在，需要重新枚举或重新
  hotplug 创建。
- TLP handler 不复用 `EMU_VUID`；它会创建或复用自己的 TLP representor。
- Host 端功能验证 samples 都通过 VFIO 打开 emulated endpoint，因此运行前需要绑定 `vfio-pci`。
- 同一时间不要运行多个占用同一个 generic emulated endpoint 的 DevEmu sample。
- Host VFIO open/close 可能触发 DPU 端 FLR 日志；除非 sample 明确失败，否则按预期现象处理。

### 6.2 公共编译方式

先编译 endpoint 管理 samples；它们用于获取或创建 `EMU_VUID`：

```bash
cd /root/ByteDance/doca-samples

SAMPLE=samples/doca_devemu/devemu_pci_device_list
meson setup "$SAMPLE/build" "$SAMPLE"
ninja -C "$SAMPLE/build"

SAMPLE=samples/doca_devemu/devemu_pci_device_hotplug
meson setup "$SAMPLE/build" "$SAMPLE"
ninja -C "$SAMPLE/build"
```

功能验证 samples 按 DPU/Host 两端分别编译。下面命令中的 `<sample-name>` 替换为具体 sample 目录名，例如
`devemu_pci_device_dma`。

DPU 端：

```bash
cd /root/ByteDance/doca-samples
SAMPLE=samples/doca_devemu/<sample-name>
meson setup "$SAMPLE/dpu/build" "$SAMPLE/dpu"
ninja -C "$SAMPLE/dpu/build"
```

Host 端：

```bash
SAMPLE=samples/doca_devemu/<sample-name>
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  meson setup $SAMPLE/host/build $SAMPLE/host && \
  ninja -C $SAMPLE/host/build"
```

功能验证 sample 的二进制命名规律：

```text
samples/doca_devemu/<sample-name>/dpu/build/doca_<sample-name>_dpu
samples/doca_devemu/<sample-name>/host/build/doca_<sample-name>_host
```

### 6.3 选择或创建 generic emulated endpoint

变量约定：

```bash
DPU_DEV=0000:03:00.0
EMU_VUID=<从当前环境枚举或 hotplug 输出获取>
HOST_EP=<Host 上当前 emulated endpoint BDF，例如 0000:40:00.0>
HOST_VFIO_GROUP=<HOST_EP 所在 IOMMU group，例如 44>
```

DPU 端确认 DOCA device：

```bash
/opt/mellanox/doca/tools/doca_caps --list-devs
```

列出当前 generic emulated PCI devices：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_list/build/doca_devemu_pci_device_list \
  -p "$DPU_DEV" \
  -l 60 \
  --sdk-log-level 40
```

也可以用 `doca_caps` 复查 representor：

```bash
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
```

如果已有 `rep_type EMULATED` generic endpoint，把当前输出中的 VUID 填入 `EMU_VUID`。例如下面只是一次本地实测输出，
不保证重启后仍存在：

```text
representor-PCI: 0000:40:00.0
    hotplug yes
    vuid MT2529603G38GES1D0F0
    rep_type EMULATED
```

```bash
EMU_VUID=<当前枚举到的 vuid>
```

如果列表为空，创建并 hotplug 一个新的 generic emulated PCI device：

```bash
./samples/doca_devemu/devemu_pci_device_hotplug/build/doca_devemu_pci_device_hotplug \
  -p "$DPU_DEV" \
  -l 60 \
  --sdk-log-level 40
```

记录输出中的新 VUID：

```text
The new emulated device VUID: <new-vuid>
```

然后重新枚举 DPU representor 和 Host endpoint：

```bash
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
$HOST_SSH "lspci -Dnn | grep -i '15b3:1021'"
$HOST_SSH "basename \$(readlink /sys/bus/pci/devices/<Host endpoint BDF>/iommu_group)"
```

设置当前测试变量：

```bash
EMU_VUID=<new-vuid>
HOST_EP=<Host lspci 中的 endpoint BDF>
HOST_VFIO_GROUP=<上一步 basename 输出，例如 44>
```

需要删除该 emulated endpoint 时，确保没有功能验证 sample 正在使用它，然后执行：

```bash
./samples/doca_devemu/devemu_pci_device_hotplug/build/doca_devemu_pci_device_hotplug \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -l 60 \
  --sdk-log-level 40
```

只绑定 `15b3:1021` emulated endpoint，不要绑定 Host 物理 PF `0000:3f:00.0` / `0000:5c:00.0`。

### 6.4 Host VFIO 准备

Host sample 运行前，将 emulated endpoint 绑定到 `vfio-pci`：

```bash
$HOST_SSH "modprobe vfio-pci && \
  printf vfio-pci > /sys/bus/pci/devices/$HOST_EP/driver_override && \
  printf $HOST_EP > /sys/bus/pci/drivers_probe && \
  lspci -Dnnk -s $HOST_EP && \
  ls -l /dev/vfio/$HOST_VFIO_GROUP"
```

期望 `lspci` 显示：

```text
Kernel driver in use: vfio-pci
```

如果 Host sample 报错：

```text
Failed to set IOMMU type 1 extension for container. Status=-1, errno=1
```

并且 Host `dmesg` 中有：

```text
No interrupt remapping support. Use the module param "allow_unsafe_interrupts" to enable VFIO IOMMU support on this platform
```

说明当前 Host 可能以 `intremap=off` 启动。仅做临时 smoke test 时可打开：

```bash
$HOST_SSH "printf 1 > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts && \
  cat /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts"
```

完成测试后必须按 6.5 清理并恢复该参数。

### 6.5 公共清理

Host 侧解除 `vfio-pci` 绑定并清除 driver override：

```bash
$HOST_SSH "if [ -e /sys/bus/pci/drivers/vfio-pci/$HOST_EP ]; then \
    printf $HOST_EP > /sys/bus/pci/drivers/vfio-pci/unbind; \
  fi; \
  : > /sys/bus/pci/devices/$HOST_EP/driver_override; \
  lspci -Dnnk -s $HOST_EP"
```

如果测试中临时打开了 `allow_unsafe_interrupts`，恢复为 `N`：

```bash
$HOST_SSH "printf 0 > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts && \
  cat /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts"
```

期望输出：

```text
N
```

如果测试前 Host endpoint 本来没有 `Kernel driver in use`，清理后解除 `vfio-pci` 并清空 `driver_override` 即可。不要强制把该
emulated endpoint 绑定到 Host 物理 PF 驱动；本地实测对 `mlx5_core/bind` 写入该 endpoint 返回 `I/O error`，恢复到无绑定状态
即可。

如果 Host 上存在前次测试残留的 `doca_devemu_pci_*` 进程或卡在 `drivers_probe` 的 shell，先清理残留再绑定 VFIO。

### 6.6 devemu_pci_device_db

`devemu_pci_device_db` 验证 Host driver 写 PCI BAR doorbell，DPU/BlueField 侧通过 DPA doorbell completion 收到
doorbell value。

特有参数：

| 参数 | 端侧 | 说明 |
| --- | --- | --- |
| `-u, --vuid` | DPU | emulated PCI device VUID |
| `-r, --region-index` | DPU/Host | DB region index；基础测试使用 `0` |
| `-i, --db-id` | DPU | DPU 端监听的 DB ID；基础测试使用 `0` |
| `-d, --db-index` | Host | Host 端写入的 doorbell index；与 DPU `--db-id` 对齐 |
| `-w, --db-value` | Host | Host 写入的 4B doorbell value |

运行步骤：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_db/dpu/build/doca_devemu_pci_device_db_dpu \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -r 0 \
  -i 0 \
  -l 60 \
  --sdk-log-level 40
```

等待 DPU 日志出现：

```text
Listening on DB with ID 0
```

Host 端写 doorbell：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_db/host/build/doca_devemu_pci_device_db_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -r 0 \
    -d 0 \
    -w 5678 \
    -l 60 \
    --sdk-log-level 40"
```

通过条件：

- DPU 端进入 `Listening on DB with ID 0`。
- Host 端打印 `Wrote a DB value of 5678 ...` 并成功退出。
- DPU 端打印 `Received Doorbell value is 5678`。
- DPU 端按 `Ctrl+c` 后打印 `Sample finished successfully`。

### 6.7 devemu_pci_device_msix

`devemu_pci_device_msix` 验证 DPU/BlueField 侧对已有 generic emulated PCI device raise MSI-X vector，Host driver
通过 VFIO eventfd 收到中断事件。

当前 MSI-X 配置：

```text
num_vectors=4
MSI-X table: BAR0 offset 0x1000
PBA:         BAR0 offset 0x2000
```

特有参数：

| 参数 | 端侧 | 说明 |
| --- | --- | --- |
| `-u, --vuid` | DPU | emulated PCI device VUID |
| `-x, --msix-index` | DPU | 要 raise 的 MSI-X vector index；当前范围 `0..3` |
| `--msix-on-dpu` | DPU | 可选；改用 DPU Arm 侧 raise MSI-X，不加时默认使用 DPA datapath |

变量：

```bash
MSIX_INDEX=0
```

Host 端先启动监听：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_msix/host/build/doca_devemu_pci_device_msix_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -l 60 \
    --sdk-log-level 40"
```

等待 Host 日志出现：

```text
Listening on all MSI-X vectors
```

DPU 端默认 DPA datapath raise MSI-X：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_msix/dpu/build/doca_devemu_pci_device_msix_dpu \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -x "$MSIX_INDEX" \
  -l 60 \
  --sdk-log-level 40
```

可选 DPU Arm 侧 raise 路径：

```bash
./samples/doca_devemu/devemu_pci_device_msix/dpu/build/doca_devemu_pci_device_msix_dpu \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -x "$MSIX_INDEX" \
  --msix-on-dpu \
  -l 60 \
  --sdk-log-level 40
```

通过条件：

- Host 端进入 `Listening on all MSI-X vectors`。
- DPU 端打印 `MSI-X raised successfully`。
- Host 端打印 `Event received for MSI-X vector index 0 new value 1`。

本地实测默认 DPA datapath 成功；第二次重复 VFIO probe 时 Host endpoint 没有重新绑定成功，因此未继续验证
`--msix-on-dpu` 可选路径。

### 6.8 devemu_pci_device_dma

`devemu_pci_device_dma` 验证 Host driver 通过 VFIO/IOMMU 暴露 DMA buffer，DPU/BlueField 侧通过 DOCA DMA 对该
Host memory 做双向复制。

特有参数：

| 参数 | 端侧 | 说明 |
| --- | --- | --- |
| `-u, --vuid` | DPU | emulated PCI device VUID |
| `-a, --addr` | DPU | Host DMA memory IOVA；Host sample 固定使用 `0x1000000` |
| `-d, --device-name` | DPU | 可选 DMA IB device 名称；不指定时使用 emulation manager 对应 DOCA device |
| `-w, --write-data` | DPU/Host | Host 端为预写入 buffer 的字符串，DPU 端为写回 Host 的字符串 |

变量：

```bash
HOST_DMA_IOVA=0x1000000
```

Host 端先启动并等待 DPU 写回：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_dma/host/build/doca_devemu_pci_device_dma_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -w host_dma_smoke \
    -l 60 \
    --sdk-log-level 40"
```

期望先输出：

```text
Allocated DMA memory(IOVA): 0x1000000
Write to DMA memory: host_dma_smoke
Wait for new DMA data from DPU--- ---
```

DPU 端后启动：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_dma/dpu/build/doca_devemu_pci_device_dma_dpu \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -a "$HOST_DMA_IOVA" \
  -w dpu_dma_smoke \
  -l 60 \
  --sdk-log-level 40
```

通过条件：

- DPU 端打印 `Success, DMA memory copied from host: host_dma_smoke`。
- DPU 端打印 `Success, DMA memory copied to host: dpu_dma_smoke`。
- Host 端打印 `Read new data from DPU: dpu_dma_smoke`。
- 两端均以 `Sample finished successfully` 结束。

运行时可能出现：

```text
Memory range isn't aligned to 64B
```

这是示例程序本地 buffer 对齐导致的性能提示；基础功能 smoke test 中可忽略。

### 6.9 devemu_pci_device_stateful_region

`devemu_pci_device_stateful_region` 验证 Host driver 通过 VFIO mmap emulated endpoint BAR 中的 stateful region，
DPU/BlueField 侧注册 stateful-region write event 并在 Host 写入时收到事件。

当前 stateful region 配置：

```text
bar_id=0
start_address=0x3000
size=0x100
```

特有参数：

| 参数 | 端侧 | 说明 |
| --- | --- | --- |
| `-u, --vuid` | DPU | emulated PCI device VUID |
| `-r, --region-index` | Host | stateful region index；当前只有 `0` |
| `-w, --write-data` | Host | 写入 stateful region 的 ASCII 字符串；为空字符串时执行 read/dump |

DPU 端先启动：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_stateful_region/dpu/build/doca_devemu_pci_device_stateful_region_dpu \
  -p "$DPU_DEV" \
  -u "$EMU_VUID" \
  -l 60 \
  --sdk-log-level 40
```

等待 DPU 日志出现：

```text
Press ([ctrl] + c) to stop sample
```

Host 端写 stateful region：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_stateful_region/host/build/doca_devemu_pci_device_stateful_region_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -r 0 \
    -w stateful_smoke_20260714 \
    -l 60 \
    --sdk-log-level 40"
```

通过条件：

- Host 端打印 `Writing to stateful region ...` 并成功退出。
- DPU 端打印 `Host wrote to stateful region of emulated device`。
- DPU 端按 `Ctrl+c` 后打印 `Sample finished successfully`。

当前不把以下现象视为失败：

- DPU 端 `Printing values of stateful region ...` 后面的 dump 为全 0。
- Host 端 read 模式读回全 0。

### 6.10 devemu_pci_device_tlp_handler

`devemu_pci_device_tlp_handler` 验证 DPU/BlueField 侧通过 DOCA DevEmu PCI TLP channel 处理 raw PCIe TLP，Host
driver 通过 VFIO mmap BAR transaction region 做写入或读取。

关键限制：

- sample 实现单个 PCIe endpoint，只支持一个 TLP channel downstream port；运行前需要确认固件配置中 TLP ports 数量为 1。
- DPU 端不需要 `-u <vuid>`；它会创建或复用 TLP representor。

当前 transaction region 配置：

```text
bar_id=0
start_address=0x3000
size=4096
```

特有参数：

| 参数 | 端侧 | 说明 |
| --- | --- | --- |
| `-s, --shm-dir-path` | DPU | 可选；TLP channel live-upgrade handover 使用的共享内存目录 |
| `-d, --handover-destination` | DPU | 可选；作为 live-upgrade handover destination 启动 |
| `-r, --region-index` | Host | transaction region index；当前只有 `0` |
| `-w, --write-data` | Host | 写入 transaction region 的字符串；`-w ""` 时执行 read/dump |

DPU 端先启动：

```bash
cd /root/ByteDance/doca-samples
./samples/doca_devemu/devemu_pci_device_tlp_handler/dpu/build/doca_devemu_pci_device_tlp_handler_dpu \
  -p "$DPU_DEV" \
  -l 60 \
  --sdk-log-level 40
```

等待 DPU 日志出现：

```text
Transaction region initialized: size=4096 bytes
Expansion ROM bar region initialized: size=65536 bytes
Polling on the TLP channel to get TLP requests. Press Ctrl+C to exit.
```

DPU 端运行期间可复查临时 TLP representor：

```bash
/opt/mellanox/doca/tools/doca_caps --list-rep-devs
```

本地实测会额外出现：

```text
representor-PCI: 0000:00:00.0
    vuid MT2529603G38TLPPF0
    rep_type EMULATED
```

Host 端写 transaction region：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_tlp_handler/host/build/doca_devemu_pci_device_tlp_handler_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -r 0 \
    -w tlp_handler_smoke_20260714 \
    -l 60 \
    --sdk-log-level 40"
```

可选 read/dump：

```bash
$HOST_SSH "cd /root/ByteDance/doca-samples && \
  ./samples/doca_devemu/devemu_pci_device_tlp_handler/host/build/doca_devemu_pci_device_tlp_handler_host \
    -p $HOST_EP \
    -g $HOST_VFIO_GROUP \
    -r 0 \
    -w \"\" \
    -l 60 \
    --sdk-log-level 40"
```

通过条件：

- DPU 端进入 `Polling on the TLP channel ...`。
- Host 端打印 `Writing to transaction region ...` 并成功退出。
- DPU 端按 `Ctrl+c` 后清理 transaction region、Expansion ROM bar 并打印 `Sample finished successfully`。

当前不把以下现象视为失败：

- DPU 端 memory write handler 默认主要输出 DEBUG 级日志，INFO 级别下 Host 写入时 DPU 端可能没有额外日志。
- Host 端 read/dump transaction region 返回全 0。本地实测写入后 read 模式仍 dump 全 0，因此只用 Host 写/读命令成功返回和
  DPU 正常 poll/清理作为 smoke test 通过条件。

### 6.11 通用常见错误

| 日志/现象 | 处理方式 |
| --- | --- |
| DPU 端提示 `The VUID parameter is missing` | DB/DMA/stateful/MSI-X 必须通过 `-u <vuid>` 指定 emulated PCI device VUID |
| DPU 端提示 `Matching emulated device not found` | 用 `doca_caps --list-rep-devs` 确认存在 `rep_type EMULATED` representor，且 VUID 输入正确 |
| DPU 端提示 hotplug state 不是 `POWER_ON` | 确认 generic PCI emulated device 已被 Host 枚举且处于 power on 状态 |
| Host 侧找不到 `15b3:1021` endpoint | 按 6.3 先运行 `devemu_pci_device_list` 枚举；若列表为空，再运行 `devemu_pci_device_hotplug` 创建 endpoint，必要时重启 Host 重新枚举 |
| Host 端提示 `VFIO group not viable` | 确认 IOMMU group 中所有设备都已绑定到 VFIO；本地调测中 group 44 只包含 `0000:40:00.0` |
| Host 端 `VFIO_SET_IOMMU` 返回 `errno=1` | 检查 Host `dmesg`；若提示无 interrupt remapping，可临时打开 `vfio_iommu_type1.allow_unsafe_interrupts` 做 smoke test，完成后恢复 |
| DPU 日志出现 FLR 并重建 PCI device | Host VFIO 初始化/释放会触发 FLR，属于预期现象 |
| 重复测试时 `drivers_probe` 或重新绑定卡住 | 清理残留 sample/probe 进程，确认 `driver_override` 已清空并恢复 `allow_unsafe_interrupts=N`；必要时重新枚举或重启 Host 后再测 |

### 6.12 sample 特有错误和限制

| sample | 日志/现象 | 处理方式 |
| --- | --- | --- |
| DB | Host 写 `-d 3` 但 DPU 监听 `-i 0` 未收到目标值 | 对 offset DB region，基础测试让 Host `--db-index` 与 DPU `--db-id` 保持一致，例如都用 `0` |
| DMA | Host 端一直等待 DPU 写回 | 确认 DPU 端使用 `-a 0x1000000`，且 DPU 端 `-w` 写回字符串非空 |
| DMA | DPU 端 `Failed to DMA read data from host` 或 `Failed to DMA write data to host` | 确认 Host sample 已先启动并成功映射 IOVA `0x1000000`，Host endpoint 已绑定 `vfio-pci` |
| stateful region | Host 端提示 region index 无效 | 当前 `PCI_TYPE_NUM_BAR_STATEFUL_REGIONS` 为 1，只能使用 `-r 0` |
| stateful region | DPU 收到事件但 dump 全 0 | 当前本地实测现象；只用事件触发作为 smoke test 通过条件 |
| MSI-X | DPU 端提示 MSI-X index 无效 | 当前 `PCI_TYPE_NUM_MSIX` 为 4，只能使用 `0..3` |
| MSI-X | Host 端没有收到 event | 确认 Host 监听端先启动，DPU 端 `-x` 指向合法 vector index，且 endpoint 已绑定 `vfio-pci` |
| TLP handler | DPU 端提示 `Sample does not support TLP channel with more or less than one downstream port` | 用 `mlxconfig` 将 TLP ports 数量配置为 1 后再运行 |
| TLP handler | Host 端 read 模式 dump 全 0 | 当前本地实测现象；只用 Host 写/读命令成功返回和 DPU 正常 poll/清理作为 smoke test 通过条件 |

## 7. 后续章节模板

```text
## N. applications/<name> 或 samples/<group>/<name>
### N.1 概述
### N.2 参数
### N.3 运行步骤
### N.4 校验
### N.5 常见错误
```

## 附录 A. 权限规则

在当前测试环境中，建议 DPU 和 Host 两端都先获取 root shell，并在沙箱外完整权限环境中执行本文档命令。
如果 Host 通过 SSH 操作，优先使用 root 登录或让 `$HOST_SSH` 进入 root shell 后执行命令。
不要依赖受限沙箱内的默认权限去访问或操作，否则容易出现权限拒绝、命令卡住或状态不一致。

以下操作统一按需要 root/非沙箱运行处理：

- 所有访问 IB/RDMA 设备的操作，包括访问 `/dev/infiniband`、verbs/uverbs 设备、RDMA netlink/devlink 信息等。
- 所有 Mellanox/MLNX/DOCA 相关命令，包括 `/opt/mellanox/doca/...`、`mst`、`mlx*`、`mlnx*`、`ib*`、`rdma`、
  `devlink`，以及本仓库编译出的 DOCA application/sample 二进制。
- 所有 iproute2 和 pciutils 相关命令，包括 `ip`、`bridge`、`tc`、`lspci`、`setpci` 等。
- 所有 sysfs/procfs 设备控制操作，包括 PCI `remove/rescan`、VFIO/IOMMU 参数、hugepage 等。
- 所有 SPDK/DPDK 运行与 RPC 操作，包括访问 `/var/tmp/*.sock`、创建或删除 bdev/subsystem/listener/function。
- 所有 Host 远端硬件操作，包括 `$HOST_SSH` 内执行的 `lspci`、`nvme`、`fio`、driver bind/unbind 等。
