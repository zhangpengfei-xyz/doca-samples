# DOCA DevEmu PCI Device Samples 主干逻辑

本文档分析 `samples/doca_devemu/devemu_pci_device_{db,msix,dma,tlp_handler,stateful_region}` 的主干逻辑。目标不是复述 README，而是把开发时真正需要抓住的对象生命周期、调用链和 Host/DPU 交互关系整理出来。

## 1. 范围与入口

这五个 sample 都围绕同一个主题：DPU 侧通过 DOCA DevEmu 暴露或接管一个可被 Host 枚举/访问的 PCI endpoint，Host 侧通过 VFIO 访问该 emulated PCI device。

| Sample | DPU 入口 | Host 入口 | 主要能力 |
| --- | --- | --- | --- |
| `devemu_pci_device_db` | `dpu/devemu_pci_device_db_dpu_main.c` | `host/devemu_pci_device_db_host_main.c` | Host 写 Doorbell，DPU/DPA 收到 DB completion |
| `devemu_pci_device_msix` | `dpu/devemu_pci_device_msix_dpu_main.c` | `host/devemu_pci_device_msix_host_main.c` | DPU 或 DPA raise MSI-X，Host 用 eventfd 接收 |
| `devemu_pci_device_dma` | `dpu/devemu_pci_device_dma_dpu_main.c` | `host/devemu_pci_device_dma_host_main.c` | Host 把内存 DMA map 到 IOVA，DPU 用 DOCA DMA 读写 |
| `devemu_pci_device_tlp_handler` | `dpu/devemu_pci_device_tlp_handler_dpu_main.c` | `host/devemu_pci_device_tlp_handler_host_main.c` | DPU 直接处理 raw PCIe TLP；Host 读写 transaction region |
| `devemu_pci_device_stateful_region` | `dpu/devemu_pci_device_stateful_region_dpu_main.c` | `host/devemu_pci_device_stateful_region_host_main.c` | Host 写 stateful BAR region，DPU 收事件并 query 内容 |

这些 sample 依赖一次前置 hotplug：Host 必须已经能看到 emulated PCI device。`db`、`msix`、`dma`、`stateful_region` 的 DPU sample 都通过 VUID 找已有 representor；它们本身不创建新 endpoint。`tlp_handler` 是例外：它会创建或复用 TLP representor，并承担 raw TLP channel 的处理。

## 2. 公共配置

普通 PCI DevEmu sample 共享 `devemu_pci_type_config.h`：

| 配置 | 值 | 用途 |
| --- | --- | --- |
| `PCI_TYPE_NAME` | `Sample PCI Type` | `doca_devemu_pci_type_create()` 的类型名 |
| `PCI_TYPE_VENDOR_ID` / `DEVICE_ID` | `0x15b3` / `0x1021` | Host 看到的 PCI config space ID |
| BAR layout | BAR0: `log_size=0xe`，64-bit prefetchable | BAR0 大小 16 KiB，承载 DB/MSI-X/stateful/transaction region |
| DB region | BAR0 `0x0000-0x0fff` | `PCI_TYPE_NUM_DB=64`，stride 为 `1 << log_db_stride_size = 4` 字节 |
| MSI-X table | BAR0 `0x1000-0x1fff` | `PCI_TYPE_NUM_MSIX=4` 的 MSI-X table |
| MSI-X PBA | BAR0 `0x2000-0x2fff` | MSI-X pending bit array |
| Stateful region | BAR0 `0x3000`，size `0x100` | `stateful_region` sample 使用 |
| Transaction region | BAR0 `0x3000`，size `0x1000` | `tlp_handler` Host sample 使用 |

注意 `stateful_configs[0]` 和 `transaction_configs[0]` 起始地址相同，但它们服务不同 sample。普通 DevEmu path 由 SDK 管理 stateful region；TLP path 则由 sample 自己维护 transaction memory 并生成 TLP completion。

## 3. 公共 DPU 骨架

除 `tlp_handler` 外，其余 DPU sample 大体遵循同一条主线：

```text
main()
  -> 初始化日志和 doca_argp
  -> 解析 -p/--pci-addr 和 -u/--vuid，解析 sample 专属参数
  -> 调用 devemu_pci_device_<feature>_dpu()

devemu_pci_device_<feature>_dpu()
  -> doca_pe_create()
  -> doca_devemu_pci_type_create(PCI_TYPE_NAME)
  -> find_supported_device()
      -> 遍历 doca_devinfo
      -> 匹配 PCI address
      -> 检查 type hotplug capability
      -> doca_dev_open()
  -> configure_and_start_pci_type()
      -> doca_devemu_pci_type_set_dev()
      -> 设置 vendor/device/subsystem/revision/class
      -> 检查 max_num_msix/max_num_db
      -> 设置 MSI-X、DB、BAR layout、stateful region
      -> doca_devemu_pci_type_start()
  -> find_emulated_device()
      -> doca_devemu_pci_type_create_rep_list()
      -> 按 VUID 找 representor
      -> doca_dev_rep_open()
  -> doca_devemu_pci_dev_create()
  -> 注册 sample 专属事件或 datapath
  -> doca_ctx_start(doca_devemu_pci_dev_as_ctx())
  -> 校验 hotplug state == POWER_ON
  -> PE progress 或执行一次性动作
  -> devemu_resources_cleanup()
```

`devemu_resources_cleanup()` 负责按反向顺序销毁 data path object、DPA thread、ctx、pci_dev、representor、DPA、PCI type、PE 和 DOCA device。`destroy_rep=false` 时只 close representor，不销毁 Host 侧仍存在的 emulated device。

## 4. 公共 Host VFIO 骨架

Host sample 共享 `devemu_pci_host_common.*`：

```text
main()
  -> 初始化日志和 doca_argp
  -> 解析 -p/--pci-addr: emulated device 的完整 BDF
  -> 解析 -g/--vfio-group: 对应 IOMMU group
  -> 解析 sample 专属参数
  -> 调用 devemu_pci_device_<feature>_host()

init_vfio_device()
  -> open("/dev/vfio/vfio")
  -> open("/dev/vfio/<group>")
  -> VFIO_GET_API_VERSION / VFIO_CHECK_EXTENSION
  -> VFIO_GROUP_GET_STATUS，要求 group viable
  -> VFIO_GROUP_SET_CONTAINER
  -> VFIO_SET_IOMMU(VFIO_TYPE1v2_IOMMU)
  -> VFIO_GROUP_GET_DEVICE_FD
  -> enable_pci_cmd()
      -> 读 VFIO config region 信息
      -> 向 PCI command register 写 0x6，启用 memory space 和 bus master

map_bar_region_memory()
  -> VFIO_DEVICE_GET_REGION_INFO
  -> 检查 region 不越界
  -> mmap BAR 子区间
```

Host 侧样例通常只做一次动作：mmap 对应 BAR region 后读/写，或者配置 eventfd/IOVA 后等待 DPU 动作。

### 4.1 线程模型口径与总表

这里的线程数按 sample 代码自己创建或运行的执行流统计：进程启动自带的主线程算 1 个，显式 `doca_dpa_thread_*` 创建的 DPA thread 算 1 个；`doca_pe` 只是 progress engine，不单独算线程；callback、signal handler、eventfd、Unix socket 和 DOCA 内部实现线程也不单独计数。

| Sample | DPU 程序线程数 | DPU 线程职责 | Host 程序线程数 | Host 线程职责 |
| --- | ---: | --- | ---: | --- |
| `db` | 2 | 主线程创建 DevEmu/DB 对象并在 `doca_pe_progress()` 中处理 ctx/FLR 事件；DPA thread 运行 `db_handler()`，等待 DB completion、ack、读 DB value 并 reschedule | 1 | 初始化 VFIO、mmap DB BAR region，向 doorbell offset 写入 `db_value` |
| `msix` | 1 | 主线程创建 DevEmu/MSI-X 对象并 raise MSI-X；DPA path 只是执行一次 `doca_dpa_rpc(raise_msix_rpc)`，没有常驻 DPA thread | 1 | 初始化 VFIO，为所有 MSI-X vector 绑定 eventfd，并在主循环中轮询读取 eventfd |
| `dma` | 1 | 主线程初始化 DevEmu/DMA/mmap/buf inventory，提交 DMA memcpy task，并在同一线程里 progress PE 等 completion/error callback | 1 | 初始化 VFIO，分配并映射 DMA buffer，写入初始数据后轮询等待 DPU 写回 |
| `tlp_handler` | 1/进程 | 主线程创建或接管 TLP channel，持续 progress TLP PE；启用 handover 时同一线程用非阻塞 socket/poll 处理 SETUP/EXPORT/BEGIN/END | 1 | 初始化 VFIO，mmap transaction region，读 dump 或写入测试数据 |
| `stateful_region` | 1 | 主线程注册 stateful write/FLR 事件并 progress PE；write/FLR callback 在该 progress 路径上执行 | 1 | 初始化 VFIO，mmap stateful region，读 dump 或写入测试数据 |

`tlp_handler` 的 live-upgrade source 和 destination 是两个独立进程时，每个进程各 1 个主线程；handover 协议本身不会在单个进程内额外创建 pthread。

## 5. DB Sample

### 5.1 目标

`db` 演示 Host 写 BAR0 doorbell region 后，DPU/DPA 如何收到 DB completion 并读取 DB value。它也演示 FLR 后 DB object 的销毁和重建。

### 5.2 DPU 主干

关键文件：

- `devemu_pci_device_db/dpu/devemu_pci_device_db_dpu_main.c`
- `devemu_pci_device_db/dpu/host/devemu_pci_device_db_dpu_sample.c`
- `devemu_pci_device_db/dpu/device/devemu_pci_device_db_dpu_kernels_dev.c`

调用链：

```text
devemu_pci_device_db_dpu(pci_address, vuid, db_region_idx, db_id)
  -> 创建 PE、PCI type、打开支持 hotplug 的 DPU device
  -> 检查 db_id < max_num_db
  -> configure_and_start_pci_type()
  -> init_dpa(devemu_pci_sample_app)
      -> doca_dpa_create()
      -> doca_dpa_set_app()
      -> doca_dpa_start()
  -> init_dpa_db_thread()
      -> doca_dpa_thread_create()
      -> doca_dpa_thread_set_func_arg(db_handler)
      -> doca_dpa_thread_start()
  -> create_db_dpa_comp()
      -> doca_devemu_pci_db_completion_create()
      -> doca_devemu_pci_db_completion_start()
      -> doca_devemu_pci_db_completion_get_dpa_handle()
  -> doca_dpa_thread_run()
  -> find_emulated_device(vuid)
  -> doca_devemu_pci_dev_create()
  -> doca_ctx_set_datapath_on_dpa()
  -> 注册 ctx state change callback
  -> 注册 FLR callback
  -> doca_ctx_start()
  -> while (!force_quit) doca_pe_progress()
```

DB object 不在创建 `pci_dev` 后立即创建，而是在 context state 进入 `DOCA_CTX_STATE_RUNNING` 时创建：

```text
state_change_event_handler_cb(RUNNING)
  -> create_db_object()
      -> doca_devemu_pci_ep_create_db_on_dpa()
      -> doca_devemu_pci_db_get_dpa_handle()
      -> init_dpa_app_ctx()
          -> DPA RPC init_app_ctx_rpc(db_comp_handle, db_handle)
          -> DPA 上 bind DB 到 DB completion
      -> doca_devemu_pci_db_start()
```

DPA device code 的主干：

```text
init_app_ctx_rpc(db_comp, db)
  -> 保存 db_comp 到 app_ctx
  -> doca_dpa_dev_devemu_pci_db_completion_bind_db()

db_handler()
  -> doca_dpa_dev_devemu_pci_get_db_completion()
  -> doca_dpa_dev_devemu_pci_db_completion_element_get_db_properties()
  -> ack completion
  -> request completion notification
  -> request DB notification
  -> doca_dpa_dev_devemu_pci_db_get_value()
  -> 打印 DB value
  -> reschedule DPA thread
```

FLR path：

```text
flr_event_handler_cb()
  -> destroy_db_object()
      -> doca_devemu_pci_db_stop()
      -> DPA RPC uninit_app_ctx_rpc()
      -> doca_devemu_pci_db_destroy()
  -> doca_ctx_stop(pci_dev ctx)
  -> doca_ctx_start(pci_dev ctx)
  -> 等 ctx 再次 RUNNING 时重建 DB object
```

### 5.3 Host 主干

Host 参数包括 emulated PCI BDF、VFIO group、DB region index、DB index、DB value。

```text
devemu_pci_device_db_host(pci_address, vfio_group, region_idx, db_idx, db_value)
  -> 根据 db_configs[region_idx] 计算 db_offset = db_idx * stride
  -> init_vfio_device()
  -> map_bar_region_memory(DB region)
  -> *((uint32_t *)&db_region.mem[db_offset]) = db_value
```

`db_value` 直接写入 mmap 后的 BAR 地址。DPU 侧通过 DB object 和 DPA completion 收到该 doorbell。

## 6. MSI-X Sample

### 6.1 目标

`msix` 演示 DPU 或 DPA 主动向 Host raise MSI-X vector，Host 通过 VFIO 为每个 vector 绑定 eventfd 并读取事件。

### 6.2 DPU 主干

关键文件：

- `devemu_pci_device_msix/dpu/devemu_pci_device_msix_dpu_main.c`
- `devemu_pci_device_msix/dpu/host/devemu_pci_device_msix_dpu_sample.c`
- `devemu_pci_device_msix/dpu/device/devemu_pci_device_msix_dpu_kernels_dev.c`

调用链：

```text
devemu_pci_device_msix_dpu(pci_address, vuid, msix_idx, msix_on_dpu)
  -> 创建 PE、PCI type、打开支持 hotplug 的 DPU device
  -> configure_and_start_pci_type()
  -> find_emulated_device(vuid)
  -> doca_devemu_pci_dev_create()
  -> 如果走 DPA path:
       init_dpa(devemu_pci_sample_app)
       doca_ctx_set_datapath_on_dpa()
  -> doca_ctx_start()
  -> 校验 hotplug state == POWER_ON
  -> create_msix_object(msix_idx, msix_on_dpu)
       DPU path: doca_devemu_pci_ep_create_msix()
       DPA path: doca_devemu_pci_ep_create_msix_on_dpa()
                 doca_devemu_pci_msix_get_dpa_handle()
  -> raise:
       DPU path: doca_devemu_pci_msix_raise()
       DPA path: doca_dpa_rpc(raise_msix_rpc, msix_handle)
  -> cleanup
```

DPA 侧只有一个 RPC：

```text
raise_msix_rpc(msix)
  -> doca_dpa_dev_devemu_pci_msix_raise(msix)
```

这个 sample 是一次性流程，不进入长期 PE loop。它要求 Host 侧已经在监听 MSI-X，否则中断事件可能没有观察者。

### 6.3 Host 主干

```text
devemu_pci_device_msix_host(pci_address, vfio_group)
  -> init_vfio_device()
  -> map_msix_to_fds()
      -> VFIO_DEVICE_GET_IRQ_INFO(VFIO_PCI_MSIX_IRQ_INDEX)
      -> 为每个 MSI-X vector 创建 eventfd(EFD_NONBLOCK)
      -> VFIO_DEVICE_SET_IRQS，把 vector 绑定到 eventfd
  -> while (!force_quit):
       for msix in [0, PCI_TYPE_NUM_MSIX):
         read(msix_vector_to_fd[msix])
```

Host 端没有指定单个 vector；它监听所有 `PCI_TYPE_NUM_MSIX` 个 vector，读到 eventfd 值时打印触发的 vector index。

## 7. DMA Sample

### 7.1 目标

`dma` 演示 Host 把普通内存映射为 VFIO IOVA，DPU 通过 emulated PCI endpoint 视角创建 remote mmap，再用 DOCA DMA 做 Host <-> DPU 双向 memcpy。

### 7.2 Host 主干

关键文件：

- `devemu_pci_device_dma/host/devemu_pci_device_dma_host_main.c`
- `devemu_pci_device_dma/host/devemu_pci_device_dma_host_sample.c`

Host 使用固定 IOVA `0x1000000`，buffer 大小 4 KiB。

```text
devemu_pci_device_dma_host(pci_address, vfio_group, write_data)
  -> init_vfio_device()
  -> allocate_dma_mem()
      -> mmap anonymous 4 KiB
  -> map_dma_mem(iova=0x1000000)
      -> VFIO_IOMMU_MAP_DMA
      -> flags = READ | WRITE
  -> memcpy(write_data, dma_mem)
  -> loop:
       sleep(2)
       如果 dma_mem 内容与原 write_data 不同:
         打印 DPU 写回的新内容
         break
  -> VFIO_IOMMU_UNMAP_DMA
  -> cleanup
```

Host 必须先运行并保持等待，因为 DPU 端需要用同一个 IOVA 访问这段内存。

### 7.3 DPU 主干

关键文件：

- `devemu_pci_device_dma/dpu/devemu_pci_device_dma_dpu_main.c`
- `devemu_pci_device_dma/dpu/devemu_pci_device_dma_dpu_sample.c`

DPU 入口参数包括 emulation manager PCI address、可选 DMA IB device name、VUID、Host IOVA、可选写回字符串。

```text
devemu_pci_device_dma_dpu(pci_address, devemu_name, vuid, host_iova, write_data)
  -> setup_devemu_resources()
      -> doca_pe_create()
      -> doca_devemu_pci_type_create()
      -> find_supported_device()
      -> configure_and_start_pci_type()
      -> find_emulated_device(vuid)
      -> doca_devemu_pci_dev_create()
      -> doca_ctx_start()
  -> setup_dma_ctx()
      -> 如果 devemu_name 为空，复用 devemu manager device
      -> 否则按 IB device name 打开 DMA-capable device
      -> doca_dma_create()
      -> doca_dma_task_memcpy_set_conf(completion_cb, error_cb)
      -> doca_ctx_set_user_data()
      -> doca_pe_connect_ctx(devemu_res.pe, dma_ctx)
      -> doca_ctx_start(dma_ctx)
  -> setup_remote_mmap(remote_addr = host_iova, len = 4 KiB)
      -> doca_devemu_pci_ep_mmap_create()
      -> doca_mmap_set_max_num_devices(1)
      -> doca_mmap_add_dev(dma_dev)
      -> doca_mmap_set_permissions(LOCAL_READ_WRITE)
      -> doca_mmap_set_memrange(host_iova, len)
      -> doca_mmap_start()
  -> 分配 local_mem_buf，并 setup_local_mmap()
  -> setup_buf_inventory(max_bufs=2)
  -> do_dma_copy(remote -> local)
  -> 如果 write_data 非空:
       strncpy(write_data, local_mem_buf)
       do_dma_copy(local -> remote)
  -> cleanup
```

`do_dma_copy()` 是该 sample 的核心：

```text
do_dma_copy(src_mmap, src_addr, dst_mmap, dst_addr, len)
  -> doca_buf_inventory_buf_get_by_addr(src)
  -> doca_buf_inventory_buf_get_by_addr(dst)
  -> doca_dma_task_memcpy_alloc_init()
  -> doca_buf_set_data(src)
  -> doca_task_submit()
  -> while num_remaining_tasks != 0:
       doca_pe_progress()
  -> completion/error callback 释放 task，写入 task_result
```

这里的 remote mmap 来自 `doca_devemu_pci_ep_mmap_create()`，所以 DPU 访问的是 Host 为 emulated PCI device 暴露出的 DMA 地址空间。

## 8. TLP Handler Sample

### 8.1 目标

`tlp_handler` 与其他四个 sample 不同：它不是让 SDK 处理普通 DevEmu PCI device context，而是创建 `doca_devemu_pci_tlp_type`、`doca_devemu_pci_tlp_channel` 和 `doca_devemu_pci_tlp_dev`，自己处理 Host 发来的 raw PCIe TLP。

它实现一个单 function endpoint，要求 TLP channel downstream port 数量为 1。代码还包含 live-upgrade handover：一个 destination 进程可以从 source 进程接管 TLP channel。

### 8.2 DPU 初始化主干

关键文件：

- `devemu_pci_device_tlp_handler/dpu/devemu_pci_device_tlp_handler_dpu_main.c`
- `devemu_pci_device_tlp_handler/dpu/devemu_pci_device_tlp_handler_dpu_sample.c`

调用链：

```text
devemu_pci_device_tlp_handler_dpu(pci_address, is_handover_destination, shm_dir_path)
  -> find_supported_tlp_device()
      -> 匹配 PCI address
      -> doca_devemu_pci_tlp_cap_get_max_types() > 0
      -> doca_dev_open()
  -> doca_devemu_pci_tlp_type_create(TLP_PCI_TYPE_NAME)
  -> configure_and_start_pci_tlp_type()
      -> doca_devemu_pci_type_set_dev()
      -> doca_devemu_pci_tlp_type_set_pci_cap_conf()
      -> doca_devemu_pci_tlp_type_set_pcie_cap_conf()
      -> 设置 BAR layout、DB region、transaction region、MSI-X table、PBA
      -> doca_devemu_pci_type_start()
  -> query_existing_tlp_reps()
      -> 找到已有 representor: 当前进程不是 channel owner
      -> 找不到 representor: 当前进程创建 channel 和 representor，成为 owner
  -> init_tlp_dev()
      owner:     doca_devemu_pci_tlp_dev_create() + start()
      non-owner: doca_devemu_pci_tlp_dev_create_started()
  -> init_pci_config_space()
  -> init_tlp_handler_cxt()
  -> tlp_init_transaction_region()
  -> tlp_init_exp_bar()
  -> 如果是 handover destination:
       initiate_tlp_channel_handover()
  -> 主循环:
       owner 持续 doca_pe_progress()
       如果启用 shm_dir_path，还同时监听 Unix socket handover 请求
```

`init_tlp_channel()` 负责 channel 生命周期：

```text
init_tlp_channel(resources, export_desc, export_desc_len)
  -> doca_pe_create()
  -> 如果有 export_desc:
       doca_devemu_pci_tlp_channel_create_from_export()
       doca_devemu_pci_tlp_channel_set_primary(false)
     否则:
       doca_devemu_pci_tlp_channel_create()
       doca_devemu_pci_tlp_channel_set_primary(true)
       doca_devemu_pci_tlp_channel_set_shm_dir_path()
  -> doca_devemu_pci_tlp_channel_set_req_user_data_size()
  -> doca_devemu_pci_tlp_channel_event_req_register(tlp_req_handler_cb)
  -> channel_ctx = doca_devemu_pci_tlp_channel_as_ctx()
  -> doca_pe_connect_ctx()
  -> doca_ctx_set_user_data(resources)
  -> doca_ctx_start()
  -> doca_devemu_pci_tlp_channel_get_num_dsp()，要求为 1
```

### 8.3 TLP 请求处理主干

TLP request 的入口是 `tlp_req_handler_cb()`：

```text
tlp_req_handler_cb(channel, tlp_req, req_user_data)
  -> 从 channel ctx user_data 取 resources
  -> opcode == PCI_EVENT:
       handle_tlp_req_pci_event()
       doca_devemu_pci_tlp_channel_req_complete_pci_event()
       return
  -> 对 config type 0 请求:
       capture_device_bus_number()
       只接受 device=0,function=0，否则返回 Unsupported Request completion
  -> 清空 completion 字段
  -> handle_tlp_req()
```

`handle_tlp_req()` 先解析 TLP header 的 `fmt/type`，再按请求类型分派：

```text
CONFIG_READ_TYPE_0
  -> handle_tlp_req_read_type_0()
      -> ext_reg_num 0..15: 读 config header
      -> ext_reg_num 16..63: 读 PCI capability
      -> ext_reg_num 64..1023: 读 PCIe extended capability
      -> 填 completion data/header
  -> doca_devemu_pci_tlp_channel_req_complete_config_read()

CONFIG_WRITE_TYPE_0
  -> handle_tlp_req_write_type_0()
      -> 依据 first_dw_be 生成写 mask
      -> 更新 config header 或 capability
      -> 填 completion header
  -> doca_devemu_pci_tlp_channel_req_complete_config_write()

MEMORY_READ
  -> parse 32-bit 或 64-bit address
  -> get_memory_read_data()
      -> 优先匹配 transaction region
      -> 其次匹配 expansion ROM BAR
      -> 不匹配则返回 dummy pattern
      -> 按 first/last DW byte enable 组装 data
  -> 填 completion data/header
  -> doca_devemu_pci_tlp_channel_req_complete_tlp(non_posted=true)

MEMORY_WRITE
  -> parse 32-bit 或 64-bit address
  -> set_memory_write_data()
      -> 只写 transaction region
      -> 按 byte enable 更新内存
  -> doca_devemu_pci_tlp_channel_req_complete_tlp(non_posted=false)

其他类型
  -> 返回 Unsupported Request completion
```

`transaction_region_memory` 是 DPU 进程内 malloc 的 buffer。Host 访问 BAR0 transaction window 时，TLP handler 根据 Host 写入的 BAR base 和 `transaction_configs[0].start_address` 计算 offset，把 memory read/write 映射到这段 buffer。

### 8.4 Live Upgrade Handover

启用 `--shm-dir-path` 后，source 进程创建 primary channel 并监听 Unix domain socket。destination 进程用 `--handover-destination` 启动，执行四阶段协议：

```text
SETUP
  destination -> source: SETUP
  source -> destination: SETUP_ACK + ibverbs cmd_fd(SCM_RIGHTS)
  destination: ibv_import_device()，doca_rdma_bridge_open_dev_from_pd()

EXPORT
  destination -> source: EXPORT
  source: doca_devemu_pci_tlp_channel_export()
  source -> destination: export_desc_len + export_desc
  destination: doca_devemu_pci_tlp_channel_create_from_export()

BEGIN
  destination -> source: BEGIN
  source:
    doca_devemu_pci_tlp_channel_set_primary(false)
    doca_ctx_stop(channel_ctx)，drain PE 到 IDLE
    发送 pci_config_space、tlp_handler_cxt、transaction region、exp ROM buffer
  destination:
    接收状态
    doca_devemu_pci_tlp_channel_set_primary(true)

END
  destination -> source: END + success/failure
  success: source destroy channel 并退出；destination 成为 owner
  failure: source 尝试重新 set_primary(true) 并 restart channel ctx
```

这个流程的核心不只是 channel export，还包括 sample 自己维护的 PCI config space、TLP handler context、transaction region 和 expansion ROM buffer 的同步。

### 8.5 Host 主干

Host 侧只是测试 transaction region：

```text
devemu_pci_device_tlp_handler_host(pci_address, vfio_group, region_index, write_data)
  -> 检查 write_data 不超过 transaction_config->size
  -> init_vfio_device()
  -> map_bar_region_memory(transaction_configs[region_index])
  -> 如果 write_data 为空:
       hex_dump(transaction_region.mem)
     否则:
       memcpy(transaction_region.mem, write_data, data_len)
```

由于 DPU 侧处理的是 raw TLP，这里的 Host mmap 读写会转化为 TLP memory read/write，请求最终落到 `tlp_req_handler_cb()`。

## 9. Stateful Region Sample

### 9.1 目标

`stateful_region` 演示 SDK 管理的 BAR stateful region：Host 写入 BAR region 后，DPU 收到 driver write event，再通过 query API 读取该 region 的完整内容。

### 9.2 DPU 主干

关键文件：

- `devemu_pci_device_stateful_region/dpu/devemu_pci_device_stateful_region_dpu_main.c`
- `devemu_pci_device_stateful_region/dpu/devemu_pci_device_stateful_region_dpu_sample.c`

调用链：

```text
devemu_pci_device_stateful_region_dpu(pci_address, vuid)
  -> 检查 PCI_TYPE_NUM_BAR_STATEFUL_REGIONS > 0
  -> 创建 PE、PCI type、打开支持 hotplug 的 DPU device
  -> configure_and_start_pci_type()
  -> find_emulated_device(vuid)
  -> doca_devemu_pci_dev_create()
  -> register_to_stateful_region_write_events()
      -> 遍历 stateful_configs[]
      -> doca_devemu_pci_dev_event_bar_stateful_region_driver_write_register()
      -> doca_ctx_set_user_data(resources)
      -> calloc(max_region_size) 作为 query buffer
  -> register_to_flr_events()
  -> doca_ctx_start()
  -> 校验 hotplug state == POWER_ON
  -> while (!force_quit):
       doca_pe_progress()
```

写事件回调：

```text
stateful_region_write_event_handler_cb(event, user_data=config)
  -> 从 event 取 pci_dev
  -> 从 ctx user_data 取 resources
  -> doca_devemu_pci_dev_query_bar_stateful_region_values(
       pci_dev, bar_id, start_address, stateful_region_values, size)
  -> hex_dump() 并打印
```

FLR path 和 DB 类似，但不涉及 DB object，只 stop/start `pci_dev` context。

### 9.3 Host 主干

```text
devemu_pci_device_stateful_region_host(pci_address, vfio_group, region_index, write_data)
  -> 检查 stateful region 存在
  -> 检查 write_data 不超过 region size
  -> init_vfio_device()
  -> map_bar_region_memory(stateful_configs[region_index])
  -> 如果 write_data 为空:
       hex_dump(stateful_region.mem)
     否则:
       memcpy(stateful_region.mem, write_data, data_len)
```

Host 写 mmap 后，DPU 侧由 DevEmu SDK 触发 stateful write event；DPU 不直接 mmap Host BAR，而是通过 `doca_devemu_pci_dev_query_bar_stateful_region_values()` 取值。

## 10. 开发判断

开发或调试这些 sample 时，可以按下面几条判断路径快速定位问题：

1. Host 看不到设备：先查 hotplug sample 是否已创建 endpoint、Host 是否重新枚举、VUID 是否正确。`db/msix/dma/stateful_region` 都假设 representor 已存在。
2. Host VFIO 初始化失败：检查设备是否绑定到 `vfio-pci`、IOMMU group 是否 viable、`/dev/vfio/<group>` 是否正确。
3. BAR mmap 越界：检查 `devemu_pci_type_config.h` 中 region 的 `bar_id/start_address/size` 是否与 Host 参数匹配。
4. DB 无 completion：检查 DPU 是否已进入 `DOCA_CTX_STATE_RUNNING` 并完成 DB bind/start；Host 写入的 `db_idx` 是否映射到 DPU 监听的 `db_id`。
5. MSI-X 无事件：先启动 Host listener，再触发 DPU sample；确认 `msix_idx < PCI_TYPE_NUM_MSIX`。
6. DMA 不通：确认 Host 已 `VFIO_IOMMU_MAP_DMA` 到 DPU 使用的同一 IOVA；DPU 端 remote mmap 的地址和长度必须覆盖 Host buffer。
7. TLP handler 不响应：确认当前进程是 channel owner，`doca_pe_progress()` 在跑；TLP channel downstream port 数量必须为 1。
8. Stateful event 不触发：确认 DPU 已注册 stateful region write event 且 ctx 已 start；Host 写入的是 stateful region 而不是 transaction region。

## 11. 未来扩展设计：VFIO Admin Queue 抽象设备

本节是未来扩展设计，不是当前 `samples/doca_devemu` 已实现的 sample 功能。目标是实现一个可复用的
VFIO-only 抽象设备：Host 设备绑定 `vfio-pci`，Host userspace 自己完成 BAR mmap、DMA map、MSI-X
eventfd 绑定、doorbell 写入和协议解析；DPU 侧拆成 `pci-fe` 和 `dev-be` 两个程序。

推荐采用 hotplug 托管模型，而不是让 `pci-fe` 代理所有数据面：

- `pci-fe`：PCI frontend/provisioning daemon，常驻运行，负责创建、清理和维护 Host 可见的 emulated PCI
  endpoint。它配置 BAR/DB/MSI-X/stateful/admin region layout，hotplug 设备，发布 VUID/BDF/layout/generation，
  并在退出时协调安全 hot-unplug。正常运行期它不创建 DB/MSI-X/DMA runtime object，也不处理业务 doorbell。
- `dev-be`：device backend/datapath owner，按需启动和退出。它读取 `pci-fe` 发布的 VUID/layout，attach 到已有
  endpoint，实际创建 DB completion、DB object、MSI-X object、DMA context/remote mmap，实际响应 Host DB、发出
  MSI-X、发起 DMA read/write，并实现 admin queue opcode。

这个拆分更接近 `db/msix/dma/stateful_region` sample 的模型：设备可以先由 hotplug/provisioning 程序创建出来，
后续业务程序再通过 VUID 找到已有 representor 并接管 datapath。代价是：`dev-be` 不在时，Host 业务 command
不会有 DPU completion，Host userspace 必须有 timeout/retry；`pci-fe` 不承诺替 `dev-be` 返回
`BACKEND_DOWN` completion。

### 11.1 总体分层

```text
Host userspace VFIO driver
  -> 等待 emulated PCI device 存在并绑定 vfio-pci
  -> VFIO 初始化、BAR mmap、MSI-X eventfd、DMA map/unmap
  -> 在 admin/stateful region 写 cmd/rsp IOVA、length、MSI-X vector、queue_enable
  -> 写 DB region 提交 command
  -> 收 MSI-X 或按 timeout poll response buffer

DPU pci-fe
  -> 常驻，负责 provisioning/lifecycle
  -> 创建/start PCI type，配置 BAR/DB/MSI-X/PBA/stateful region
  -> 创建或清理 emulated endpoint，保证 Host 能枚举设备
  -> 发布 VUID、BDF、BAR layout、DB/MSI-X/stateful region、generation
  -> 正常运行期不持有 datapath owner，不处理业务 DB/MSI-X/DMA
  -> 退出时先协调 dev-be 停止，再 hot-unplug/destroy endpoint

DPU dev-be
  -> 读取 pci-fe 发布的 VUID/layout/generation
  -> 创建同名/同配置 PCI type，用 VUID find_emulated_device()
  -> doca_devemu_pci_dev_create() attach 到已有 endpoint
  -> 创建 DB completion/DB object，创建 MSI-X object，创建 DMA context/remote mmap
  -> 从 admin/stateful region 获取 Host 发布的 IOVA/length/vector
  -> 收 DB 后 DMA 读 command、执行业务、DMA 写 response、raise MSI-X
```

第一版只做一个 admin queue，不做完整 virtqueue descriptor ring。这样可以先验证 hotplug 托管、VFIO BAR mmap、
stateful/admin 配置区、DB、MSI-X、DMA 的闭环，避免一开始就处理 descriptor chain、used ring、available ring、
indirect descriptor、reset race 和标准 VirtIO driver 兼容等复杂度。

### 11.2 程序边界与所有权

`pci-fe` 和 `dev-be` 的边界必须围绕 DOCA object owner 设计。`pci-fe` 只拥有 provisioning/lifecycle 对象；
`dev-be` 是 runtime datapath owner。

```text
pci-fe owns:
  PCI type provisioning config
  endpoint creation/removal intent
  emulated endpoint lifecycle metadata
  published VUID/BDF/layout/generation file
  optional lifecycle IPC with dev-be

pci-fe should not own during normal datapath:
  doca_devemu_pci_dev runtime ctx
  DB completion / DB object
  MSI-X object
  DMA context / remote mmap / buf inventory
  command inflight state

dev-be owns:
  opened representor for the published VUID
  doca_devemu_pci_dev runtime ctx
  DB completion / DB object
  MSI-X object
  DMA context / remote mmap / buf inventory
  admin queue runtime state
  opcode implementation and business resources
```

`pci-fe` 创建 endpoint 后，应避免长期持有会阻止 `dev-be` attach 的 runtime handle。可行做法是：`pci-fe` 创建
representor/hotplug endpoint 后，只保留生命周期元数据；如果 SDK 要求打开 representor 才能创建 endpoint，创建完成后应
close representor 而不是 destroy representor。`dev-be` 再按 VUID 打开同一个 representor 并创建
`doca_devemu_pci_dev`。

`pci-fe` 与 `dev-be` 的 attach 规则如下：

- `pci-fe` 可以常驻并保持 PCI type started，同时发布 VUID/layout/generation。
- `dev-be` 自己创建同名同配置 PCI type，通过 rep list 找到 `pci-fe` 发布的 VUID，打开 representor，并独占创建
  `doca_devemu_pci_dev` runtime ctx。
- 同一 VUID 只能有一个已 start 的 `doca_devemu_pci_dev` ctx。`pci-fe` 不应长期持有该 ctx，也不应创建
  DB/MSI-X/DMA runtime object。
- 第一版不需要把 `pci-fe` 降级为短生命周期 provisioning step，也不需要由 `pci-fe` 代理 attach/query API。

`pci-fe` 与 `dev-be` 可以保留一个很小的 lifecycle IPC，但不走 per-command 数据面：

```text
DEV_BE_REGISTER(pid, generation, vuid)
DEV_BE_READY(runtime_feature_bits)
DEV_BE_QUIESCE_REQUEST(reason)
DEV_BE_QUIESCE_DONE()
DEV_BE_EXITING()
HEARTBEAT(generation)
```

该 IPC 只用于启动顺序、健康检查和安全关停，不传 command payload，不代理 DMA。

### 11.3 pci-fe 发布的设备描述

`pci-fe` 创建或发现 endpoint 后，向本地文件或 Unix socket 发布设备描述。`dev-be` 和运维脚本以它为准，不在源码里硬编码
VUID/BDF。

```json
{
  "name": "adminq0",
  "generation": 12,
  "vuid": "...",
  "host_bdf": "0000:48:00.0",
  "type_name": "adminq",
  "bar": 0,
  "bar_log_size": 14,
  "db_region_index": 0,
  "db_region_offset": 0,
  "db_region_size": 4096,
  "db_index": 0,
  "msix_table_offset": 4096,
  "msix_pba_offset": 8192,
  "num_msix": 4,
  "admin_region_offset": 12288,
  "admin_region_size": 256
}
```

建议带 `generation`。`pci-fe` 每次重新创建 endpoint 都递增 generation；`dev-be` attach 后如果发现 generation 变化，必须
销毁 runtime object 并重新 attach。Host userspace 也可以把 generation 写入 command header，避免使用旧设备残留状态。

### 11.4 PCI/BAR layout

如果使用普通 DevEmu hotplug 托管模型，优先沿用现有 sample 的 16 KiB BAR0，减少未知数：

| BAR0 offset | size | 名称 | Host 访问 | `pci-fe` 处理 | `dev-be` 处理 |
| ---: | ---: | --- | --- | --- | --- |
| `0x0000` | `0x1000` | DB / doorbell region | MMIO write | 配置 region | 创建 DB object，收 Host doorbell |
| `0x1000` | `0x1000` | MSI-X table | VFIO/MSI-X | 配置 region | 创建 MSI-X object 并 raise vector |
| `0x2000` | `0x1000` | MSI-X PBA | VFIO/MSI-X | 配置 region | 由 MSI-X 机制使用 |
| `0x3000` | `0x100` 或 `0x1000` | admin/stateful config region | MMIO write/read mmap | 配置 stateful/admin region | query Host 写入的 IOVA/len/vector/queue_enable |

如果后续需要 vnet/vblk 风格的 `0x4000` notify window，可以把 BAR0 扩到 32 KiB：

| BAR0 offset | size | 名称 | 用途 |
| ---: | ---: | --- | --- |
| `0x0000` | `0x100` | future common/admin window | 仅 TLP mode 下可做动态 MMIO read/write |
| `0x1000` | `0x1000` | MSI-X table | Host 写 vector message address/data |
| `0x2000` | `0x1000` | MSI-X PBA | pending bit array |
| `0x3000` | `0x1000` | admin/stateful config region | Host 发布 queue DMA config |
| `0x4000` | `0x1000` | DB / notify | Host doorbell |

普通托管路径下，不要假设 DPU 可以像 TLP handler 那样任意响应 BAR MMIO read completion。Host->DPU 的配置通道应使用
stateful/admin region；DPU->Host 的完成状态应写入 Host DMA response buffer，并通过 MSI-X 通知 Host。

### 11.5 Capability 与 PCI type

第一版建议使用 vendor-specific PCI device ID。普通 DevEmu 托管路径如果不能配置任意 vendor-specific capability，
就不要依赖 capability 发现 admin layout，而是依赖 `pci-fe` 发布的 layout 文件和 Host/DPU 共享头文件。

不要暴露 `1af4` VirtIO vendor/device ID，也不要暴露完整 VirtIO PCI capability chain 给 Host 内核 `virtio-pci`
driver。标准 virtio driver 一旦绑定，就会执行 feature negotiation、common_cfg 读写、queue setup 和 device_status
状态机；普通 hotplug 托管路径不能等价实现 vnet/vblk 那类动态 VirtIO PCI MMIO/TLP 语义。

实现选择分两档：

```text
VFIO admin queue v1:
  普通 DevEmu PCI type
  vendor-specific PCI device ID
  fixed BAR/DB/MSI-X/stateful layout
  pci-fe creates/provisions endpoint
  dev-be owns DB/MSI-X/DMA runtime
  Host 只绑定 vfio-pci

Future VirtIO PCI:
  TLP type
  pci-fe 完整响应 Host config/MMIO TLP
  暴露标准 VirtIO PCI capability chain
  支持 Linux virtio-pci 原生 probe
```

标准 VirtIO PCI 能力只作为未来扩展预留，不在第一版 Host-visible config space 中启用。可以在发布文件或私有协议里记录：

```text
ADMINQ_F_FUTURE_VIRTIO_PCI_RESERVED = 1
ADMINQ_F_STANDARD_VIRTIO_PCI_ENABLED = 0
```

### 11.6 Admin/stateful config region

普通托管路径下，BAR 内 admin region 更适合作为 Host 写给 DPU 的配置结构，而不是 TLP 动态寄存器。Host 通过 VFIO mmap
该 region，写入 queue DMA 地址、长度、MSI-X vector 和 enable 位；`dev-be` 通过 stateful write event 或 query API
读取这些值。

```c
#define ADMINQ_MAGIC 0x41445130u /* "ADQ0" */

struct adminq_host_cfg {
    uint32_t magic;              /* Host writes ADMINQ_MAGIC */
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t generation;         /* Must match pci-fe published generation */

    uint64_t cmd_iova;           /* Host DMA IOVA of command buffer */
    uint32_t cmd_len;            /* command buffer length */
    uint32_t cmd_stride;         /* one command slot size, v1 may equal cmd_len */

    uint64_t rsp_iova;           /* Host DMA IOVA of response buffer */
    uint32_t rsp_len;            /* response buffer length */
    uint32_t rsp_stride;         /* one response slot size */

    uint16_t msix_vector;        /* completion MSI-X vector */
    uint16_t queue_enable;       /* 0/1 */
    uint32_t producer_seq;       /* Host updates before DB */
    uint32_t flags;
};
```

不要把 `consumer_seq`、`last_status` 这类 DPU 更新字段放在普通托管 BAR register 语义里作为第一版依赖。完成状态放在
response DMA buffer 中：

```c
struct adminq_rsp_hdr {
    uint32_t seq;
    uint32_t status;
    uint32_t actual_output_len;
    uint32_t reserved;
};
```

如果后续切到 TLP mode，才可以把 `consumer_seq`、`last_status`、`backend_status` 做成真正由 `pci-fe` 或 `dev-be`
动态返回的 MMIO read register。

### 11.7 Command/response buffer

Host 通过 VFIO 分配并映射 DMA buffer。v1 固定为“一次一个 command”，不做 ring：

```c
struct adminq_cmd_hdr {
    uint32_t opcode;
    uint32_t flags;
    uint32_t seq;
    uint32_t input_len;
    uint32_t output_len;
    uint32_t generation;
};

enum adminq_cmd_status {
    ADMINQ_STATUS_OK = 0,
    ADMINQ_STATUS_INVALID = 1,
    ADMINQ_STATUS_BACKEND_NOT_READY = 2,
    ADMINQ_STATUS_TIMEOUT = 3,
    ADMINQ_STATUS_IO_ERROR = 4,
    ADMINQ_STATUS_GENERATION_MISMATCH = 5,
};
```

Host 写入 command buffer：

```text
cmd_iova -> adminq_cmd_hdr + payload
rsp_iova -> adminq_rsp_hdr + payload space
```

`dev-be` 收 DB 后：

```text
query/read adminq_host_cfg from stateful/admin region
validate magic/version/generation/queue_enable/cmd_iova/rsp_iova/len/vector
create/update remote mmap if Host config changed
DMA read cmd_iova, cmd_len
validate cmd.seq, cmd.input_len, cmd.output_len, cmd.generation
execute opcode
DMA write rsp_iova, rsp_len
ensure MSI-X object for msix_vector
raise MSI-X
```

如果 `dev-be` 不在，Host 写 DB 不会得到 completion。Host userspace 必须给 command 等待设置 timeout；timeout 后可以重试
`GET_STATUS`/`NOP` 类 command，或要求运维先启动 `dev-be`。

后续要扩展吞吐时，再把 `cmd_iova/rsp_iova` 改成 ring base，增加 `queue_size`、`head/tail`、slot stride；这一步再接近
VirtIO virtqueue。

### 11.8 Host VFIO 流程

Host userspace 初始化流程：

```text
wait device exists and is bound to vfio-pci
init_vfio_device()
  -> open /dev/vfio/vfio
  -> open /dev/vfio/<group>
  -> VFIO_GROUP_SET_CONTAINER
  -> VFIO_SET_IOMMU(VFIO_TYPE1v2_IOMMU)
  -> VFIO_GROUP_GET_DEVICE_FD
  -> enable PCI command: memory space + bus master

map BAR0 admin/stateful region and DB region
  -> VFIO_DEVICE_GET_REGION_INFO(VFIO_PCI_BAR0_REGION_INDEX)
  -> mmap admin/stateful region
  -> mmap DB region

setup MSI-X
  -> VFIO_DEVICE_GET_IRQ_INFO(VFIO_PCI_MSIX_IRQ_INDEX)
  -> eventfd(EFD_NONBLOCK)
  -> VFIO_DEVICE_SET_IRQS(vector -> eventfd)

setup DMA
  -> mmap anonymous or hugepage memory for cmd/rsp
  -> VFIO_IOMMU_MAP_DMA(cmd_iova, cmd_len)
  -> VFIO_IOMMU_MAP_DMA(rsp_iova, rsp_len)

publish queue
  -> write adminq_host_cfg: magic/generation/cmd_iova/cmd_len/rsp_iova/rsp_len/msix_vector
  -> store barrier
  -> write queue_enable = 1
```

提交 command：

```text
fill command buffer
store barrier
write producer_seq in adminq_host_cfg
MMIO write DB offset: value = producer_seq or qid
poll eventfd with timeout
read response buffer and validate seq/status/generation
```

Host 侧必须把设备绑定到 `vfio-pci`。如果绑定到内核功能驱动，userspace 就无法直接 mmap BAR 和管理 MSI-X/DMA。
Host 程序可以早于 `dev-be` 启动，但提交 command 必须有 timeout；如果 `dev-be` 尚未 attach DB，就不会有 MSI-X completion。
普通托管路径下，Host 不应依赖 BAR read 观察 DPU 进度；completion 以 response DMA buffer 和 MSI-X/eventfd 为准。

### 11.9 pci-fe 流程

`pci-fe` 启动流程：

```text
take singleton lock
open DOCA dev
create PE if provisioning API requires it
create PCI type
configure vendor/device/class
configure BAR layout
configure DB region
configure MSI-X table/PBA region
configure stateful/admin region
start PCI type
cleanup stale endpoint owned by previous generation
create endpoint / representor / hotplug device
publish VUID/BDF/layout/generation
close runtime handles that would block dev-be attach, but do not destroy endpoint
monitor lifecycle IPC / signals
```

启动时总清理旧残留是正确方向，但不能把“强制 destroy”作为第一步。建议用 owner tag/generation 识别旧设备：

```text
if stale endpoint belongs to this pci-fe:
  ask dev-be to quiesce if it is running
  wait dev-be close/destroy runtime objects with timeout
  hot-unplug or destroy old representor
  wait Host remove/ack with timeout
  force cleanup only after timeout
create endpoint with new generation
```

如果 Host 正在启动或正在枚举，强制清理仍可能造成 AER 或 completion timeout。因此生产流程应尽量让 `pci-fe` 在 Host
power cycle 前启动完成；异常恢复时才走 timeout 后 force cleanup。

`pci-fe` 正常运行期主要做 health/lifecycle 工作：

```text
serve /run/devemu-adminq/adminq0.json
watch dev-be heartbeat if lifecycle IPC is enabled
on SIGTERM:
  ask dev-be quiesce
  wait dev-be done or timeout
  hot-unplug endpoint
  destroy provisioning objects
```

### 11.10 dev-be 流程

`dev-be` 前置条件是 `pci-fe` 已创建 emulated PCI endpoint 并发布 VUID/layout。`dev-be` 是真正 datapath owner。

```text
read /run/devemu-adminq/adminq0.json
open DOCA dev
create PE
create same-name/same-config PCI type
start PCI type
find_emulated_device(vuid)
open representor
doca_devemu_pci_dev_create()
register FLR/stateful write callbacks
start pci_dev ctx
wait ctx RUNNING / hotplug state POWER_ON
create DB completion and DB object
create DMA context / buf inventory / local buffers
mark dev-be READY in lifecycle IPC or log
main loop: progress PE + DB/DMA completions
```

DB path：

```text
DB completion arrives
  -> read DB value / producer_seq
  -> query admin/stateful config
  -> validate generation and queue_enable
  -> update remote mmap if cmd/rsp IOVA changed
  -> submit DMA read command
  -> execute opcode
  -> submit DMA write response
  -> raise MSI-X
```

MSI-X object 可以在 queue enable 时创建，也可以在第一次 completion 前 lazy create：

```text
if msix == NULL or cached_vector != cfg.msix_vector:
    destroy old msix
    doca_devemu_pci_ep_create_msix(pci_ep, BAR0, MSIX_TABLE_OFFSET,
                                   cfg.msix_vector, &msix)
doca_devemu_pci_msix_raise(msix)
```

`dev-be` 崩溃或退出时，Host 可见设备仍由 `pci-fe` 保持存在；但业务 DB 不会被消费。Host userspace 应 timeout，
并在 `dev-be` 恢复后重新发布 queue config 或重新提交 command。

### 11.11 状态机、reset 与关停

设备级状态由 `pci-fe` 维护在本地元数据中，不要求 Host 通过 BAR 动态读取：

```text
ABSENT
  -> pci-fe start
  -> PRESENT_NO_BE
  -> PRESENT_BE_ATTACHED
  -> QUIESCING
  -> UNPLUGGING
  -> ABSENT
```

queue/runtime 状态由 `dev-be` 维护：

```text
DETACHED
  dev-be attach VUID and ctx RUNNING
  -> ATTACHED

ATTACHED
  Host 写 adminq_host_cfg.queue_enable=1 且 IOVA/len/vector 合法
  -> CONFIGURED

CONFIGURED
  dev-be remote mmap 创建成功
  -> RUNNING

RUNNING
  Host DB
  -> BUSY

BUSY
  DMA/read/execute/write/MSI-X 完成
  -> RUNNING

任意状态:
  Host FLR 或 generation mismatch
  -> 停止接收新 DB
  -> drain 或取消 in-flight DMA
  -> destroy msix/mmap/DB runtime state
  -> ATTACHED 或 DETACHED
```

`dev-be` 正常退出：

```text
dev-be:
  stop accepting new DB
  wait current command done or timeout
  destroy DB object / DB completion
  destroy MSI-X object
  destroy DMA remote mmap / context / buf inventory
  stop/destroy pci_dev ctx
  close representor
  notify pci-fe lifecycle IPC if enabled
  exit

pci-fe:
  keep endpoint present
  mark local metadata as PRESENT_NO_BE
```

`pci-fe` 正常退出才负责安全移除设备，顺序必须先停 `dev-be`：

```text
pci-fe:
  ask dev-be to quiesce
  wait dev-be done or timeout
  if timeout: record forced cleanup and continue carefully
  hot-unplug endpoint
  destroy representor/provisioning objects
  exit
```

等待 Host 或 `dev-be` 确认必须有超时。Host 可能已经重启、Host VFIO app 可能崩溃，或 `dev-be` 可能已经异常退出。
超时后 `pci-fe` 应进入 forced unplug/cleanup，并记录 generation，下一次启动继续识别和清理 stale endpoint。

FLR、hot-unplug、Host unmap DMA 时必须让 `dev-be` 缓存的 IOVA 失效。Host 应先写 `queue_enable=0`，等待当前
command completion 或 timeout，再 `VFIO_IOMMU_UNMAP_DMA`。`dev-be` 如果在 BUSY 状态收到 FLR/reset，应拒绝后续 DB，
取消未开始的 DMA，并销毁 remote mmap/MSI-X runtime state。

### 11.12 内存顺序和确认

Host 提交 command 的顺序：

```text
写 command buffer
store barrier
写 producer_seq 到 adminq_host_cfg
store/MMIO barrier
MMIO write DB
```

`dev-be` 完成 command 的顺序：

```text
DMA write response buffer
确认 DMA completion
raise MSI-X
```

Host 收到 MSI-X 后仍应读取 response header 校验 `seq/status/generation`，不要只依赖 eventfd 次数判断完成。eventfd
只表示有中断，不携带 command id。Host 超时后不能假设 command 没被执行；第一版应把 opcode 设计成可重试或带 seq 去重。

### 11.13 第一版边界

第一版建议只支持：

- `pci-fe` + `dev-be` 两进程，`pci-fe` 是 provisioning/lifecycle owner，`dev-be` 是 datapath owner。
- 1 个 admin queue。
- 1 个 in-flight command。
- 固定 command/response buffer 长度，例如 4 KiB。
- 1 个 MSI-X vector。
- doorbell value 只携带 `producer_seq` 或固定 `qid=0`。
- Host/DPU 共享同一个协议头文件，layout 固定。
- `pci-fe` 发布 VUID/BDF/layout/generation；`dev-be` 按 VUID attach。
- `dev-be` 实际创建并使用 DB/MSI-X/DMA runtime object。

暂时不做：

- `pci-fe` 代理业务 DB、MSI-X 或 DMA。
- `pci-fe` 代替 `dev-be` 返回 `BACKEND_DOWN` completion。
- 标准 VirtIO PCI endpoint 暴露。
- Linux `virtio-pci` / `virtio-net` / `virtio-blk` driver 兼容。
- 完整 VirtIO descriptor ring。
- 多 queue。
- indirect descriptor。
- packed ring。
- live update/handover。

这样可以把风险集中在五件事：`pci-fe` 是否能稳定创建和清理 Host 可见 PCI endpoint，`dev-be` 是否能可靠 attach
已有 VUID，VFIO BAR/DB/MSI-X 是否按 layout 工作，IOVA 是否能被 `dev-be` 通过 DOCA DMA 访问，Host timeout/retry
是否能覆盖 `dev-be` 不在或崩溃的窗口。
