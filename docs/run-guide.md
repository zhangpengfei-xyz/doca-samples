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
BMC_HOST=192.168.1.10
BMC_USER=toutiao
BMC_PASS='toutiao!@#'
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

## 4. 后续章节模板

```text
## N. applications/<name> 或 samples/<group>/<name>
### N.1 概述
### N.2 参数
### N.3 运行步骤
### N.4 校验
### N.5 常见错误
```

## 附录 A. 权限规则

在当前测试环境中，以下操作统一按需要提权或非沙箱运行处理：

- 所有访问 IB/RDMA 设备的操作，包括访问 `/dev/infiniband`、verbs/uverbs 设备、RDMA netlink/devlink 信息等。
- 所有 Mellanox/MLNX/DOCA 相关命令，包括 `/opt/mellanox/doca/...`、`mst`、`mlx*`、`mlnx*`、`ib*`、`rdma`、
  `devlink`，以及本仓库编译出的 DOCA application/sample 二进制。
- 所有 iproute2 和 pciutils 相关命令，包括 `ip`、`bridge`、`tc`、`lspci`、`setpci` 等。
