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
