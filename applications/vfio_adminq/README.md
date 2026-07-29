# VFIO AdminQ 最小验证程序

本目录包含两代设计：

- [BASIC_DESIGN.md](BASIC_DESIGN.md)：已经实现的 v1 最小 VFIO AdminQ/DMA 闭环。
- [SRDMA_DRIVER_DESIGN.md](SRDMA_DRIVER_DESIGN.md)：对接真实 `srdma.ko` 的 v2
  目标设计，包括单 BAR0、MSI-X、真实 AdminQ、EQ/CQ/QP 和数据面边界。
- [host-ctrl-test/](host-ctrl-test/)：Host 侧 libibverbs 控制面测试，覆盖 UCTX、PD、
  MR、EQ、CQ、QP、GID、统计、健康和 AdminQ 计数。

当前代码已实现 v2 控制面，并保留 v1 host emulator 作为低层故障注入工具：

- `doca_vfio_adminq_pci_fe`：DPU PCI 前端，负责设备插拔、单 BAR0、CFG TLP、
  doorbell region 和 Gemini 服务端。
- `doca_vfio_adminq_dev_be`：DPU 后端，负责 Gemini 客户端、AdminQ DMA、资源表、
  AdminQ DPA doorbell completion 和 MSI-X。
- `vfio_adminq_host_emu`：Host 侧 VFIO 测试程序，负责 AdminQ DMA/DB 闭环。

v2 只支持一个 endpoint，提供 128-depth AdminQ、128 个 MSI-X vector 和
UCTX/PD/MR/EQ/CQ/QP 控制面；不实现 RoCE 数据面。

## DPU 构建

在仓库根目录执行：

```bash
meson setup --reconfigure applications/build applications \
  -Denable_vfio_adminq=true -Ddpacc_mcpu=nv-dpa-bf3
meson compile -C applications/build \
  doca_vfio_adminq_pci_fe doca_vfio_adminq_dev_be \
  vfio_adminq_host_emu vfio_adminq_srdma_control_test
meson test -C applications/build vfio_adminq_srdma_control --print-errorlogs
```

DPU 产物位于 `applications/build/vfio_adminq/`。其中 Meson 生成的
`vfio_adminq_host_emu` 与 DPU 同为 AArch64，仅用于编译检查；Host 若不是 AArch64，
必须在 Host 上原生构建。

## Host 构建

### srdma.ko

必须针对 Host 当前实际加载的 OFED 构建，而不是仅依赖 CMake 自动找到的第一个
`/usr/src/ofa_kernel`。先用 `modinfo -n ib_core` 确认当前 RDMA 模块来源，再显式指定
包含匹配 `Module.symvers` 的目录。例如：

```bash
cd /root/ByteDance/srdma/host/kernel
SRDMA_OFA_DIR=/usr/src/ofa_kernel-dkms/$(uname -m)/$(uname -r)
test -f "$SRDMA_OFA_DIR/Module.symvers"

cmake -S . -B build -DOFA_DIR="$SRDMA_OFA_DIR"
cmake --build build --target modules -j"$(nproc)"
modinfo build/src/srdma.ko | grep -E '^(version|vermagic|depends):'
```

如果 `insmod` 报 `Invalid parameters`，且 `dmesg` 出现 `disagrees about version of
symbol`，说明构建使用的 OFED `Module.symvers` 与已加载的 `ib_core` 不匹配。不要用
`--force` 绕过，应使用正确的 `OFA_DIR` 重新构建。

### Host 控制面测试

Host 需要安装与驱动 ABI 匹配的 sRDMA libibverbs provider、`libibverbs-dev`、
`rdma-core`、`iproute2` 和 `devlink`。DOCA-OFED 26.04 的 libibverbs private ABI
为 59，provider 构建结果必须是 `libsrdma-rdmav59.so`：

```bash
cd /root/ByteDance/srdma/host/provider
./autogen.sh
./configure --prefix=/usr \
  --libdir="/usr/lib/$(gcc -print-multiarch)" --sysconfdir=/etc
make -j"$(nproc)"
make install
ldconfig

grep '^IBV_DEVICE_LIBRARY_EXTENSION' Makefile
readlink -f "/usr/lib/$(gcc -print-multiarch)/libibverbs/libsrdma-rdmav59.so"
```

然后在 Host 原生构建控制面测试：

```bash
make -C /root/ByteDance/doca-samples/applications/vfio_adminq/host-ctrl-test
```

## srdma.ko 端到端运行

以下命令均需 root 权限。示例使用 DPU DOCA PCI 地址 `0000:03:00.0` 和 Host
endpoint BDF `0000:46:00.0`；不同环境必须替换。启动前确认没有其他 io-engine 或
demo 占用相同 DOCA PCI type、representor、Gemini socket 或控制 socket。

### 1. 确认 Host 关联 netdev MAC

设备 MAC 必须按 Host 关联 netdev 的 permanent MAC 配置。当前验证环境关联 `eth0`：

```bash
ethtool -P eth0
ip -brief address show eth0
```

记录输出的 permanent MAC，后续作为 `--netdev-mac` 参数。

### 2. 启动 DPU 服务

在 DPU 的两个终端依次启动 PCI 前端和后端，进程需要持续运行。

终端一：

```bash
cd /root/ByteDance/doca-samples
applications/build/vfio_adminq/doca_vfio_adminq_pci_fe serve \
  --pci-addr 0000:03:00.0 \
  --netdev-mac cc:40:f3:3f:82:2e
```

终端二：

```bash
cd /root/ByteDance/doca-samples
applications/build/vfio_adminq/doca_vfio_adminq_dev_be serve \
  --pci-addr 0000:03:00.0 \
  --socket /var/tmp/bes2/bes2-server.sock
```

第三个 DPU 终端执行 plug：

```bash
cd /root/ByteDance/doca-samples
applications/build/vfio_adminq/doca_vfio_adminq_pci_fe plug
applications/build/vfio_adminq/doca_vfio_adminq_pci_fe status
```

plug 后、Host 驱动加载前，状态通常为 `PRESENT_STOPPED`；Host probe 完成 START 后应为
`STARTED`。

### 3. Host 重新枚举 endpoint

DPU 服务重启后，Host 可能仍保留旧 PCI 实例。必须删除旧实例并 rescan，否则 BAR
访问可能返回全 `0xff`，驱动日志表现为 `netdev ff:ff:ff:ff:ff:ff not found`。

```bash
SRDMA_BDFS=($(lspci -Dnn -d 1e93:006a | awk '{print $1}'))
SRDMA_BDF=0000:46:00.0

if lsmod | grep -q '^srdma '; then
  rmmod srdma
fi
if test -e "/sys/bus/pci/devices/$SRDMA_BDF/remove"; then
  echo 1 > "/sys/bus/pci/devices/$SRDMA_BDF/remove"
fi
echo 1 > /sys/bus/pci/rescan

lspci -Dnn -s "$SRDMA_BDF"
```

### 4. 加载 srdma.ko

```bash
cd /root/ByteDance/srdma/host/kernel
insmod build/src/srdma.ko

rdma link show srdma_0/1
ibv_devinfo -d srdma_0
```

期望 `srdma_0/1 state ACTIVE`、`netdev eth0`。PCI 配置应报告一个 64 KiB BAR0 和
128-entry MSI-X capability；DOCA type 的 BAR0 aperture 为 1 MiB。MSI-X table/PBA
位于 transaction region，由 `pci-fe` 软件处理；control path 的 vector 0 address/data
同步给 `dev-be`，由后者直接 DMA 写 MSI-X address。

```bash
lspci -Dvv -s "$SRDMA_BDF" | \
  grep -E 'Region 0|MSI-X|Vector table|PBA'
cat /proc/interrupts | grep 'srdma-.*\[ctrl\]'
```

### 5. 检查健康状态

```bash
devlink health show "pci/$SRDMA_BDF" reporter fw
devlink health diagnose "pci/$SRDMA_BDF" reporter fw
```

当前后端通过 `cap_ext.bit2 (HEALTH_MAC_ONLY)` 声明只实现 heartbeat/MAC 健康状态。
正常结果应满足：

- `overall_status: OK`；
- `mac0_status/mac1_status: on`；
- `offload_alive/software_switch_alive: false`；
- `health_h` 高 32 bit 的低四位为 `0x3`，而不是虚报全部组件存活的 `0xf`；
- 多次读取时 heartbeat 单调增长，速率约为每 500 ms 一次。

### 6. 运行完整控制面测试

测试地址必须是关联 netdev 上尚未使用的地址；程序会临时添加并自动删除，以验证
`ADD_GID/DEL_GID`。以下文档地址仅为示例：

```bash
cd /root/ByteDance/doca-samples/applications/vfio_adminq/host-ctrl-test
./srdma_ctrl_test \
  --device srdma_0 --port 1 --vector 1 \
  --gid-cycle 2001:db8:46::1/128
```

测试覆盖 UCTX、PD、MR、EQ/CQ、RC QP 全状态机、统计、GID 和主动
`HEALTH_CHECK`。完整通过的期望摘要为：

```text
Summary: PASS=57 WARN=0 FAIL=0
```

不指定 `--gid-cycle` 时 ADD/DEL GID 会记为 WARN。测试结束后应确认临时地址已经删除：

```bash
if ip -6 address show dev eth0 | grep -q '2001:db8:46::1'; then
  echo 'temporary GID address was not removed'
fi
```

## 停止与重新运行

必须先让 Host 停止使用并删除 PCI function，再在 DPU 销毁 endpoint：

Host：

```bash
SRDMA_BDF=0000:46:00.0
rmmod srdma
echo 1 > "/sys/bus/pci/devices/$SRDMA_BDF/remove"
```

DPU：

```bash
cd /root/ByteDance/doca-samples
applications/build/vfio_adminq/doca_vfio_adminq_pci_fe unplug
```

随后依次用 `Ctrl-C` 停止 `doca_vfio_adminq_dev_be` 和
`doca_vfio_adminq_pci_fe`。不允许在 Host function 仍绑定或仍存在于 sysfs 时直接
销毁 DPU endpoint；部分 Root Port 会把随后的配置访问判为 ACS violation 并触发 DPC
containment，导致整个 BlueField PCI function（包括 Host 网络接口）离线。

正常复测不需要重启 BF3 或 Host。重复运行时重新执行“启动 DPU 服务 → Host 删除旧
实例并 rescan → 加载模块”的顺序。

## VFIO host emulator（可选）

`host-emu` 是不经过 `srdma.ko` 的低层 AdminQ DMA/doorbell 故障注入工具，不是上述
端到端验收的替代品。把 `host-emu/` 与 `common/` 保持相邻目录复制到 Host 后执行：

```bash
make -C host-emu
host-emu/bind-vfio.sh 0000:46:00.0
host-emu/vfio_adminq_host_emu --bdf 0000:46:00.0
host-emu/unbind-vfio.sh 0000:46:00.0
```

## 常用参数

三个程序均支持 `--help`。默认 Gemini socket 是
`/var/tmp/bes2/bes2-server.sock`，默认控制 socket 是
`/run/vfio-adminq/pci-fe.sock`。`pci-fe` 的默认 DOCA PCI 地址为
`0000:03:00.0`；实际部署应使用已验证环境对应的地址。
