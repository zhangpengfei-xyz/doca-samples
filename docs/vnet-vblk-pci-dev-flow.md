# vnet_pci_dev 与 vblk_pci_dev 主干代码流程

本文档基于 `docs/architecture-design.md`、`docs/run-guide.md` 以及
`applications/vnet_pci_dev`、`applications/vblk_pci_dev` 当前源码整理。目标是抽取两个 VirtIO PCI
DevEmu 应用的主干流程，并按功能块重新划分 DOCA API 调用，而不是逐个复述现有函数边界。

## 1. 范围与前提

主干流程按运行文档中的基础测试路径理解：

- DPU 端先启动 `doca_vnet_pci_dev` 或 `doca_vblk_pci_dev` 并保持运行。
- Host 侧通过 BMC/IPMI power cycle，在启动 PCI 扫描阶段枚举 DPU 暴露的虚拟 PCI bridge 和 VirtIO endpoint。
- 以 static mode 为主线，即启动时创建 endpoint；hotplug、live update、诊断统计属于扩展路径，本文只在相关功能块中说明。
- `vblk_pci_dev` 当前读写后端仍是占位实现，主干关注 PCI 枚举、virtio-blk 请求收发、DMA 中转和请求完成。

关键源码入口：

| 应用 | 主入口 | 主流程 | PCI/TLP | VirtIO/数据面 |
| --- | --- | --- | --- | --- |
| `vnet_pci_dev` | `vnet_pci_dev.c` | `vnet_pci_dev_core.c` | `vnet_pci_device.c`, `pci_spec_tlp.h` | VNet offload engine, RX/TX/CVQ |
| `vblk_pci_dev` | `vblk_pci_dev.c` | `vblk_pci_dev_core.c` | `vblk_pci.c`, `vblk_tlp_ctx.c` | `vblk_ctrl.c`, `vblk_io_ctx.c`, `vblk_mpool.c` |

## 2. 共同抽象

两个程序都可以抽象为同一类 DevEmu/TLP 应用：

```text
CLI/日志
  -> 打开 DOCA device
  -> 创建 PCI TLP type 和 TLP channel
  -> 构造虚拟 PCI switch 拓扑：1 USP + N DSP + N endpoint
  -> 为每个 endpoint 创建 representor + TLP device
  -> 创建 VirtIO 设备模型和 offload engine
  -> Host 枚举期间处理 config/MMIO/PCI event TLP
  -> Host virtio driver 写 FEATURES_OK/DRIVER_OK 后启动 VQ/IO 数据面
  -> 主循环持续 doca_pe_progress()
  -> 退出时按反向顺序 stop/disable/destroy
```

共同的 DOCA API 功能块：

```c
/* 参数与日志 */
doca_log_backend_create_standard();
doca_log_backend_create_with_file_sdk();
doca_log_backend_set_sdk_level();
doca_argp_init();
doca_argp_param_create();
doca_argp_register_param();
doca_argp_start();

/* DOCA device 与 PE */
open_doca_device_with_pci() 或 open_doca_device_with_ibdev_name();
doca_pe_create();
doca_pe_connect_ctx();
doca_pe_progress();
doca_pe_destroy();
doca_dev_close();

/* ctx 生命周期 */
doca_ctx_set_user_data();
doca_ctx_start();
doca_ctx_stop();
doca_ctx_get_state();
doca_ctx_flush_tasks();

/* PCI DevEmu/TLP */
doca_devemu_pci_type_set_dev();
doca_devemu_pci_tlp_type_set_pci_cap_conf();
doca_devemu_pci_type_set_num_msix();
doca_devemu_pci_type_set_num_db();
doca_devemu_pci_type_start();
doca_devemu_pci_type_stop();
doca_devemu_pci_type_destroy();

doca_devemu_pci_tlp_channel_create();
doca_devemu_pci_tlp_channel_event_req_register();
doca_devemu_pci_tlp_channel_set_acg_enabled();
doca_devemu_pci_tlp_channel_as_ctx();
doca_devemu_pci_tlp_channel_req_complete_tlp();
doca_devemu_pci_tlp_channel_req_complete_pci_event();
doca_devemu_pci_tlp_channel_req_complete_acg();
doca_devemu_pci_tlp_channel_destroy();

/* Endpoint 资源 */
doca_devemu_pci_type_create_rep();
doca_devemu_pci_type_destroy_rep();
doca_devemu_pci_tlp_dev_create();
doca_devemu_pci_tlp_dev_start();
doca_devemu_pci_tlp_dev_stop();
doca_devemu_pci_tlp_dev_destroy();
doca_devemu_pci_tlp_dev_as_ep();
doca_devemu_pci_ep_create_msix();
doca_devemu_pci_msix_raise();
doca_devemu_pci_msix_destroy();
```

## 3. vnet_pci_dev 主干流程

`vnet_pci_dev` 在 DPU 侧暴露 VirtIO Net endpoint。它的主干特征是：endpoint 创建阶段只准备 PCI/TLP 设备和 VNet offload engine；RX/TX VQ、CVQ、IO context 和 engine enable 都由 Host virtio-net driver 的
`FEATURES_OK` / `DRIVER_OK` 状态写入异步触发。

### 3.1 线程模型

`vnet_pci_dev` 常规运行期有 2 个常驻线程：

| 线程 | 数量 | 创建位置 | 主要职责 |
| --- | ---: | --- | --- |
| main / TLP progress 线程 | 1 | 进程主线程 | 跑 `run_progress_loop()`，持续 `doca_pe_progress(g_tlp_ctx->pe)`；处理 Host PCI config/MMIO/PCI event TLP、ACG/MSI retry、stdin hotplug/speed 命令、pending unplug 超时和 LU 触发检查 |
| PCI config workqueue worker | 1 | `pci_cfg_workqueue_init()` 中 `pthread_create(pci_cfg_worker_thread)` | 处理 TLP callback 提交的重操作：engine start、VQ 初始化、IO context 初始化、start+enable、stats list、device reset、delayed destroy、hotplug、speed change；同时定时 progress 所有 controller 的 `worker_pe` |

需要注意：`num_ep` 会让每个 controller 各有一个 `worker_pe`，但这些 PE 都由同一个 workqueue worker 轮流 progress，不是每个 endpoint 一个 worker pthread。

还有两类按需临时线程：

- MQ 扩容时，CVQ 请求已经回复 Host 后，会创建 joinable MQ start thread 执行 `vnet_pci_dev_execute_mq_start_qps()`；`MQ_START_MAX_CONCURRENT=2` 只限制同时执行的数量，线程完成后由 workqueue join/reap。
- LU standby phase2 会按 endpoint 临时创建 enable thread，并在 phase2 结束前 join，最多 `num_ep` 个。

`doca_pe` 本身不算线程；callback 在调用 `doca_pe_progress()` 的线程上下文里执行。

### 3.2 代码级主线

```text
# 标注说明：
# [PCI设备模拟]：创建/维护 Host 可枚举的 PCI bridge、endpoint、配置空间、BAR、TLP channel、hotplug/MSI。
# [Doorbell/DMA通道]：创建 VNet/virtio offload engine、doorbell/VQ/CVQ/IO context，
#                    绑定 Host 写入的 virtqueue 地址并启用收发 Host 内存的 DMA 数据面。

vnet_pci_dev_run()
  -> init_tlp_context(&g_tlp_ctx, g_config.num_ep, g_config.hotplug_mode)
      -> calloc(tlp_context)
      -> pthread_mutex_init(acg_queue_lock)
      -> calloc(devs_config)             // 1 USP + N DSP + N EP + dummy, N = g_config.num_ep
      -> pthread_rwlock_init(endpoint_lock)
      -> calloc(bdf_entries)
      -> calloc(virtio_dev)
      -> calloc(vnet_controller)
      -> pthread_mutex_init(stats_ref_mutex)

  -> parse_mac_address(g_config.mac_addr, mac_bytes)
      -> 纯软件解析，无 DOCA API

  -> find_doca_device(g_config.pci_address, g_config.ibdev_name, &g_tlp_ctx->dev)
      -> open_doca_device_with_ibdev_name((const uint8_t *)g_config.ibdev_name,
                                          strlen(g_config.ibdev_name), NULL, &g_tlp_ctx->dev)
         // g_config.ibdev_name 非空时优先使用
      或 open_doca_device_with_pci(g_config.pci_address, NULL, &g_tlp_ctx->dev)
         // g_config.ibdev_name 为空时使用

  -> init_progress_engine(g_tlp_ctx)
      -> doca_pe_create(&g_tlp_ctx->pe)  // PE1: 主线程 TLP 快路径

  -> vnet_pci_device_configure_affinity(g_config.tlp_core_idx,
                                        g_config.worker_core_idx,
                                        g_config.mq_core_idx)

  // [PCI设备模拟] 创建 PCI DevEmu type、TLP channel、虚拟拓扑和 virtio-net 配置空间；
  // [Doorbell/DMA通道] 同一初始化阶段也创建 VNet 子系统和 endpoint 对应的 offload engine。
  -> init_virtio_network_device(mac_bytes)
      // [PCI设备模拟] 初始化 PCI type/workqueue 框架。
      -> vnet_pci_dev_init(g_tlp_ctx)
          // [PCI设备模拟] 声明 virtio-net PCI TLP type、MSI-X capability，并启动 PCI type。
          -> vnet_pci_device_init(g_tlp_ctx)
              -> doca_dev_as_devinfo(g_tlp_ctx->dev)
              -> doca_devemu_vnet_cap_is_pci_tlp_type_supported(devinfo, &supported)
              -> doca_devemu_vnet_pci_tlp_type_create("vnet_pci_dev", &g_tlp_ctx->pci_type)
              -> doca_devemu_pci_type_set_dev(g_tlp_ctx->pci_type, g_tlp_ctx->dev)
              -> doca_devemu_pci_type_set_num_msix(g_tlp_ctx->pci_type, VNET_PCI_DEV_NUM_MSIX)
              // [Doorbell/DMA通道] 向 PCI type 声明 doorbell register 数量，供 Host notify/VQ 通知使用。
              -> doca_devemu_pci_type_set_num_db(g_tlp_ctx->pci_type, VNET_PCI_DEV_NUM_DB)
              -> doca_devemu_pci_tlp_type_set_pci_cap_conf(g_tlp_ctx->pci_type,
                                                           PCI_CAP_ID_MSIX,
                                                           offsetof(struct pcie_virtio_dev, cfg.msix_cap),
                                                           sizeof(struct msix_capability))
              -> doca_devemu_pci_type_start(g_tlp_ctx->pci_type)
              -> doca_devemu_pci_type_create_bar_info_list(g_tlp_ctx->pci_type, &bar_list, &n)
              -> doca_devemu_pci_bar_info_get_bar_id(bar_list[i], &id)
              -> doca_devemu_pci_bar_info_get_log_sz(bar_list[i], &log_sz)
              -> doca_devemu_pci_bar_info_get_mem_type(bar_list[i], &mem_type)
              -> doca_devemu_pci_bar_info_get_prefetchable(bar_list[i], &is_prefetch)
              -> doca_devemu_pci_type_destroy_bar_info_list(bar_list)
              -> doca_devemu_pci_type_create_transaction_region_info_list(g_tlp_ctx->pci_type,
                                                                          &mmio_list, &n)
              -> doca_devemu_pci_transaction_region_info_get_bar_id(mmio_list[i], &bar_id)
              -> doca_devemu_pci_transaction_region_info_get_start_addr(mmio_list[i], &start_addr)
              -> doca_devemu_pci_transaction_region_info_get_size(mmio_list[i], &size)
              -> doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list)
              // [Doorbell/DMA通道] 查询 firmware 给 virtio-net 预定义的 doorbell region 布局。
              -> doca_devemu_pci_type_create_db_region_by_data_info_list(g_tlp_ctx->pci_type,
                                                                         &db_list, &n)
              -> doca_devemu_pci_db_region_by_data_info_get_bar_id(db_list[i], &bar_id)
              -> doca_devemu_pci_db_region_by_data_info_get_start_addr(db_list[i], &start_addr)
              -> doca_devemu_pci_db_region_by_data_info_get_size(db_list[i], &size)
              -> doca_devemu_pci_type_destroy_db_region_by_data_info_list(db_list)
              -> doca_devemu_pci_type_create_msix_table_region_info_list(g_tlp_ctx->pci_type,
                                                                         &msix_list, &n)
              -> doca_devemu_pci_msix_table_region_info_get_bar_id(msix_list[i], &bar_id)
              -> doca_devemu_pci_msix_table_region_info_get_start_addr(msix_list[i], &start_addr)
              -> doca_devemu_pci_msix_table_region_info_get_size(msix_list[i], &size)
              -> doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list)
              -> doca_devemu_pci_type_create_msix_pba_region_info_list(g_tlp_ctx->pci_type,
                                                                       &pba_list, &n)
              -> doca_devemu_pci_msix_pba_region_info_get_bar_id(pba_list[i], &bar_id)
              -> doca_devemu_pci_msix_pba_region_info_get_start_addr(pba_list[i], &start_addr)
              -> doca_devemu_pci_msix_pba_region_info_get_size(pba_list[i], &size)
              -> doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list)

      // [PCI设备模拟] 创建 TLP channel 并注册 Host config/MMIO/PCI event 请求回调。
      -> vnet_pci_dev_start(g_tlp_ctx)                 // 非 LU standby
          -> doca_devemu_pci_tlp_channel_create(g_tlp_ctx->dev, &g_tlp_ctx->tlp_channel)
          -> doca_devemu_pci_tlp_channel_set_req_user_data_size(g_tlp_ctx->tlp_channel,
                                                                VNET_TLP_REQ_USER_DATA_SIZE)
          -> doca_devemu_pci_tlp_channel_event_req_register(g_tlp_ctx->tlp_channel,
                                                            vnet_pci_dev_event_cb)
          -> if g_tlp_ctx->hotplug_mode:
               doca_devemu_pci_tlp_channel_set_acg_enabled(g_tlp_ctx->tlp_channel, 1)
      -> LU standby only: vnet_pci_dev_start_from_export(g_tlp_ctx, export_desc, export_desc_len,
                                                         VNET_LU_CH_SHM_DIR)
          -> doca_devemu_pci_tlp_channel_create_from_export(export_desc, export_desc_len,
                                                            VNET_LU_CH_SHM_DIR,
                                                            g_tlp_ctx->dev, &g_tlp_ctx->tlp_channel)
          -> doca_devemu_pci_tlp_channel_set_primary(g_tlp_ctx->tlp_channel, false)
          -> doca_devemu_pci_tlp_channel_set_req_user_data_size(g_tlp_ctx->tlp_channel,
                                                                VNET_TLP_REQ_USER_DATA_SIZE)
          -> doca_devemu_pci_tlp_channel_event_req_register(g_tlp_ctx->tlp_channel,
                                                            vnet_pci_dev_event_cb)
          -> if g_tlp_ctx->hotplug_mode:
               doca_devemu_pci_tlp_channel_set_acg_enabled(g_tlp_ctx->tlp_channel, true)

      // [PCI设备模拟] 将 TLP channel 接入 PE1，Host 枚举请求由主循环 progress。
      -> doca_devemu_pci_tlp_channel_as_ctx(g_tlp_ctx->tlp_channel)
      -> doca_pe_connect_ctx(g_tlp_ctx->pe, vnet_pci_dev_tlp_channel_ctx(g_tlp_ctx))
      -> doca_ctx_set_user_data(vnet_pci_dev_tlp_channel_ctx(g_tlp_ctx),
                                user_data.ptr = g_tlp_ctx)
      -> doca_ctx_start(vnet_pci_dev_tlp_channel_ctx(g_tlp_ctx))
      -> doca_devemu_pci_tlp_channel_get_num_dsp(g_tlp_ctx->tlp_channel,
                                                 &num_nv_switch_tlp_dsp)
      -> init_acg_queue(g_tlp_ctx)
          -> doca_dev_as_devinfo(g_tlp_ctx->dev)
          -> doca_devemu_pci_tlp_cap_get_max_acg(devinfo, &g_tlp_ctx->acg_queue_size)

      -> init_device_topology()
          -> 纯软件初始化 USP/DSP/EP 配置空间、capability、BDF map 初始状态

      // [Doorbell/DMA通道] 初始化 VNet 子系统，后续 offload engine/VQ/IO context 依赖它。
      -> vnet_pci_dev_vnet_controller_init()
          -> doca_devemu_vnet_add_dev(g_tlp_ctx->dev)
          -> doca_devemu_vnet_init()
          -> parse_mac_address(g_config.mac_addr, g_tlp_ctx->mac_bytes_base)
          -> g_tlp_ctx->max_queue_pairs = g_config.max_queue_pairs
          -> g_tlp_ctx->queue_size = g_config.queue_size
          -> g_tlp_ctx->mtu = g_config.mtu

      // [PCI设备模拟] 创建 virtio-net 软件设备模型：common cfg、device cfg、feature、queue shadow。
      -> vnet_pci_device_create(g_tlp_ctx, &attr)
          -> 为每个 EP 初始化 virtio-net 软件设备模型
          -> attr.num_queues = g_config.max_queue_pairs * 2 + 1
          -> attr.queue_size = g_config.queue_size
          -> attr.dev_cfg = &net_cfg                 // MAC=mac_bytes, mtu/speed/duplex 来自 g_config
          -> attr.device_features = VNET_PCI_DEV_DEFAULT_FEATURES
          -> attr.pci_cfg_change_cb = virtio_net_ctrl_change_cb
          -> attr.cb_arg = g_tlp_ctx

      // [PCI设备模拟] static mode 下为每个 EP 创建 Host 可枚举的 representor/TLP device。
      -> static mode: create_all_devices()
          -> for each EP:
              -> create_device(g_tlp_ctx,
                               &g_tlp_ctx->devs_config[FIRST_PF_IDX(g_tlp_ctx) + i],
                               ep_vhca_id = 0 或 LU 恢复值)
              -> 更新 DSP slot 为 Power ON / PDS=1 / DLActive=1

      -> init_transaction_region(g_tlp_ctx, TRANSACTION_REGION_SIZE)
          -> malloc() 每个 PF 的 transaction region backing memory

  // [Doorbell/DMA通道] 每个 controller 创建 PE2，承载 VQ/IO/stat 等数据面重操作。
  -> for each controller:
      -> doca_pe_create(&g_tlp_ctx->vnet_controller[i].worker_pe)  // PE2: VQ/IO/stat 等重操作

  -> pci_cfg_workqueue_set_worker_pes(worker_pes, g_config.num_ep)

  -> run_progress_loop()
      -> vnet_pci_device_pin_main_thread()
      -> pci_cfg_workqueue_set_diag_collection(g_tlp_ctx, true)
      -> while !force_quit:
          // [PCI设备模拟] Host 启动后通过 TLP channel 发起 config read/write、MMIO read/write、PCI event。
          //              vnet_pci_device.c 中的 TLP callback 维护 bridge/endpoint 配置空间、
          //              BAR、BDF map、virtio common config，并完成对应 TLP 请求。
          -> doca_pe_progress(g_tlp_ctx->pe)
              -> vnet_pci_dev_event_cb(channel, tlp_req, req_user_data)
                  // channel/tlp_req 由 DOCA 回调传入；g_tlp_ctx 从 channel ctx user_data 取回
                  -> doca_devemu_pci_tlp_channel_as_ctx(channel)
                  -> doca_ctx_get_user_data(doca_devemu_pci_tlp_channel_as_ctx(channel),
                                            &channel_user_data)
                  -> doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req)
                  -> doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req)
                  -> doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req)
                  -> doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req)
                  -> doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req)
                  -> if opcode == ACG and queue full:
                       doca_devemu_pci_tlp_channel_req_complete_acg(
                         tlp_req, 0, DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH)
                  // [PCI设备模拟] 完成 Host 枚举/配置空间访问和 PCI event。
                  -> doca_devemu_pci_tlp_channel_req_complete_config_read(
                         tlp_req, dev_cfg->tlp_dev, is_cap_id_valid, cap_id, is_pcie_cap)
                  -> doca_devemu_pci_tlp_channel_req_complete_config_write(
                         tlp_req, dev_cfg->tlp_dev, is_cap_id_valid, cap_id, is_pcie_cap)
                  -> doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, is_non_posted,
                                                                  dev_cfg->tlp_dev 或 NULL)
                  -> doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req)
          // [PCI设备模拟] hotplug MSI 通过 ACG credit 构造 Memory Write TLP 通知 Host。
          -> drain_msi_retries()
              -> send_msi_via_memory_write_tlp(g_tlp_ctx, dsp)
                  -> acg_queue_pop(g_tlp_ctx)          // 返回缓存的 acg_req
                  -> doca_devemu_pci_tlp_channel_req_get_acg_buf(acg_req)
                  -> doca_devemu_pci_tlp_channel_req_complete_acg(
                         acg_req, tlp_size,
                         DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_MMIO_WRITE)
          -> stdin hotplug/speed command
              -> pci_cfg_workqueue_submit_hotplug(g_tlp_ctx, cmd.ep_index, plug)
                 -> create_device(g_tlp_ctx, endpoint, 0) 或延迟 vnet_pci_dev_destroy_device()
                 -> send_msi_via_memory_write_tlp(g_tlp_ctx, dsp)
              或 pci_cfg_workqueue_submit_speed_change(&g_tlp_ctx->vnet_controller[cmd.ep_index],
                                                       cmd.speed)
                 // [PCI设备模拟] 使用 Host 分配的 MSI-X vector 发送配置变更中断。
                 -> doca_devemu_pci_tlp_dev_as_ep(endpoint->tlp_dev)
                 -> if vector changed:
                      doca_devemu_pci_msix_destroy(controller->config_msix)
                 -> doca_devemu_pci_ep_create_msix(pci_ep, VNET_VIRTIO_BAR_ID,
                                                   VNET_VIRTIO_MSIX_TABLE_OFFSET,
                                                   live_vector, &controller->config_msix)
                 -> doca_devemu_pci_msix_raise(controller->config_msix)
      -> pci_cfg_workqueue_set_diag_collection(g_tlp_ctx, false)

  -> cleanup_virtio_network_device()
      -> pci_cfg_workqueue_shutdown()
      -> for each virtio_dev:
          -> vnet_pci_device_destroy(&g_tlp_ctx->virtio_dev[i])
      -> for each EP:
          -> vnet_pci_dev_destroy_device(g_tlp_ctx, ep)       // 重复调用，见下方独立展开
      // [Doorbell/DMA通道] 关闭 VNet 子系统和 offload engine 相关资源。
      -> vnet_pci_dev_vnet_controller_uninit()
          -> doca_devemu_vnet_teardown()
          -> doca_devemu_vnet_rm_dev(g_tlp_ctx->dev)
      // [PCI设备模拟] flush ACG、停止 TLP channel、销毁 PCI type。
      -> flush ACG credits
          -> doca_devemu_pci_tlp_channel_req_complete_acg(
                 acg_req, 0, DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH)
      -> doca_ctx_stop(vnet_pci_dev_tlp_channel_ctx(g_tlp_ctx))
      -> while ctx not IDLE:
          -> doca_pe_progress(g_tlp_ctx->pe)
          -> doca_ctx_get_state(vnet_pci_dev_tlp_channel_ctx(g_tlp_ctx), &ctx_state)
      -> vnet_pci_dev_stop(g_tlp_ctx)
          -> doca_devemu_pci_tlp_channel_destroy(g_tlp_ctx->tlp_channel)
      -> vnet_pci_dev_reset(g_tlp_ctx)
          -> doca_devemu_pci_type_stop(g_tlp_ctx->pci_type)
          -> doca_devemu_pci_type_destroy(g_tlp_ctx->pci_type)

  -> pci_cfg_workqueue_clear_worker_pes()
  -> for each controller:
      -> doca_pe_destroy(g_tlp_ctx->vnet_controller[i].worker_pe)
  -> cleanup_progress_engine(g_tlp_ctx)
      -> doca_pe_destroy(g_tlp_ctx->pe)
  -> tlp_ctx_cleanup(g_tlp_ctx)
      -> doca_dev_close(g_tlp_ctx->dev)
      -> free software objects
```

重复调用的 endpoint/controller 函数单独展开如下，避免在每个 EP 下重复列出：

```text
create_device(tlp_ctx == g_tlp_ctx, endpoint)
  // [PCI设备模拟] 为一个 endpoint 创建 Host 侧可见的 representor + TLP device。
  -> doca_devemu_pci_type_create_rep(g_tlp_ctx->pci_type, &endpoint->rep)
     或 LU standby: vnet_lu_find_existing_rep(g_tlp_ctx->pci_type, lu_ep_vhca_id, &endpoint->rep)
  -> doca_dev_rep_as_devinfo(endpoint->rep)
  -> doca_devinfo_rep_get_vhca_id(devinfo_rep, &vhca_id)
  -> doca_devemu_pci_tlp_dev_create(g_tlp_ctx->pci_type, endpoint->rep, &endpoint->tlp_dev)
  -> doca_devemu_pci_tlp_dev_start(endpoint->tlp_dev)
  // [Doorbell/DMA通道] endpoint 存在后，为该 PCI EP 创建 VNet offload engine。
  -> vnet_pci_dev_vnet_controller_create(g_tlp_ctx, endpoint)

vnet_pci_dev_vnet_controller_create(tlp_ctx == g_tlp_ctx, endpoint)
  -> get_pf_index_for_device(g_tlp_ctx, endpoint, &pf_index)  // pf_index 决定 controller[i]
  -> controller = &g_tlp_ctx->vnet_controller[pf_index]
  // [Doorbell/DMA通道] 以 PCI endpoint 为锚点创建/恢复 offload engine。
  -> doca_devemu_pci_tlp_dev_as_ep(endpoint->tlp_dev)
  -> doca_devemu_vnet_offload_engine_create(pci_ep, &controller->offload_engine)
     或 LU standby: doca_devemu_vnet_offload_engine_create_from_export(
           import_desc, import_desc_len, VNET_LU_OE_SHM_DIR, pci_ep, &controller->offload_engine)
  -> doca_devemu_vnet_offload_engine_as_virtio_offload(controller->offload_engine)
  -> doca_devemu_virtio_offload_engine_set_shm_dir_path(virtio_engine, VNET_LU_OE_SHM_DIR)
  -> doca_devemu_vnet_offload_engine_set_mtu(controller->offload_engine,
                                             g_tlp_ctx->mtu /* 来自 g_config.mtu */)
  -> doca_devemu_vnet_offload_engine_set_mac(controller->offload_engine,
                                             mac_bytes = g_tlp_ctx->mac_bytes_base, mac_bytes[5] += pf_index)
  -> doca_devemu_virtio_offload_engine_set_num_queues(
         virtio_engine, total_vqs = g_tlp_ctx->max_queue_pairs * 2 + 1)
  -> doca_devemu_vnet_offload_engine_get_rep(controller->offload_engine, &rep)
  -> doca_dev_rep_as_devinfo(rep)
  -> doca_devinfo_rep_get_vhca_id(devinfo_rep, &vhca_id)
  -> doca_devinfo_rep_get_iface_index(devinfo_rep, &ifindex)
  -> doca_devinfo_rep_get_pci_func_type(devinfo_rep, &pci_func_type)
  -> doca_devinfo_rep_get_sf_index(devinfo_rep, &sf_index)     // 仅 SF 类型
  -> calloc(rx_vqs)
  -> calloc(tx_vqs)

Host 写 FEATURES_OK 后的异步硬件准备路径：
  // [Doorbell/DMA通道] virtio_net_ctrl_change_cb() 处理 VirtIO 状态变化。
  //                  Host 写 FEATURES_OK 后，feature 已协商完成；这里开始准备 engine、
  //                  RX/TX VQ、CVQ 和 IO context，但不阻塞 TLP 主线程。
  -> virtio_net_ctrl_change_cb(dev, prev_status, pci_cfg->device_status)
      -> ctrl = &g_tlp_ctx->vnet_controller[dev->pf_index]
      -> ctrl->max_queue_pairs = g_tlp_ctx->max_queue_pairs 或 1
         // 取决于 dev->driver_features 是否同时协商 VIRTIO_NET_F_CTRL_VQ 和 VIRTIO_NET_F_MQ
      -> pci_cfg_workqueue_submit_engine_start(ctrl)
          -> doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine)
          -> doca_devemu_virtio_offload_engine_start(virtio_engine)
      -> pci_cfg_workqueue_submit_initialize_vqs(ctrl)
          -> for i in [0, ctrl->max_queue_pairs):
              -> doca_devemu_vnet_rx_vq_create(ctrl->offload_engine, &ctrl->rx_vqs[i])
              -> doca_devemu_vnet_tx_vq_create(ctrl->offload_engine, &ctrl->tx_vqs[i])
          -> if ctrl->mq_feature_negotiated:
              -> doca_devemu_vnet_ctrl_vq_create(ctrl->offload_engine, &ctrl->cvq)
      -> pci_cfg_workqueue_submit_initialize_io_context(ctrl)
          -> doca_devemu_vnet_io_create_from_offload_engine(ctrl->offload_engine, &ctrl->io_ctx)
          -> doca_devemu_vnet_io_as_virtio_io(ctrl->io_ctx)
          -> doca_devemu_virtio_io_as_ctx(virtio_io)
          -> doca_pe_connect_ctx(ctrl->worker_pe, io_ctx)
          -> doca_devemu_vnet_io_event_vnet_ctrl_req_register(ctrl->io_ctx,
                                                              vnet_pci_dev_ctrl_req_handler)
          -> doca_ctx_start(io_ctx)

Host 写 DRIVER_OK 或后续 VQ 配置更新后的异步启用路径：
  // [Doorbell/DMA通道] Host 写 DRIVER_OK 或完成后续 VQ 配置更新后，
  //                  由 workqueue 启动 VQ 并 enable engine。Host 写入
  //                  queue_desc/queue_driver/queue_device 后，
  //                  通过 set_conf/start/enable 让 offload engine 读写 Host 内存。
  -> pci_cfg_workqueue_submit_start_and_enable(ctrl, dev)
      -> vqs = vnet_pci_device_get_virtq_pci_cfg(dev)
         // vqs[*].queue_size/msix_vector/desc/driver/device 全部来自 Host 对 virtio common cfg 的 TLP 写
      -> for active RX/TX VQ:
          -> doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i])
          -> doca_devemu_virtio_vq_set_conf(rx_vq, rx_idx, vqs[rx_idx].queue_size,
                                            vqs[rx_idx].queue_msix_vector,
                                            vqs[rx_idx].queue_desc,
                                            vqs[rx_idx].queue_driver,
                                            vqs[rx_idx].queue_device)
          -> doca_devemu_virtio_vq_start(rx_vq)
          -> doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i])
          -> doca_devemu_virtio_vq_set_conf(tx_vq, tx_idx, vqs[tx_idx].queue_size,
                                            vqs[tx_idx].queue_msix_vector,
                                            vqs[tx_idx].queue_desc,
                                            vqs[tx_idx].queue_driver,
                                            vqs[tx_idx].queue_device)
          -> doca_devemu_virtio_vq_start(tx_vq)
      -> if CVQ ready:
          -> doca_devemu_vnet_ctrl_vq_as_vq(ctrl->cvq)
          -> doca_devemu_vnet_io_as_virtio_io(ctrl->io_ctx)
          -> doca_devemu_virtio_vq_set_conf(cvq_vq, cvq_index, vqs[cvq_index].queue_size,
                                            vqs[cvq_index].queue_msix_vector,
                                            vqs[cvq_index].queue_desc,
                                            vqs[cvq_index].queue_driver,
                                            vqs[cvq_index].queue_device)
          -> doca_devemu_virtio_vq_start(cvq_vq)
          -> doca_devemu_virtio_io_bind_vq(virtio_io, cvq_vq, ctrl)
      -> doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine)
      -> doca_devemu_virtio_offload_engine_enable(virtio_engine)
      -> doca_devemu_vnet_counters_create(ctrl->offload_engine, &ctrl->vnet_counters)
      -> doca_devemu_vnet_counters_reset(ctrl->vnet_counters)
      -> doca_devemu_virtio_offload_engine_queue_dbg_state_create_list(virtio_engine,
                                                                       &state_list, &list_len)
      -> diagnostics/stat collection on worker PE:
          -> doca_devemu_virtio_queue_dbg_state_is_populated(state_list, &is_populated)
          -> doca_devemu_virtio_queue_dbg_state_populate_list(state_list)
          -> doca_devemu_virtio_queue_dbg_state_get_id(stats, &id)
          -> doca_devemu_virtio_queue_dbg_state_get_enabled(stats, &enabled)
          -> doca_devemu_virtio_queue_dbg_state_get_size(stats, &size)
          -> doca_devemu_virtio_queue_dbg_state_get_inflights(stats, &inflights)
          -> doca_devemu_virtio_queue_dbg_state_get_hw_avail_idx(stats, &hw_avail_idx)
          -> doca_devemu_virtio_queue_dbg_state_get_driver_avail_idx(stats, &driver_avail_idx)
          -> doca_devemu_virtio_queue_dbg_state_get_hw_used_idx(stats, &hw_used_idx)
          -> doca_devemu_virtio_queue_dbg_state_get_driver_used_idx(stats, &driver_used_idx)
          -> doca_devemu_vnet_counters_populate_sync(ctrl->vnet_counters)
          -> doca_devemu_vnet_counters_rx_vq_query(ctrl->vnet_counters, vq_index, &rx_cnt)
          -> doca_devemu_vnet_counters_tx_vq_query(ctrl->vnet_counters, vq_index, &tx_cnt)

CVQ/MQ 控制请求路径：
  // [Doorbell/DMA通道] CVQ 绑定到 IO context 后，Host 的 virtio-net 控制请求在这里完成。
  -> vnet_pci_dev_ctrl_req_handler(req, cls, cmd, user_data)
      -> doca_devemu_vnet_ctrl_req_get_vq_user_data(req)       // bind_vq 时传入的 ctrl
      -> if cls == VIRTIO_NET_CTRL_MQ && cmd == VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET:
          -> doca_devemu_vnet_ctrl_req_get_data_len(req)
          -> doca_devemu_vnet_ctrl_req_get_data(req)
          -> doca_buf_get_data(data_buf, &data_ptr)            // data_ptr 指向 Host CVQ 命令体
          -> doca_devemu_vnet_ctrl_req_complete(req, ack, sizeof(uint8_t))
          -> if new_qps > old_qps:
              -> doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine)
              -> doca_devemu_virtio_vq_set_conf(rx_vq/tx_vq, vq_idx, vqs[vq_idx].*)
              -> doca_devemu_virtio_vq_start(rx_vq/tx_vq)
              -> doca_devemu_virtio_vq_group_enable(virtio_engine,
                                                    start_vq_idx = old_qps * 2,
                                                    end_vq_idx = new_qps * 2 - 1)
          -> if new_qps < old_qps:
              -> doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine)
              -> doca_devemu_virtio_vq_group_disable(virtio_engine,
                                                     start_vq_idx = new_qps * 2,
                                                     end_vq_idx = old_qps * 2 - 1)
              -> doca_devemu_virtio_vq_stop(rx_vq/tx_vq)
      -> else:
          -> doca_devemu_vnet_ctrl_req_complete(req, ack, sizeof(uint8_t))

vnet_pci_dev_destroy_device(tlp_ctx == g_tlp_ctx, endpoint)
  // [Doorbell/DMA通道] 先停数据面：VQ/IO/counters/offload engine。
  -> vnet_pci_dev_vnet_controller_destroy(g_tlp_ctx, endpoint)
      -> vnet_pci_dev_shutdown_device()
          -> vnet_controller_cleanup(destroy_engine=true)
              -> doca_pe_progress(ctrl->worker_pe)             // drain stats/counters in progress
              -> doca_devemu_vnet_counters_destroy(ctrl->vnet_counters)
              -> doca_devemu_virtio_queue_dbg_state_destroy_list(local_ref->state_list)
              -> doca_devemu_pci_msix_destroy(ctrl->config_msix)
              -> doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine)
              -> doca_devemu_vnet_rx_vq_as_vq(ctrl->rx_vqs[i])
              -> doca_devemu_vnet_tx_vq_as_vq(ctrl->tx_vqs[i])
              -> doca_devemu_vnet_ctrl_vq_as_vq(ctrl->cvq)
              -> doca_devemu_virtio_vq_disable(vq)
              -> doca_devemu_virtio_offload_engine_disable(virtio_engine)
              -> doca_devemu_vnet_io_as_virtio_io(ctrl->io_ctx)
              -> doca_devemu_virtio_io_flush_vq(virtio_io, cvq_vq)
              -> doca_devemu_virtio_io_unbind_vq(virtio_io, cvq_vq)
              -> doca_devemu_virtio_vq_stop(vq)
              -> doca_devemu_virtio_io_as_ctx(virtio_io)
              -> doca_ctx_flush_tasks(io_ctx)
              -> doca_ctx_stop(io_ctx)
              -> while io_ctx not IDLE:
                  -> doca_pe_progress(ctrl->worker_pe)
                  -> doca_ctx_get_state(io_ctx, &ctx_state)
              -> doca_devemu_vnet_io_destroy(saved_io_ctx)
              -> doca_devemu_vnet_rx_vq_destroy(ctrl->rx_vqs[i])
              -> doca_devemu_vnet_tx_vq_destroy(ctrl->tx_vqs[i])
              -> doca_devemu_vnet_ctrl_vq_destroy(ctrl->cvq)
              -> doca_devemu_virtio_offload_engine_stop(virtio_engine)
              -> doca_devemu_vnet_offload_engine_destroy(ctrl->offload_engine)
  // [PCI设备模拟] 再停 endpoint：TLP device 和 representor。
  -> doca_devemu_pci_tlp_dev_stop(endpoint->tlp_dev)
  -> doca_devemu_pci_tlp_dev_destroy(endpoint->tlp_dev)
  -> doca_devemu_pci_type_destroy_rep(endpoint->rep)
     或 LU standby: doca_dev_rep_close(endpoint->rep)
```

## 4. vblk_pci_dev 主干流程

`vblk_pci_dev` 在 DPU 侧暴露 VirtIO Block endpoint。与 `vnet` 相比，它更显式地拆分了线程和 VQ 状态机：

- TLP 线程：处理 Host PCI config/MMIO/PCI event TLP，并读 stdin 命令。
- IO context 线程：每个 IO core 一个线程，推进 VBlk IO context 和 DMA context。
- offload engine 线程：由其中一个 IO 线程兼任，负责创建 controller、启动/销毁 VQ、enable offload engine、hotplug 状态推进。

线程总数由 `io_ctx_mask` 决定：

```text
num_io_ctx = popcount(io_ctx_mask)
常规运行期总线程数 = 1 个 main 线程 + 1 个 TLP 线程 + num_io_ctx 个 IO 线程
```

默认配置 `io_ctx_mask = 0x00FF`，因此 `num_io_ctx = 8`，默认运行期总线程数是 10 个。`offload_engine_core_idx` 必须落在 `io_ctx_mask` 中，所以 OE 角色由这 8 个 IO 线程中的 1 个兼任，不额外增加线程。

| 线程 | 数量 | 创建位置 | 主要职责 |
| --- | ---: | --- | --- |
| main 线程 | 1 | 进程主线程 | 初始化日志/参数/信号、VBlk library、PE、mpool、PCI/TLP 资源；创建 TLP/IO 线程；等待 `IO_READY` 和 `CTRL_ENABLED`；退出时 join 工作线程并清理资源 |
| TLP 线程 | 1 | `vblk_pci_tlp_thread_create()` | 连接并 start TLP channel ctx，循环 `doca_pe_progress(tlp_pe)` 和 `vblk_pci_tlp_poll()`；处理 Host PCI config/MMIO/PCI event TLP；读取 stdin 的 `cap`、`plug`、`unplug` 命令；退出时 stop PCI event channel |
| IO context 线程 | `num_io_ctx` | `vblk_io_ctx_oe_threads_create()` 为每个 io ctx 调 `vblk_io_ctx_thread_create()` | 为每个 EP 创建本线程负责的 VBlk IO context、DMA context、共享 mpool 视角；运行时 progress DMA PE 和 IO PE，处理 VBlk request/DMA completion，推进 VQ bind/enable/unbind；退出时关闭本线程的 IO/DMA context |
| OE + IO 线程 | 1，包含在 `num_io_ctx` 内 | `affinity_core == offload_engine_core_idx` 的 IO 线程 | 除普通 IO 职责外，static mode 下创建所有 endpoint controller/offload engine 并 enable；hotplug mode 下处理 plug/unplug、创建/销毁 controller、发送或重试 hotplug MSI；运行时推进 VQ create/start/destroy 状态机 |

### 4.1 代码级主线

```text
main()
  -> 初始化日志
  -> doca_argp 解析 emulation-manager、num_ep、num_queues、io_ctx_mask、provider 等参数
  -> 注册 SIGINT/SIGTERM
  -> vblk_pci_dev_run()

vblk_pci_dev_run()
  -> 校验配置，解析 io_ctx_mask 为 io_ctx_cores
  -> 分配 app resources、IO thread config
  -> doca_devemu_vblk_set_datapath_on_dpa()
  -> vblk_init()
  -> progress_contexts_init()
  -> shared_mpools_create()
  -> vblk_pci_init()
  -> progress_contexts_start()
  -> vblk_pci_reset(), vblk_reset(), destroy mpool/PE/device

vblk_init()
  -> open_doca_device_with_ibdev_name()
  -> doca_devemu_vblk_add_dev()
  -> doca_devemu_vblk_set_vblk_req_user_data_size()
  -> capability 检查与全局参数设置
  -> doca_devemu_vblk_init()

vblk_pci_init()
  -> doca_devemu_pci_tlp_cap_get_max_types()
  -> vblk_pci_switch_init()
  -> doca_devemu_pci_tlp_channel_create()
  -> doca_devemu_pci_tlp_channel_event_req_register()
  -> hotplug 时启用 ACG
  -> doca_devemu_vblk_pci_tlp_type_create()
  -> doca_devemu_pci_type_set_dev()
  -> doca_devemu_pci_tlp_type_set_pci_cap_conf()
  -> doca_devemu_pci_type_set_num_msix()
  -> doca_devemu_pci_type_set_num_db()
  -> doca_devemu_pci_type_start()
```

### 4.2 线程启动与运行

```text
progress_contexts_start()
  -> vblk_pci_tlp_thread_create()
       -> doca_pe_connect_ctx(tlp_pe, tlp_channel_ctx)
       -> doca_ctx_start(tlp_channel_ctx)
       -> pthread_create(TLP thread)

  -> vblk_io_ctx_oe_threads_create()
       -> 为每个 io_ctx 创建一个线程
       -> affinity_core == offload_engine_core_idx 的线程兼任 offload engine

  -> 等待 IO context ready
  -> 等待 controller enable
  -> join TLP/IO 线程直到退出

TLP thread loop
  -> doca_pe_progress(tlp_pe)
  -> vblk_pci_tlp_poll()
  -> stdin: cap/plug/unplug

IO/OE thread loop
  -> hotplug mode 下 OE 线程处理 plug/unplug 请求
  -> 按需创建或关闭 IO context
  -> 推进 DMA PE
  -> OE 线程推进 VQ create/start/destroy 状态机
  -> IO 线程推进 VQ bind/enable/unbind 状态机
  -> doca_pe_progress(io_pe)
```

### 4.3 按功能块重组的 vblk DOCA API 伪代码

```c
int vblk_main_flow(config)
{
    /*
     * 功能块 A：日志、参数、信号
     */
    doca_log_backend_create_standard();
    doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    doca_argp_init("doca_vblk_pci_dev", &config);
    register_vblk_args();
    doca_argp_start(argc, argv);
    install_signal_handlers();

    /*
     * 功能块 B：VBlk library 初始化
     */
    doca_devemu_vblk_set_datapath_on_dpa(config.datapath_on_dpa);
    open_doca_device_with_ibdev_name(config.device_name, &doca_dev);
    doca_devemu_vblk_add_dev(doca_dev);
    doca_devemu_vblk_set_vblk_req_user_data_size(sizeof(struct vblk_io_request));
    doca_devemu_vblk_set_seg_max(max_seg_max);
    doca_devemu_vblk_set_max_queue_size(max_queue_size);
    doca_devemu_vblk_init();

    /*
     * 功能块 C：PE 与 DPU 中转 buffer 池
     */
    for each io_ctx:
        doca_pe_create(&io_pe[ctx_id]);
    doca_pe_create(&tlp_pe);

    for each io_ctx:
        /* vblk_mpool: hugepage memory + mmap + buf_pool */
        doca_mmap_create(&mmap);
        doca_mmap_set_max_num_devices(mmap, 1);
        doca_mmap_set_memrange(mmap, hugepage_mem, size);
        doca_mmap_set_permissions(mmap, LOCAL_RW | RDMA_READ | RDMA_WRITE | PCI_RELAXED_ORDERING);
        doca_mmap_add_dev(mmap, doca_dev);
        doca_mmap_start(mmap);
        doca_buf_pool_create(num_bufs, buf_size, mmap, &buf_pool);
        doca_buf_pool_start(buf_pool);

    /*
     * 功能块 D：PCI TLP type、TLP channel 和 virtio-blk type
     */
    doca_devemu_pci_tlp_cap_get_max_types(doca_dev_as_devinfo(doca_dev), &n);
    init_software_pci_switch(1 USP, N DSP, N EP);

    doca_devemu_pci_tlp_channel_create(doca_dev, &tlp_channel);
    doca_devemu_pci_tlp_channel_event_req_register(tlp_channel, vblk_pci_ev_cb);
    if (hotplug)
        doca_devemu_pci_tlp_channel_set_acg_enabled(tlp_channel, true);

    doca_devemu_vblk_pci_tlp_type_create("vblk_pci_vblk", &pci_type);
    doca_devemu_pci_type_set_dev(pci_type, doca_dev);
    doca_devemu_pci_tlp_type_set_pci_cap_conf(pci_type, EXP, ...);
    doca_devemu_pci_tlp_type_set_pci_cap_conf(pci_type, MSIX, ...);
    doca_devemu_pci_type_set_num_msix(pci_type, VBLK_PCI_VIRTIO_MAX_NUM_MSIX);
    doca_devemu_pci_type_set_num_db(pci_type, VBLK_PCI_VIRTIO_MAX_NUM_DB);
    doca_devemu_pci_type_start(pci_type);

    /*
     * 功能块 E：启动 TLP 线程
     */
    doca_pe_connect_ctx(tlp_pe, doca_devemu_pci_tlp_channel_as_ctx(tlp_channel));
    doca_ctx_start(doca_devemu_pci_tlp_channel_as_ctx(tlp_channel));
    pthread_create(tlp_thread);

    /*
     * 功能块 F：static mode 下 OE 线程创建 endpoint controller
     */
    for each ep on OE thread:
        doca_devemu_pci_type_create_rep(pci_type, &ep->rep);
        doca_devemu_pci_tlp_dev_create(pci_type, ep->rep, &ep->tlp_dev);
        doca_devemu_pci_tlp_dev_start(ep->tlp_dev);

        vblk_pci_virtio_dev_create({
            .num_queues = config.num_queues,
            .device_type = VBLK,
            .device_features = MQ | SEG_MAX | SIZE_MAX | VERSION_1 | ACCESS_PLATFORM,
            .dev_cfg = {capacity, num_queues, seg_max, size_max},
            .pci_cfg_change_cb = virtio_ctrl_change_cb,
        });

        pci_ep = doca_devemu_pci_tlp_dev_as_ep(ep->tlp_dev);
        doca_devemu_vblk_offload_engine_create(pci_ep, &ctrl->vq_engine);
        doca_devemu_vblk_offload_engine_set_seg_max(ctrl->vq_engine, config.seg_max);
        virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
        doca_devemu_virtio_offload_engine_set_indir_descs_enabled(virtio_engine, config.indirect);
        doca_devemu_virtio_offload_engine_set_num_queues(virtio_engine, config.num_queues);
        doca_devemu_virtio_offload_engine_start(virtio_engine);

    /*
     * 功能块 G：每个 IO 线程创建 VBlk IO context 和 DMA context
     */
    for each ep, each io_ctx thread:
        doca_devemu_vblk_io_create_from_offload_engine(ctrl->vq_engine, &vblk_io);
        doca_ctx_set_user_data(vblk_io_as_ctx(vblk_io), io_ctx);
        doca_devemu_vblk_io_event_vblk_req_register(vblk_io, vblk_io_handler);

        doca_dma_create(doca_dev, &dma);
        doca_ctx_set_user_data(doca_dma_as_ctx(dma), io_ctx);
        doca_dma_task_memcpy_set_conf(dma, dma_done_cb, dma_error_cb, pool_size);
        doca_dma_set_ordered_completions(dma, false);

        doca_pe_connect_ctx(io_pe, vblk_io_as_ctx(vblk_io));
        doca_pe_create(&dma_pe);
        doca_pe_connect_ctx(dma_pe, doca_dma_as_ctx(dma));
        doca_ctx_start(doca_dma_as_ctx(dma));
        doca_ctx_start(vblk_io_as_ctx(vblk_io));

    /*
     * 功能块 H：所有 IO context 就绪后 enable offload engine
     */
    wait_all_io_ctx_ready();
    for each ep:
        virtio_engine = doca_devemu_vblk_offload_engine_as_virtio_offload(ctrl->vq_engine);
        doca_devemu_virtio_offload_engine_enable(virtio_engine);

    /*
     * 功能块 I：Host DRIVER_OK 后创建、配置、启动、绑定 VQ
     */
    on_virtio_driver_ok():
        cache_all_vq_cfg_from_pci_common_cfg();
        for each enabled_queue:
            state[qid] = STARTING;

    on_oe_progress():
        if state[qid] == STARTING:
            doca_devemu_vblk_req_vq_create(ctrl->vq_engine, &req_vq);
            virtio_vq = doca_devemu_vblk_req_vq_as_vq(req_vq);
            doca_devemu_virtio_vq_set_conf(
                virtio_vq, qid, queue_size, msix_vector,
                queue_desc, queue_driver, queue_device);
            doca_devemu_virtio_vq_start(virtio_vq);
            state[qid] = BINDING;

    on_io_progress():
        if state[qid] == BINDING:
            virtio_io = doca_devemu_vblk_io_as_virtio_io(vblk_io);
            doca_devemu_virtio_io_bind_vq(virtio_io, virtio_vq, &vq_user_data);
            doca_devemu_virtio_vq_enable(virtio_vq);
            state[qid] = RUNNING;

    /*
     * 功能块 J：virtio-blk 请求处理与 DMA 中转
     */
    on_vblk_req(req, type, sector):
        if type == VBLK_T_GET_ID:
            doca_buf_pool_buf_alloc(mpool, &dpu_buf);
            fill_dpu_buf_with_id("vblk_bdev0");
            doca_dma_task_memcpy_alloc_init(dma, dpu_buf, host_req_buf, req_udata, &task);
            doca_task_submit(doca_dma_task_memcpy_as_task(task));

        else if type == VBLK_T_IN:
            doca_buf_pool_buf_alloc(mpool, &dpu_buf);
            fill_or_prepare_placeholder_read_data(dpu_buf);
            doca_dma_task_memcpy_alloc_init(dma, dpu_buf, host_req_buf, req_udata, &task);
            doca_task_submit(doca_dma_task_memcpy_as_task(task));

        else if type == VBLK_T_OUT:
            doca_buf_pool_buf_alloc(mpool, &dpu_buf);
            doca_dma_task_memcpy_alloc_init(dma, host_req_buf, dpu_buf, req_udata, &task);
            doca_task_submit(doca_dma_task_memcpy_as_task(task));

        else:
            doca_devemu_vblk_req_complete(req, VBLK_S_UNSUPP, 0);

    on_dma_done(task, req):
        doca_task_free(task);
        doca_buf_dec_refcount(req->dpu_buf);
        if req is read/get_id:
            doca_devemu_vblk_req_complete(req->doca_req, VBLK_S_OK, out_len);
        else:
            doca_devemu_vblk_req_complete(req->doca_req, VBLK_S_OK, 0);

    /*
     * 功能块 K：清理
     */
    force_quit = true;
    for each IO thread:
        for each owned VQ:
            doca_devemu_virtio_vq_disable();
            doca_devemu_virtio_io_flush_vq();
            doca_devemu_virtio_io_unbind_vq();
        doca_ctx_stop(doca_dma_as_ctx(dma));
        doca_ctx_stop(vblk_io_as_ctx(vblk_io));

    TLP thread:
        doca_ctx_stop(tlp_channel_ctx);

    OE thread:
        doca_devemu_virtio_vq_stop(all_vqs);
        doca_devemu_vblk_req_vq_destroy(all_vqs);
        doca_devemu_virtio_offload_engine_disable(virtio_engine);
        doca_devemu_virtio_offload_engine_stop(virtio_engine);
        doca_devemu_vblk_offload_engine_destroy(ctrl->vq_engine);

    doca_devemu_pci_tlp_dev_stop();
    doca_devemu_pci_tlp_dev_destroy();
    doca_devemu_pci_type_destroy_rep();
    doca_devemu_pci_tlp_channel_destroy();
    doca_devemu_pci_type_stop();
    doca_devemu_pci_type_destroy();
    doca_devemu_vblk_teardown();
    doca_mmap_stop();
    doca_buf_pool_stop();
    doca_pe_destroy();
    doca_dev_close();
}
```

## 5. 关键差异

| 维度 | `vnet_pci_dev` | `vblk_pci_dev` |
| --- | --- | --- |
| 设备类型 | VirtIO Net | VirtIO Block |
| 数据面模型 | VNet offload engine 负责网络包队列 | VBlk request VQ + IO context + DMA task |
| 主线程模型 | 2 个常驻线程：main/TLP progress + PCI config workqueue worker；MQ/LU 有按需临时线程 | `1 + 1 + popcount(io_ctx_mask)`：main + TLP + 多个 IO 线程，其中一个 IO 线程兼任 OE |
| VQ 创建时机 | Host `FEATURES_OK` 后异步创建 RX/TX/CVQ | Host `DRIVER_OK` 后 OE 线程按 VQ 状态机创建 req_vq |
| VQ enable 路径 | `set_conf -> start -> offload_engine_enable`，CVQ 还要 bind IO | `create -> set_conf -> start -> bind IO -> enable` |
| 控制队列 | CVQ 处理 MQ 等 virtio-net ctrl request | 无 CVQ，virtio-blk 请求由 `doca_devemu_vblk_io_event_vblk_req_register` 进入 |
| 数据搬运 | 网络 offload engine 接 representor | DOCA DMA 在 Host request buf 与 DPU mpool buf 间 memcpy |
| 当前后端完整性 | 面向 virtio-net 连通性和多队列 | 读写后端占位，适合 smoke test，不保证写后读一致性 |

## 6. 阅读建议

如果要继续定位问题，可以按以下顺序读代码：

1. `docs/run-guide.md`：确认运行模式和期望 Host 枚举现象。
2. `applications/vnet_pci_dev/vnet_pci_dev_core.c`：看 `vnet_pci_dev_run()`、`init_virtio_network_device()`、`virtio_net_ctrl_change_cb()`。
3. `applications/vnet_pci_dev/vnet_pci_device.c`：看 TLP request 分发、配置空间/MMIO 处理、workqueue 提交点。
4. `applications/vblk_pci_dev/vblk_pci_dev_core.c`：看 `vblk_pci_dev_run()`、线程和状态同步。
5. `applications/vblk_pci_dev/vblk_pci.c`：看 PCI/TLP type、TLP channel、config/MMIO 分发。
6. `applications/vblk_pci_dev/vblk_ctrl.c` 与 `vblk_io_ctx.c`：看 VQ 状态机、IO context、DMA request 完成路径。
