# NVMe Emulation 主干逻辑开发文档

本文档分析 `applications/nvme_emulation` 的主干逻辑。这里的 “host” 目录不是外部 x86 Host 程序，而是 DPU 用户态侧代码；`device` 目录是编译到 DPA 上运行的 device code。整体目标是在 DPU 上通过 DOCA DevEmu 暴露一个 NVMe PCI endpoint，并把 Host 驱动对 PCI BAR、doorbell、SQ/CQ 的访问接到 SPDK NVMe-oF target。

## 1. 模块边界

| 文件 | 职责 |
| --- | --- |
| `nvme_emulation.c` | 进程入口，只初始化 DOCA log 和 SPDK app，随后进入 `spdk_app_start()`。 |
| `host/doca_transport.c` | SPDK external NVMf transport `DOCA` 的主体，管理 DevEmu PCI endpoint、listener、poll group、admin QP、IO QP、NVMe command 分发。 |
| `host/nvmf_doca_io.c` / `.h` | DOCA IO 抽象层，封装 CQE 写回、SQE 读取、Host/DPU 数据 DMA、DPA doorbell 线程、Comch MsgQ。 |
| `device/doca_transport_dev.c` | DPA 线程逻辑，处理 DPU 发来的 bind/unbind/MSI-X 消息，并把 Host doorbell 通知转发回 DPU。 |
| `common/doca_transport_common.h` | DPU 和 DPA 共用的 Comch 消息格式、DPA thread 参数。 |
| `host/nvme_pci_type_config.h` | emulated NVMe PCI type 的 vendor/device/class、BAR、DB、MSI-X、stateful region 配置。 |
| `host/nvme_pci_common.c` / `.h` | DevEmu PCI type 创建、能力检查、representor 查找、资源清理等公共函数。 |
| `host/nvmf_rpc.c` / `rpc_nvmf_doca.py` | SPDK RPC 扩展，用于枚举 emulation manager、创建/销毁/list emulated function。 |

构建层面，`meson.build` 将 `host/doca_transport.c`、`host/nvmf_doca_io.c`、`host/nvmf_rpc.c`、`host/nvme_pci_common.c` 编入 DPU 用户态应用，并通过 `build_device_code.sh` 编译 `device/doca_transport_dev.c` 生成 DPA app `nvmf_doca_transport_app`。

## 2. 顶层入口

`applications/nvme_emulation/nvme_emulation.c` 的 `main()` 做的事情很少：

```text
main()
  -> doca_log_backend_create_standard()
  -> 设置 DOCA SDK log level
  -> spdk_app_opts_init()
  -> opts.name = "nvmf"
  -> spdk_app_parse_args()
  -> spdk_app_start(&opts, nvmf_tgt_started, NULL)
  -> spdk_app_fini()
```

真正业务逻辑不是从 `nvmf_tgt_started()` 开始，而是通过两个注册机制被 SPDK 调用：

- `SPDK_NVMF_TRANSPORT_REGISTER(doca, &spdk_nvmf_transport_doca)` 注册 custom transport。
- `SPDK_RPC_REGISTER(...)` 注册 DOCA function 管理类 RPC。

因此开发时应把 `nvme_emulation.c` 看作 SPDK event framework 的启动壳，主干代码在 `host/doca_transport.c`。

## 3. PCI Type 与 BAR 布局

`host/nvme_pci_type_config.h` 定义 Host 侧枚举到的 NVMe PCI function 外观：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| type name | `NVME Type` | `doca_devemu_pci_type_create()` 使用。 |
| vendor/device | `0x15b3 / 0x6001` | Host PCI config space ID。 |
| class code | `0x010802` | NVMe controller class。 |
| BAR0 | `log_size=0xf` | BAR0 大小 32 KiB，非 prefetchable。 |
| BAR1 | `log_size=0x0` | 配置了 BAR1，但主要逻辑集中在 BAR0。 |
| stateful region | BAR0 `0x0000`, size `0x80` | NVMe controller register block。 |
| DB region | BAR0 `0x1000`, size `0x1000` | doorbell 区域，4 字节 DB，stride 4 字节。 |
| MSI-X table | BAR0 `0x2000`, size `0x1000` | 4 个 MSI-X vector。 |
| MSI-X PBA | BAR0 `0x3000`, size `0x1000` | MSI-X pending bit array。 |

`nvmf_doca_pci_type_create_and_start()` 创建并启动 PCI type，除了设置上述配置，还会给 stateful region 写入默认 NVMe registers：

- `CAP.MQES = NVMF_DOCA_DEFAULT_MAX_QUEUE_DEPTH - 1`
- `CAP.CQR = 1`
- `CAP.TO = 0xf0`
- `CAP.CSS = 1`
- `VS = 1.3`

这些默认值让 Host NVMe driver 在 BAR0 起始处能读到基本 controller capability。

## 4. 管理类 RPC

`host/nvmf_rpc.c` 提供 function 管理，不负责把 function 绑定到 SPDK subsystem：

```text
nvmf_doca_get_managers
  -> doca_devinfo_create_list()
  -> 过滤 doca_devinfo_cap_is_hotplug_manager_supported()
  -> 返回 ibdev name

nvmf_doca_create_function(dev-name)
  -> create_find_start_pci_type()
  -> doca_devemu_pci_type_create_rep()
  -> 返回 VUID
  -> close rep，stop/destroy pci_type

nvmf_doca_list_functions(dev-name)
  -> create_find_start_pci_type()
  -> doca_devemu_pci_type_create_rep_list()
  -> 返回每个 rep 的 VUID 和 PCI address

nvmf_doca_destroy_function(dev-name, vuid)
  -> create_find_start_pci_type()
  -> create rep list
  -> 按 VUID open rep
  -> doca_devemu_pci_type_destroy_rep()
```

注意这里的 `pci_type` 生命周期是临时的：每次 RPC 创建、查找或销毁 function 后都会 `cleanup_pci_resources()`。真正运行 NVMe emulation 的 transport 创建时会再次创建长期存在的 PCI type。

## 5. Transport 创建

SPDK 用户通过标准 NVMf RPC 创建 transport 时，SPDK 会调用 `nvmf_doca_create()`：

```text
nvmf_doca_create()
  -> calloc nvmf_doca_transport
  -> 初始化 poll_groups / emulation_managers
  -> doca_devinfo_create_list()
  -> 查找支持 hotplug manager 的 DOCA device
  -> nvmf_doca_create_emulation_manager()
      -> doca_dev_open()
      -> nvmf_doca_pci_type_create_and_start()
      -> doca_dpa_create()
      -> doca_dpa_set_app(nvmf_doca_transport_app)
      -> doca_dpa_start()
  -> nvmf_doca_admin_poll_group_create()
      -> doca_pe_create()
      -> 在 SPDK app thread 上注册 poller
```

关键对象关系：

```text
nvmf_doca_transport
  -> emulation_managers[]
       -> doca_dev
       -> doca_devemu_pci_type
       -> doca_dpa
  -> admin_pg
       -> doca_pe                 # PCI dev ctx / stateful / hotplug / FLR 事件
       -> SPDK poller on app thread
  -> poll_groups[]
       -> doca_pe                 # 普通 IO SQ/CQ DMA progress
       -> admin_qp_pe             # admin QP 的 DMA/Comch progress，降低轮询频率
```

当前实现只取第一个可用 hotplug manager，然后 `break`。如果要支持多个 DPU function 或多个 emulation manager，入口在 `nvmf_doca_create()` 的 devinfo 遍历逻辑。

## 6. Listener 与 Hotplug

当 SPDK NVMf target 添加 listener，且 transport type 为 `DOCA` 时，调用 `nvmf_doca_listen()`。`trid->traddr` 被当作 emulated function 的 VUID。

```text
nvmf_doca_listen(trid.traddr = vuid)
  -> check_for_duplicate()
  -> nvmf_doca_pci_dev_admin_create()
      -> find_emulation_manager_and_function_by_vuid()
          -> 遍历每个 pci_type 的 rep list
          -> 按 VUID open doca_dev_rep
      -> calloc nvmf_doca_pci_dev_admin
      -> doca_devemu_pci_dev_create(pci_type, rep, admin_pg.pe)
      -> register_handlers_set_datapath_and_start()
          -> doca_ctx_set_datapath_on_dpa()
          -> 注册 ctx state change
          -> 注册 hotplug state change
          -> 注册 FLR
          -> 注册 stateful region write
          -> doca_ctx_start()
  -> TAILQ_INSERT admin_pg.pci_dev_admins
  -> doca_devemu_pci_dev_hotplug()
  -> devemu_hotplug_transition_wait(POWER_ON)
```

listener 与 SPDK subsystem 的关联由 `nvmf_doca_listen_associate()` 完成，只是把 `pci_dev_admin->subsystem` 指向对应 `spdk_nvmf_subsystem`。后续构造 fabric connect request 时会读取 subsystem NQN。

`nvmf_doca_stop_listen()` 做 hotunplug 和资源销毁，但要求 `admin_qp == NULL`。也就是说 stop listener 前必须让 Host 驱动释放 controller/queue，或者由 reset/FLR 流程先清理 admin QP 和 IO QP。

## 7. Controller 初始化

Host NVMe driver 枚举设备后，会读写 BAR0 stateful region，也就是 NVMe controller registers。所有 stateful write 都进入：

```text
stateful_region_write_event_handler_cb()
  -> handle_controller_register_events()
      -> doca_devemu_pci_dev_query_bar_stateful_region_values()
      -> 解析 CC / AQA / ASQ / ACQ / CSTS
```

当 Host 写 `CC.EN=1` 且 listener 仍是 `UNINITIALIZED`：

```text
handle_controller_register_events()
  -> state = INITIALIZING
  -> choose_poll_group()
  -> 从 registers 读取:
       ACQ = admin CQ host address
       ASQ = admin SQ host address
       AQA.ACQS / AQA.ASQS = admin queue depth - 1
  -> spdk_thread_exec_msg(poll_group thread, nvmf_doca_create_admin_qp)
```

`nvmf_doca_create_admin_qp()` 在选中的 poll group 线程上创建 admin CQ/SQ：

```text
nvmf_doca_create_admin_qp()
  -> calloc nvmf_doca_admin_qp
  -> nvmf_doca_create_pci_dev_poll_group()
      -> nvmf_doca_create_host_mmap()
          -> doca_devemu_pci_ep_mmap_create()
          -> mmap range [0, UINT64_MAX]
  -> nvmf_doca_io_create(admin CQ)
      -> DPA thread
      -> Comch MsgQ
      -> DB completion
      -> optional MSI-X vector 0
      -> CQ queue DMA
      -> CQ doorbell
      -> run DPA thread
  -> nvmf_doca_io_add_sq(admin SQ)
      -> SQ request pool
      -> SQ queue DMA
      -> data DMA pool
      -> SQ doorbell
      -> send BIND_SQ_DB to DPA
```

SQ 添加是异步的。DPA 收到 bind 后返回 `BIND_SQ_DB_DONE`，DPU 侧 `nvmf_doca_io_handle_bind_sq_db_done_msg()` 再调用 `nvmf_doca_sq_add_continue()`：

```text
nvmf_doca_sq_add_continue()
  -> doca_devemu_pci_db_start(sq->db)
  -> spdk_nvmf_tgt_new_qpair(target, &sq->spdk_qp)
      -> SPDK 调用 transport poll_group_add()
      -> nvmf_doca_connect_spdk_qp()
      -> SPDK fabrics CONNECT
      -> nvmf_doca_set_property()
      -> SPDK fabrics PROPERTY_SET(CC)
      -> nvmf_doca_create_admin_qp_done()
          -> state = INITIALIZED
          -> stateful region offset 28 写 ready=1
          -> pci_dev_admin->admin_qp = admin_qp
```

这里的 `offset 28` 对应 NVMe `CSTS` 附近的 ready 状态更新。Host 看到 controller ready 后，开始向 admin SQ 下发命令。

## 8. Admin Queue 深入

Admin queue 是这个应用里最重要的一段 glue code：Host NVMe driver 按真实 NVMe PCI controller 的方式分配 ASQ/ACQ，写 BAR0 controller register；DPU 侧不分配 Host queue memory，只读取 Host 写进 `ASQ/ACQ/AQA` 的地址，再把这些 Host IOVA 包装成 DOCA buffer，用 DMA 读 SQE、写 CQE。

### 8.1 Admin Queue Layout 来源

Admin queue 的 layout 由 Host 通过 BAR0 stateful region 提供：

```text
BAR0 stateful region, offset 0x0000
  CAP / VS / CC / CSTS / AQA / ASQ / ACQ ...

Host 初始化 controller:
  -> 写 AQA.ASQS = admin SQ depth - 1
  -> 写 AQA.ACQS = admin CQ depth - 1
  -> 写 ASQ = admin SQ base Host IOVA
  -> 写 ACQ = admin CQ base Host IOVA
  -> 写 CC.EN = 1

DPU stateful write callback:
  -> query stateful region
  -> admin_sq_address = registers->asq
  -> admin_cq_address = registers->acq
  -> admin_sq_size = registers->aqa.bits.asqs + 1
  -> admin_cq_size = registers->aqa.bits.acqs + 1
```

创建出的 admin layout 可以按下图理解：

```text
Host memory
  ASQ -> [ SQE0 ][ SQE1 ][ SQE2 ] ...      element size = 64B
  ACQ -> [ CQE0 ][ CQE1 ][ CQE2 ] ...      element size = 16B

DPU local memory
  admin_sq.queue.local_queue_address
       -> [ local SQE0 ][ local SQE1 ] ...  DMA read target
  admin_cq.queue.local_queue_address
       -> [ local CQE0 ][ local CQE1 ] ...  DMA write source

BAR0 DB region
  DB id 0 -> admin SQ tail / producer index
  DB id 1 -> admin CQ head / consumer index
```

这里的 `ASQ/ACQ` 是 Host IOVA，不是 DPU 虚拟地址。`nvmf_doca_create_host_mmap()` 通过 `doca_devemu_pci_ep_mmap_create()` 创建一个能访问 emulated endpoint Host address space 的 mmap，并把 memrange 设成 `[0, UINT64_MAX]`。后续 SQ/CQ/data buffer 都通过这个 mmap 把 Host IOVA 转成 DOCA buffer。

### 8.2 `nvmf_doca_io` 在 Admin QP 中代表什么

Admin QP 由一个 `struct nvmf_doca_admin_qp` 表示，但真正做 DMA/DB/DPA 的对象是 `struct nvmf_doca_io`：

```text
nvmf_doca_admin_qp
  -> admin_cq: struct nvmf_doca_io
       -> cq: struct nvmf_doca_cq
       -> db_comp: one DPA-polled DB completion context
       -> msix: vector 0，admin path 启用
       -> dpa_thread: one DPA thread
       -> comch: DPU <-> DPA MsgQ
       -> sq_list: contains admin SQ
  -> admin_sq: struct nvmf_doca_sq
```

`admin_cq` 这个名字容易误导：`nvmf_doca_io` 不只是 CQ，它是一个以 CQ 为中心的 IO group。它持有一个 CQ、一个 DB completion、一个 DPA thread 和若干 SQ。admin 场景下 `max_num_sq = 1`，所以这个 group 只接一个 admin SQ。

### 8.3 SQ Queue：读 Host SQE

Admin SQ 创建入口是 `nvmf_doca_io_add_sq()`，内部调用 `nvmf_doca_sq_create()`：

```text
nvmf_doca_sq_create()
  -> nvmf_doca_request_pool_create(sq_depth)
  -> nvmf_doca_queue_create(is_read_from_remote = true)
       remote_queue_address = ASQ
       element_size = sizeof(nvmf_doca_sqe) = 64
       num_elements = admin_sq_size
       local_queue_address = calloc(admin_sq_size * 64)
       for each idx:
         remote buf = Host ASQ + idx * 64
         local buf  = local_queue_address + idx * 64
         DMA task   = remote -> local
  -> nvmf_doca_dma_pool_create()
  -> create SQ DB on DPA, db_id = 2 * qid
```

SQ queue 的每个元素在初始化时就预分配了一个 DMA memcpy task。Host 每次写 SQ DB 后，DPA 把新的 producer index 送回 DPU；DPU 不重新创建 task，只提交已有 task：

```text
Host writes admin SQ DB value = new PI
  -> DPA sends HOST_DB(sq pointer, new PI)
  -> nvmf_doca_sq_update_pi()
      -> old pi 到 new pi 之间每个 sqe_idx:
           task = sq->queue.elements[sqe_idx]
           reset local dst buffer data len
           submit DMA remote ASQ[sqe_idx] -> local SQE[sqe_idx]
      -> sq->pi = new PI
  -> nvmf_doca_sq_sqe_read_cb()
      -> local SQE buffer -> fetch_sqe_cb()
      -> admin path: nvmf_doca_on_fetch_sqe_complete()
```

`sqe_idx` 会被保存到 `request->sqe_idx`，回 CQE 时写进 `sqhd`：

```text
post_cqe_from_response()
  -> request->request.rsp->nvme_cpl.sqhd = request->sqe_idx
  -> nvmf_doca_io_post_cqe()
```

这份代码用 `sq->pi` 作为 DPU 已处理到的 SQ tail shadow。它不主动读 Host SQ head；Host 到 DPU 的新请求边界完全由 SQ doorbell value 驱动。

### 8.4 CQ Queue：写 Host CQE

Admin CQ 创建发生在 `nvmf_doca_io_create()` 内部的 `nvmf_doca_cq_create()`：

```text
nvmf_doca_cq_create()
  -> nvmf_doca_queue_create(is_read_from_remote = false)
       remote_queue_address = ACQ
       element_size = sizeof(nvmf_doca_cqe) = 16
       num_elements = admin_cq_size
       local_queue_address = calloc(admin_cq_size * 16)
       for each idx:
         local buf  = local_queue_address + idx * 16
         remote buf = Host ACQ + idx * 16
         DMA task   = local -> remote
  -> create CQ DB on DPA, db_id = 2 * qid + 1
  -> start CQ DB
```

CQE post 的核心逻辑：

```text
nvmf_doca_io_post_cqe()
  -> cqe_idx = cq->pi % cq_depth
  -> task = cq->queue.elements[cqe_idx]
  -> local CQE buffer = task src
  -> Host CQE buffer = task dst
  -> *local_cqe = response cqe
  -> phase = !((cq->pi / cq_depth) % 2)
  -> local_cqe->status.p = phase
  -> submit DMA local CQE -> Host ACQ[cqe_idx]
  -> cq->pi++

nvmf_doca_cq_cqe_post_cb()
  -> nvmf_doca_io_raise_msix()
  -> admin post_cqe_cb = nvmf_doca_on_post_cqe_complete()
  -> request free
```

CQ doorbell 的作用是更新 DPU 侧 CQ consumer index shadow：

```text
Host writes admin CQ DB value = new CI
  -> DPA sends HOST_DB(db_user_data = 0, db_value = new CI)
  -> nvmf_doca_cq_update_ci(cq, new CI)
```

当前代码只保存 `cq->ci`，没有在 `nvmf_doca_io_post_cqe()` 中检查 CQ full。因此开发者如果要增强 backpressure，需要在 post CQE 前加入 `pi/ci` 环形队列容量判断。

### 8.5 Admin Data Buffer：命令数据搬运

Admin SQE 本身只有 64B，Identify、Get Log Page、Get/Set Features 等命令的数据区在 Host PRP 指向的 data buffer 中。admin data path 由 `nvmf_doca_dma_pool` 管理：

```text
nvmf_doca_sq_create()
  -> nvmf_doca_dma_pool_create()
       local_data_memory = spdk_dma_zmalloc(max_ops * 4096)
       local_data_mmap   = mmap(local_data_memory)
       local_data_pool   = 4096B buffer pool
       host_data_inventory = wraps Host IOVA buffers
       host_data_mmap = pci_dev_pg->host_mmap
       dma = DOCA DMA ctx on admin_qp_pe
```

admin SQ 的 `max_dma_operations = sq_depth * NVMF_REQ_MAX_BUFFERS`，单个 DPU data buffer 大小固定为 `DMA_POOL_DATA_BUFFER_SIZE = 4096`。

admin 命令进入 `nvmf_doca_on_fetch_sqe_complete()` 后先确定数据方向和长度：

```text
ASYNC_EVENT_REQUEST:
  length = 0, DATA_NONE

IDENTIFY:
  length = 4096

GET_LOG_PAGE:
  length = (NUMD + 1) * 4

GET_FEATURES / SET_FEATURES:
  部分 feature length = 4096 / 512 / 256 / 16 / 8
  另一部分强制 DATA_NONE
```

然后按方向走三条路径：

```text
DATA_NONE:
  spdk_nvmf_request_exec()
  -> post CQE

HOST_TO_CONTROLLER:
  init_dpu_host_buffers()
    -> host buffer = PRP1, size 4096
    -> dpu buffer  = local_data_pool alloc 4096
    -> request.data / request.iov 指向 DPU buffer
  DMA Host PRP1 -> DPU buffer
  spdk_nvmf_request_exec()
  post CQE

CONTROLLER_TO_HOST:
  init_dpu_host_buffers()
    -> host buffer = PRP1, size 4096
    -> dpu buffer  = local_data_pool alloc 4096
    -> request.data / request.iov 指向 DPU buffer
  spdk_nvmf_request_exec()
  DMA DPU buffer -> Host PRP1
  post CQE
```

与 IO queue 的 READ/WRITE 不同，admin path 当前不调用 `nvme_cmd_map_prps()`。也就是说 admin data 当前按 “PRP1 指向一段连续 Host buffer” 处理，不拆 PRP2，也不处理 PRP list。扩展 admin opcode 时要特别检查 `request.length` 是否可能超过 4096 或跨页；如果会超过，应该复用或抽象 IO path 的 PRP list 处理，而不是直接扩大 `request.length`。

request 和 buffer 的释放链路：

```text
nvmf_doca_request_get()
  -> 从 sq->request_pool 取一个 request

data copy callback / SPDK completion
  -> nvmf_doca_request_complete()
      -> request->doca_cb(...)

post CQE done
  -> nvmf_doca_req_free()
      -> nvmf_doca_request_free()
          -> dec_ref dpu_buffer[]
          -> dec_ref host_buffer[]
          -> free request.data if data_from_alloc
          -> request 归还 sq->request_pool
```

`nvmf_doca_request_free_impl()` 会清空 `command/cq_entry/iov/length/buffer counters`。因此 request 对象是复用的，不能把 request 内部字段地址长期保存到异步流程之外。

### 8.6 Admin DB 与 DPA 绑定时序

Admin QP 的 DB/DPA 初始化分两段：CQ DB 在 DPA thread 启动前通过 RPC 绑定；SQ DB 在 SQ 创建后通过 Comch 消息绑定。

```text
nvmf_doca_io_create(admin CQ group)
  -> create DPA thread object
  -> create Comch send/recv MsgQ
  -> create DB completion, max_num_dbs = max_num_sq + 1 = 2
  -> create MSI-X vector 0 on DPA
  -> create CQ DB, db_id = 1, user_data = 0, start
  -> nvmf_doca_io_run_dpa_thread()
      -> collect DPA handles
      -> doca_dpa_rpc(io_thread_init_rpc, db_comp, cq_db, consumer)
          -> DPA binds CQ DB to db_comp
          -> DPA acks consumer credits
      -> copy io_thread_arg to DPA memory
      -> doca_dpa_thread_run()

nvmf_doca_io_add_sq(admin SQ)
  -> create SQ DB, db_id = 0, user_data = sq pointer
  -> send COMCH_MSG_TYPE_BIND_SQ_DB(db_handle, cookie = sq)
  -> DPA binds SQ DB to same db_comp
  -> DPA replies BIND_SQ_DB_DONE(cookie = sq)
  -> DPU marks sq DB bound
  -> start SQ DB
  -> spdk_nvmf_tgt_new_qpair()
```

为什么 CQ DB 先通过 RPC 绑定，而 SQ DB 走消息绑定：`nvmf_doca_io_run_dpa_thread()` 必须先让 DPA thread 能收到 CQ doorbell 和 Comch 消息；而 SQ 可以在 DPA thread 运行后动态添加或删除，所以用 Comch 做 bind/unbind。

### 8.7 Comch 方向

`nvmf_doca_dpa_comch` 里有两个 MsgQ，命名是以 DPU 视角看的：

| 字段 | 方向 | 用途 |
| --- | --- | --- |
| `comch.send` | DPU -> DPA | bind SQ DB、unbind SQ DB、raise MSI-X。 |
| `comch.recv` | DPA -> DPU | bind/unbind done、Host DB notification。 |

DPU 侧为 `comch.recv.consumer` 预投递 `MAX_NUM_COMCH_MSGS` 个 receive task；收到消息后 callback 会立即重新 submit 同一个 recv task，然后分发：

```text
nvmf_doca_dpa_msgq_recv_cb()
  -> resubmit recv task
  -> nvmf_doca_io_handle_dpa_msg()
      -> BIND_SQ_DB_DONE: sq->db_state = BOUND; continue add
      -> UNBIND_SQ_DB_DONE: sq->db_state = UNBOUND; continue stop
      -> HOST_DB: update CQ CI or SQ PI
```

DPA 侧收到 DPU 消息后使用 immediate-only send 回复。Host doorbell 也被 DPA 转成 `COMCH_MSG_TYPE_HOST_DB`，携带两个字段：

- `db_user_data`：创建 DB 时设置的 user data。CQ 为 `0`，SQ 为 `struct nvmf_doca_sq *`。
- `db_value`：Host 写入 doorbell 的 32-bit value，admin SQ 中表示新的 SQ tail，admin CQ 中表示新的 CQ head。

### 8.8 Admin Queue 与 IO Queue 的关键差异

| 项 | Admin Queue | IO Queue |
| --- | --- | --- |
| 创建触发 | Host 写 `CC.EN=1`，从 `ASQ/ACQ/AQA` 创建。 | Admin command `CREATE_IO_CQ/CREATE_IO_SQ`。 |
| qid | 固定 `0`。 | Host 指定。 |
| CQ MSI-X | 固定启用 vector 0。 | 按 `CREATE_IO_CQ` 的 `IEN/IV`。 |
| 所属 PE | `doca_pg->admin_qp_pe`，poll rate 被限频。 | `doca_pg->pe`，常规 poll。 |
| SQ 数量 | `max_num_sq = 1`。 | 每个 IO CQ 可挂多个 SQ，代码默认 `max_num_sq = 64`。 |
| data buffer | 当前按 PRP1 单 buffer 处理。 | READ/WRITE 走 PRP1/PRP2/PRP list 映射。 |
| SPDK connect | 创建 admin SQ 后自动 fabric CONNECT qid 0，再 PROPERTY_SET CC。 | 创建 IO SQ 后 fabric CONNECT 对应 qid。 |

## 9. Doorbell 与 DPA 线程

每个 `nvmf_doca_io` 都有一个 DPA thread，函数是 `device/doca_transport_dev.c::io_thread()`。它有两类输入：

- DPU 发给 DPA 的 Comch 消息：bind/unbind SQ DB、raise MSI-X。
- Host 写 DB region 后产生的 DB completion：CQ doorbell 和 SQ doorbell。

DPA 主循环：

```text
io_thread()
  -> handle_dpu_msgs()
      -> BIND_SQ_DB: db_completion_bind_db(); 回 BIND_SQ_DB_DONE
      -> UNBIND_SQ_DB: db_completion_unbind_db(); 回 UNBIND_SQ_DB_DONE
      -> RAISE_MSIX: doca_dpa_dev_devemu_pci_msix_raise()
  -> handle_dbs()
      -> get DB completions
      -> ack/request notification
      -> handle_db()
          -> read DB value
          -> 回 HOST_DB(db_user_data, db_value)
  -> doca_dpa_dev_thread_reschedule()
```

DB id 映射在 `nvmf_doca_io.c` 中固定：

| 队列 | DB id |
| --- | --- |
| SQ qid | `2 * qid` |
| CQ qid | `2 * qid + 1` |

CQ DB 的 `user_data = 0`，所以 DPU 收到 `HOST_DB` 时如果 `db_user_data == NULL`，就更新 CQ consumer index；否则把 `db_user_data` 解释成 `struct nvmf_doca_sq *` 并更新 SQ producer index。

```text
nvmf_doca_io_handle_host_db_msg()
  -> if msg.db_user_data == NULL:
       nvmf_doca_cq_update_ci(cq, db_value)
     else:
       nvmf_doca_sq_update_pi(sq, db_value)
```

`nvmf_doca_sq_update_pi()` 会根据 Host 写入的新 PI 计算新增 SQE 个数，并提交对应 SQE DMA read task。DMA 成功后进入 transport 层的 `fetch_sqe_cb`。

## 10. Admin Command 路径

admin SQE 读取完成后进入 `nvmf_doca_on_fetch_sqe_complete()`：

```text
nvmf_doca_on_fetch_sqe_complete()
  -> 从 SQE 解析 spdk_nvme_cmd
  -> 从 request pool 取 nvmf_doca_request
  -> 设置 request.cmd / qpair / sqe_idx / xfer
  -> 特殊处理 queue management opcode
  -> 普通 admin opcode 设置 data length
  -> 按数据方向进入执行流
```

admin queue management 命令由 transport 自己处理：

| Opcode | 处理函数 | 主干 |
| --- | --- | --- |
| `CREATE_IO_CQ` | `handle_create_io_cq()` | 选择 poll group，创建 IO CQ，回 admin CQE。 |
| `CREATE_IO_SQ` | `handle_create_io_sq()` | 找对应 IO CQ，在同一 poll group 创建 IO SQ，connect SPDK qpair。 |
| `DELETE_IO_SQ` | `handle_delete_io_sq()` | 向 owning poll group 发 stop SQ 消息，完成后回 CQE。 |
| `DELETE_IO_CQ` | `handle_delete_io_cq()` | 向 owning poll group 发 stop IO/CQ 消息，完成后回 CQE。 |

普通 admin 命令交给 SPDK NVMf target：

```text
DATA_NONE:
  begin_nvme_cmd_data_none()
    -> spdk_nvmf_request_exec()
    -> post_cqe_from_response()

HOST_TO_CONTROLLER:
  begin_nvme_admin_cmd_data_host_to_dpu()
    -> init_dpu_host_buffers()
    -> nvmf_doca_sq_copy_data(host -> dpu)
    -> execute_spdk_request()
    -> post_cqe_from_response()

CONTROLLER_TO_HOST:
  begin_nvme_admin_cmd_data_dpu_to_host()
    -> init_dpu_host_buffers()
    -> spdk_nvmf_request_exec()
    -> copy_dpu_data_to_host()
    -> post_cqe_from_response()
```

`post_cqe_from_response()` 最终调用 `nvmf_doca_io_post_cqe()`，把 SPDK completion DMA 写回 Host CQ。

## 11. IO Queue 与 NVM Command 路径

Host 通过 admin command 创建 IO CQ/SQ 后，IO CQ 被分配到 `choose_poll_group()` 选出的 poll group；IO SQ 必须挂到对应 IO CQ 所在的 `nvmf_doca_pci_dev_poll_group`，保证同一组 CQ/SQ 在同一 SPDK thread / DOCA PE 上 progress。

IO SQE 读取完成后进入 `nvmf_doca_on_fetch_nvm_sqe_complete()`：

```text
nvmf_doca_on_fetch_nvm_sqe_complete()
  -> 支持 FLUSH / WRITE / READ
  -> READ/WRITE length = (nr + 1) * 512
  -> 按数据方向分发
```

NVM READ/WRITE 使用 PRP 映射：

```text
init_dpu_host_buffers()
  -> 非 admin 且 PSDT == PRP:
       nvme_cmd_map_prps()
          -> PRP1 生成第一个 Host/DPU buffer
          -> PRP2 为单页或 PRP list
          -> 如果是 PRP list，先 DMA 读取 PRP list
          -> 组装 request.iov[]
```

WRITE 路径：

```text
Host SQ doorbell
  -> DPA HOST_DB
  -> nvmf_doca_sq_update_pi()
  -> DMA read SQE
  -> nvmf_doca_on_fetch_nvm_sqe_complete()
  -> begin_nvme_cmd_data_host_to_dpu()
  -> nvme_cmd_map_prps()
  -> DMA copy Host data -> DPU buffers
  -> spdk_nvmf_request_exec()
  -> post CQE
```

READ 路径：

```text
Host SQ doorbell
  -> DPA HOST_DB
  -> DMA read SQE
  -> nvmf_doca_on_fetch_nvm_sqe_complete()
  -> begin_nvme_cmd_data_dpu_to_host()
  -> nvme_cmd_map_prps()
  -> spdk_nvmf_request_exec()
  -> DMA copy DPU buffers -> Host PRP buffers
  -> post CQE
```

CQE 写回逻辑：

```text
nvmf_doca_io_post_cqe()
  -> cqe_idx = cq->pi % cq_depth
  -> 写 phase bit
  -> DMA copy local CQE -> Host CQE
  -> cq->pi++

nvmf_doca_cq_cqe_post_cb()
  -> 如果启用 MSI-X，发 RAISE_MSIX 给 DPA
  -> 调用 post_cqe_cb
      -> request free
```

## 12. Reset、Shutdown 与 FLR

Host 写 controller register 会触发 reset/shutdown：

```text
handle_controller_register_events()
  -> 如果 state INITIALIZED/INITIALIZATION_ERROR 且 CC.SHN 为 normal/abrupt:
       nvmf_doca_pci_dev_admin_reset()
  -> 如果 state INITIALIZED/INITIALIZATION_ERROR 且 CC.EN == 0:
       nvmf_doca_pci_dev_admin_reset()
```

FLR 事件走同一套 reset 逻辑：

```text
flr_event_handler_cb()
  -> pci_dev_admin->is_flr = true
  -> nvmf_doca_pci_dev_admin_reset()
```

reset 是异步拆解：

```text
nvmf_doca_pci_dev_admin_reset()
  -> state = RESETTING
  -> 如果 admin_qp 存在:
       向 admin_qp_pg 线程发送 nvmf_doca_destroy_admin_qp()
     否则:
       nvmf_doca_pci_dev_admin_reset_continue()

nvmf_doca_destroy_admin_qp()
  -> stop all IO SQs
  -> stop all IO CQs
  -> stop admin SQ
  -> stop admin CQ
  -> free admin_qp
  -> 回 admin_pg.thread 调用 reset_continue()

nvmf_doca_pci_dev_admin_reset_continue()
  -> state = UNINITIALIZED
  -> 如果 SHN 请求，设置 CSTS.SHST = COMPLETE
  -> 如果 CC.EN == 0，设置 CSTS.RDY = 0
  -> 写回 stateful region
  -> 如果 is_flr，doca_ctx_stop(pci_dev ctx)，等待 ctx IDLE 后自动 restart
```

`devemu_state_changed_cb()` 在 PCI dev ctx 进入 `IDLE` 时，如果不是 destroy flow，会重新 `doca_ctx_start()`；进入 `RUNNING` 时会调用 `handle_controller_register_events()`，用于继续处理 Host register 状态。

## 13. 线程与 Progress 模型

按代码显式创建/注册的执行流看：

| 执行流 | 创建位置 | 负责内容 |
| --- | --- | --- |
| SPDK app thread | `spdk_app_start()` | RPC、admin poller、PCI dev ctx event progress。 |
| SPDK reactor/poll group threads | SPDK NVMf target | 调用 `nvmf_doca_poll_group_poll()`，progress IO PE 和低频 admin QP PE。 |
| 每个 `nvmf_doca_io` 一个 DPA thread | `nvmf_doca_io_create()` | 接收 DPU Comch 消息，处理 Host doorbell，raise MSI-X。 |

`doca_pe_progress()` 位置：

- `admin_pg.pe`：SPDK app thread poller，处理 PCI dev ctx、hotplug、FLR、stateful write。
- `doca_pg.pe`：poll group poll，处理普通 IO CQ/SQ DMA、data DMA、Comch receive/send。
- `doca_pg.admin_qp_pe`：poll group poll 中每 1000 次普通 poll 轮询一次，处理 admin QP 相关任务。

## 14. 关键对象生命周期

```text
transport create
  -> emulation manager
      -> doca_dev
      -> pci_type
      -> dpa

listen(vuid)
  -> pci_dev_admin
      -> dev_rep
      -> pci_dev
      -> stateful region callbacks
      -> hotplug

Host CC.EN=1
  -> admin_qp
      -> pci_dev_poll_group
      -> host mmap
      -> admin CQ nvmf_doca_io
          -> DPA thread
          -> Comch
          -> DB completion
          -> CQ DB / optional MSI-X
      -> admin SQ

CREATE_IO_CQ / CREATE_IO_SQ
  -> IO CQ nvmf_doca_io
  -> IO SQ
  -> SPDK qpair connect

reset / delete / stop
  -> stop SQ
  -> unbind SQ DB on DPA
  -> stop data DMA and SQ queue DMA
  -> stop CQ DB and CQ queue DMA
  -> stop Comch / destroy DPA thread
  -> free qpair and queue objects
```

## 15. 开发切入点

常见改动位置：

- 修改 emulated PCI 外观：`host/nvme_pci_type_config.h` 和 `nvmf_doca_pci_type_create_and_start()`。
- 增加或调整 management RPC：`host/nvmf_rpc.c` 和 `rpc_nvmf_doca.py`。
- 调整 controller enable/reset 行为：`handle_controller_register_events()`、`nvmf_doca_pci_dev_admin_reset*()`。
- 支持更多 admin opcode：`nvmf_doca_on_fetch_sqe_complete()`，注意设置 `request.length` 和 `request.xfer`。
- 支持更多 NVM opcode：`nvmf_doca_on_fetch_nvm_sqe_complete()`，并确认 PRP/数据方向逻辑。
- 调整 doorbell 行为：`device/doca_transport_dev.c`、`nvmf_doca_io_handle_host_db_msg()`、SQ/CQ DB id 映射。
- 调整 CQE/MSI-X 行为：`nvmf_doca_io_post_cqe()`、`nvmf_doca_cq_cqe_post_cb()`、`nvmf_doca_io_raise_msix()`。
- 调整 queue 分配策略：`choose_poll_group()`、`handle_create_io_cq()`、`handle_create_io_sq()`。

需要特别注意：

- `nvmf_doca_io_add_sq()` 是异步流程，不能在发送 bind 消息后立即认为 SQ ready；真正 ready 在 `BIND_SQ_DB_DONE` 后。
- `stop_listen()` 不会强制清理已有 admin QP，调用前要先让 controller reset/shutdown 或处理 FLR。
- CQE 写回是异步 DMA，request 只能在 `post_cqe_cb` 后释放。
- READ/WRITE 的数据 buffer 和 PRP list buffer 使用 DOCA buf refcount，新增路径要确保所有完成和错误路径都释放。
- Admin QP 和 IO QP 可能在不同 SPDK thread 上，跨线程操作通过 `spdk_thread_exec_msg()`，不要直接跨线程销毁对象。
