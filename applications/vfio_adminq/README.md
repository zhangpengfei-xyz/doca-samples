# VFIO AdminQ 最小验证程序

本目录按 [DESIGN.md](DESIGN.md) 实现三个独立程序，并严格复用已验证的
SRDMA/io-engine ABI：

- `doca_vfio_adminq_pci_fe`：DPU PCI 前端，负责设备插拔、PCI config/BAR0、TLP 和
  Gemini 服务端。
- `doca_vfio_adminq_dev_be`：DPU 后端，负责 Gemini 客户端、DOCA DB/DPA 和 DMA。
- `vfio_adminq_host_emu`：Host 侧 VFIO 测试程序，负责 AdminQ DMA/DB 闭环。

v1 只支持一个 endpoint、一个 AdminQ 和一个 outstanding 请求，不实现 MSI-X。

## DPU 构建

在仓库根目录执行：

```bash
meson setup --reconfigure applications/build applications \
  -Denable_vfio_adminq=true -Ddpacc_mcpu=nv-dpa-bf3
meson compile -C applications/build \
  doca_vfio_adminq_pci_fe doca_vfio_adminq_dev_be vfio_adminq_host_emu
```

DPU 产物位于 `applications/build/vfio_adminq/`。其中 Meson 生成的
`vfio_adminq_host_emu` 与 DPU 同为 AArch64，仅用于编译检查；Host 若不是 AArch64，
必须在 Host 上原生构建。

## Host 构建

把 `host-emu/` 与 `common/` 保持相邻目录复制到 Host，然后执行：

```bash
make -C host-emu
```

该程序只依赖 Linux VFIO UAPI 和 libc。

## 运行顺序

以下命令均需 root 权限。先确认没有另一套 io-engine 或 demo 占用相同 DOCA PCI type、
representor 或 Gemini socket。

1. DPU 启动 PCI 前端：

   ```bash
   applications/build/vfio_adminq/doca_vfio_adminq_pci_fe serve \
     --pci-addr 0000:03:00.0
   ```

2. DPU 启动后端：

   ```bash
   applications/build/vfio_adminq/doca_vfio_adminq_dev_be serve \
     --pci-addr 0000:03:00.0 \
     --socket /var/tmp/bes2/bes2-server.sock
   ```

3. DPU 插入并检查设备：

   ```bash
   applications/build/vfio_adminq/doca_vfio_adminq_pci_fe plug
   applications/build/vfio_adminq/doca_vfio_adminq_pci_fe status
   ```

4. Host 找到 `1e93:006a` 的 BDF，绑定 `vfio-pci` 后运行闭环测试：

   ```bash
   host-emu/bind-vfio.sh 0000:01:00.0
   host-emu/vfio_adminq_host_emu --bdf 0000:01:00.0
   ```

   `host-emu` 会建立 VFIO DMA 映射、写 AdminQ IOVA/depth、置 `INIT_DONE`、写
   BAR0 doorbell，然后轮询并校验后端 DMA 回写。

5. 停止测试时，必须先在 Host 删除 PCI function，再在 DPU 销毁 endpoint：

   ```bash
   host-emu/unbind-vfio.sh 0000:01:00.0
   applications/build/vfio_adminq/doca_vfio_adminq_pci_fe unplug
   ```

   不允许在 Host function 仍绑定或仍存在于 sysfs 时直接销毁 DPU endpoint。部分 Root
   Port 会把随后到达的配置访问判为 ACS violation，并执行 DPC containment，导致整个
   BlueField PCI function（包括 Host 网络接口）离线。

## 常用参数

三个程序均支持 `--help`。默认 Gemini socket 是
`/var/tmp/bes2/bes2-server.sock`，默认控制 socket 是
`/run/vfio-adminq/pci-fe.sock`。`pci-fe` 的默认 DOCA PCI 地址为
`0000:03:00.0`；实际部署应使用已验证环境对应的地址。
