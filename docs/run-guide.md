# DOCA Samples Run Guide

本文档记录本仓库中 DOCA applications 和 samples 的运行方式。新增条目时，按
`applications/<name>` 或 `samples/<group>/<name>` 增加独立章节；系统权限相关规则统一放在附录。

## 1. 通用环境

### 1.1 角色与路径

| 角色 | 说明 | 仓库路径 |
| --- | --- | --- |
| DPU | BlueField-3 DPU | `/root/ByteDance/doca-samples` |
| Host | BF3 对端 x86 host | `/root/ByteDance/doca-samples` |

Host 登录方式：

```bash
ssh -o ProxyJump=10.249.181.55 192.168.163.201
```

### 1.2 已知设备地址

| 位置 | 用途 | PCI 地址 |
| --- | --- | --- |
| DPU | DOCA device | `0000:03:00.0` |
| DPU | Host PF representor | `0000:5c:00.0` |
| Host | DOCA device | `0000:5c:00.0` |

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

## 3. 后续章节模板

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
