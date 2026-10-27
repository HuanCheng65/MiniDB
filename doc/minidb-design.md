# MiniDB 设计文档

> 版本：v0.2
> 适用范围：数据库系统实现课程实验一（存储管理），同时作为后续四次实验的总体架构基线
> 状态：第 4 ~ 7 章的磁盘格式按 v0.2（"每个关系一个文件"，文件头 `format_version = 2`）冻结
>
> v0.2 相对 v0.1 的变化：磁盘组织由"单数据文件"改为"每个关系一个数据文件 + 一个 FSM 文件"（理由见 5.1）；
> 文件头字段随之调整（5.3）；FSM 移出段，放入独立文件（5.6）；系统表增加文件名列（10.1）；
> 补充了实现中确定的细节（7.3、7.4、8.6、9.2）。

---

## 目录

1. 目标与范围
2. 技术选型与工程约定
3. 总体架构
4. 基础类型与通用约定
5. 磁盘文件布局
6. 类型系统与记录格式
7. 空闲空间管理
8. 缓冲区管理
9. 表堆（TableHeap）
10. 系统目录（Catalog）
11. 事务、并发与恢复的预留设计
12. 后续实验的接入点
13. 可观测性：日志与统计
14. 工具：shell 与 loader
15. 测试方案
16. 实施计划
17. 风险与待定问题
18. 附录：核心接口草案

---

## 1. 目标与范围

### 1.1 背景

课程共五次实验，最终目标是一个可以运行的小型关系数据库。实验一要求实现：

- 数据文件组织：段页式，支持 `char`、`varchar`、`int`、`long`、`float`、`double`、`date`，不要求大对象；
- 空闲空间管理：空间组织、分配、回收；
- 缓冲区管理：页面组织、查找、淘汰；
- 验收：导入 TPC-H `supplier` 表（10000 条），展示缓冲区和空闲空间的管理过程，演示增删改。

评分项中「管理的测试数据大小」占 20 分，因此本设计以**完整导入 TPC-H SF1（lineitem 约 600 万行）**为规模目标。

后续实验预计为：索引、查询处理、并发控制、故障恢复（以课程实际安排为准）。

### 1.2 设计目标

| 编号 | 目标 | 说明 |
|---|---|---|
| G1 | 磁盘格式一次定型 | 后续实验需要的页头字段、记录头字段在实验一就预留，避免返工 |
| G2 | 接口稳定、实现可替换 | 淘汰策略、FSM 搜索、日志管理器等均通过接口隔离，内部实现可升级 |
| G3 | 规模可扩展 | 单文件最大约 32 GB，单段最大约 2 GB，满足 TPC-H SF1 |
| G4 | 过程可观测 | 缓冲区命中/淘汰/刷盘、区和页的分配/回收都有结构化日志与计数器 |
| G5 | 易于验收 | 任意 Linux 机器一条命令构建；提交的可执行文件静态链接 C++ 运行时 |
| G6 | 复杂度可控 | 只引入"成本低、收益高"的现代特性，避免为本课程规模过度设计 |

### 1.3 非目标（明确不做）

- 大对象（BLOB/TEXT 跨页存储）；
- 用户可配置的表空间、分区（每个关系固定对应一组文件，见 5.1）；
- 压缩、加密；
- `O_DIRECT`、io_uring 等异步 I/O（理由见 5.9）；
- 无锁数据结构、指针混写（pointer swizzling）；
- 客户端/服务器网络协议（系统以嵌入式库 + 命令行工具形式提供）；
- 实验一不处理崩溃一致性（正常关闭保证数据完整；崩溃恢复留给恢复实验）。

---

## 2. 技术选型与工程约定

### 2.1 语言与编译器

- 语言标准：**C++20**。不使用 C++23 特性，不使用 C++20 modules；
- 最低编译器：GCC 11 / Clang 14；`std::format` 不依赖（GCC 13 之前不完整），格式化输出用自带的小工具函数；
- 主平台：Linux x86-64；Windows 用户通过 WSL 构建运行；
- 字节序：按小端存储，代码中假设运行平台为小端并在启动时断言。

### 2.2 依赖

| 依赖 | 用途 | 引入方式 |
|---|---|---|
| （无） | | CRC32C、日志、SQL 解析、测试断言均自行实现；测试为两个自包含的可执行文件（见第 15 章），不依赖 GoogleTest |

### 2.3 构建
1: 客户端构建
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/minidb_shell --db data/tpch.db
```
2：动态链接库构建（用于嵌入式接入c++程序）
示例
```
int main(int argc, char** argv) {
    mydb_db* db;
    mydb_open(argv[1], nullptr, &db);
    // REPL：读一行 -> mydb_exec / mydb_prepare+step
    mydb_close(db);
}
```

- CMake ≥ 3.16，提供 `CMakePresets.json`：`dev`（Debug + ASan/UBSan + `-Werror`）、`release`（O2 + 静态链接 C++ 运行时）；
- 提交的可执行文件使用 `-static-libstdc++ -static-libgcc` 链接；
- 附 `Dockerfile`（基于 Ubuntu 22.04）作为构建环境兜底；
- `-Werror` 只在 `dev` 预设中开启，避免助教环境的新版编译器因新警告导致构建失败。

### 2.4 目录结构

```
MiniDB/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── include/minidb/minidb.h    # 公开 C API
├── doc/
│   └── minidb-design.md       # 本文档
├── src/
│   ├── common/                # 类型、Result、日志、CRC32C、日期/定点数工具
│   ├── storage/               # DiskManager、通用页头、文件头页
│   ├── space/                 # SpaceManager（区位图）、Segment、FreeSpaceMap
│   ├── buffer/                # BufferPool、Replacer、PageGuard
│   ├── record/                # 类型系统、Schema、Tuple 编解码
│   ├── table/                 # HeapPage、TableHeap、迭代器
│   ├── catalog/               # Catalog、系统表引导
│   ├── txn/                   # Transaction、TxnManager（实验一为最小实现）
│   ├── log/                   # LogManager（实验一为空实现）
│   ├── sql/                   # 词法 + 递归下降解析器
│   ├── db/                    # Database：组件装配、语句执行（含 COPY、PRAGMA）、启动/关闭流程
│   └── capi/                  # C API 封装
├── tools/shell/               # 交互式命令行
├── examples/demo.cpp          # 嵌入式 API 示例
├── tests/
│   ├── test_storage.cpp       # 存储层测试（内部 C++ 接口）
│   └── test_minidb.cpp        # 集成测试（C API）
├── scripts/demo_supplier.txt  # 验收演示脚本
└── test/tpc-h/schema.sql      # 实验文档 Listing 1 的 TPC-H DDL；test/ 下其余文件（.tbl 数据）不入库
```

构建产物：`libminidb_core.a`（核心库）、`libminidb.so`、`minidb_shell`、`minidb_demo`、`minidb_tests`、`minidb_storage_tests`。工具和集成测试只通过公开头文件访问系统；存储层测试直接链接核心库的内部接口。批量导入是 SQL 的 `COPY`（14.4），不再单独提供 `tools/loader`。尚未提供：`Dockerfile`。

### 2.5 代码规范

- 命名空间 `minidb`；类型和函数用 PascalCase，局部变量 snake_case，成员变量以 `_` 结尾；
- 头文件中禁止 `using namespace`；
- 一个主要类一对 `.h/.cpp`；
- 提供 `.clang-format`，提交前格式化；
- 禁止全局可变状态和单例（日志器除外，见第 13 章）。

---

## 3. 总体架构

### 3.1 分层

```
┌──────────────────────────────────────────┐  ┌────────────────┐
│ 查询处理（后续）  解析 → 计划 → 执行器         │  │ Catalog        │
├────────────────────┬─────────────────────┤  ├────────────────┤
│ TableHeap          │ B+ 树索引（后续）       │  │ 事务与锁（后续）  │
├────────────────────┴─────────────────────┤  ├────────────────┤
│ BufferPool  帧 · 页表 · Replacer · PageGuard│  │ LogManager     │
├──────────────────────────────────────────┤  │ （实验一为空实现）│
│ SpaceManager / Segment / FSM              │  ├────────────────┤
├──────────────────────────────────────────┤  │ 恢复（后续）     │
│ DiskManager  数据文件 · 日志文件（后续）       │  └────────────────┘
└──────────────────────────────────────────┘
```

### 3.2 模块职责

| 模块 | 职责 | 依赖 |
|---|---|---|
| DiskManager | 按页号读写数据文件；扩展文件；计算/校验页校验和 | 无 |
| BufferPool | 页缓存、查找、淘汰、刷盘；提供 PageGuard 与页锁存 | DiskManager、LogManager |
| SpaceManager | 区的分配与回收（区位图）；段的创建与删除 | BufferPool |
| Segment | 段内页的分配与回收（段头页中的区列表和页掩码）；FSM | BufferPool、SpaceManager |
| TableHeap | 记录的增删改查与扫描；转发指针；页内整理 | Segment、BufferPool、Tuple |
| Tuple / Schema | 类型系统、记录编解码 | 无 |
| Catalog | 表与列的元数据，持久化在系统表中 | TableHeap |
| TxnManager | 分配事务 id，维护事务状态 | LogManager |
| LogManager | 预写日志；实验一为空实现 | DiskManager |
| Database | 按固定顺序构造所有组件，负责打开、关闭、（以后的）恢复 | 全部 |

### 3.3 依赖规则

1. 只允许上层依赖下层，同层之间通过 `Database` 注入的引用协作；
2. 除 BufferPool 和 DiskManager 外，任何模块不得直接读写磁盘；
3. 所有对页内容的修改都通过第 11.2 节的统一修改路径完成；
4. 组件之间通过构造函数传引用，`Database` 持有所有组件的所有权。

### 3.4 Database 的生命周期

数据库是一个目录，目录中每个关系（含两张系统表）各有一个 `<name>.dat` 和一个 `<name>.fsm`（见 5.1）。

**创建新库**（目录不存在或为空）：

1. 创建 `sys_tables.dat/.fsm` 与 `sys_columns.dat/.fsm`：每个数据文件写入文件头页（页 0）和区位图页（页 1），区 0 标记为已用；
2. 为两张系统表各分配一个区并创建段，段头页号写入各自数据文件的文件头；
3. 把两张系统表自身的元数据行写入系统表；
4. `FlushAll`，设置 `clean_shutdown = 1`。

**打开已有库**：

1. 打开 `sys_tables.dat`、`sys_columns.dat`，校验文件头（魔数、版本、页大小、区大小、文件类型），从文件头取段头页号；
2. 扫描系统表，按 `data_file` / `fsm_file` 列打开其余关系的文件，加载 Catalog；
3. 对每个文件：若 `clean_shutdown == 0`，实验一打印警告（恢复实验中在此处执行 ARIES 恢复）；然后置 `clean_shutdown = 0` 并立即写回。

**关闭**：

1. 确认没有活跃事务；
2. 各段把内存中的段元信息写回段头页，`BufferPool::FlushAll()`，对所有文件 `fsync`；
3. 对每个文件置 `clean_shutdown = 1`，写回文件头，`fsync`。

---

## 4. 基础类型与通用约定

### 4.1 基本类型

```cpp
using page_id_t  = uint32_t;   // 物理页号，0 ~ 2^32-2
using frame_id_t = uint32_t;
using extent_id_t = uint32_t;
using segment_id_t = uint32_t;
using slot_id_t  = uint16_t;
using txn_id_t   = uint64_t;
using lsn_t      = uint64_t;   // 日志文件中的字节偏移

constexpr page_id_t   kInvalidPageId = UINT32_MAX;
constexpr lsn_t       kInvalidLsn    = 0;
constexpr uint32_t    kPageSize      = 8192;
constexpr uint32_t    kPagesPerExtent = 64;     // 区大小 512 KB

struct RID {
  page_id_t page_id;
  slot_id_t slot;
};
```

页号是**文件内**页号；缓冲池用 `(file_id, page_id)` 标识一页，`file_id` 是进程内为每个打开的文件分配的编号，不落盘。
页号到文件偏移：`offset = (uint64_t)page_id * kPageSize`，必须使用 64 位运算。
区号到页号：`first_page = extent_id * kPagesPerExtent`。数据文件的区 0 包含页 0（文件头）和页 1（区位图），整个区 0 保留给系统使用。

### 4.2 错误处理

| 场景 | 方式 |
|---|---|
| 可预期的失败（记录不存在、页空间不足、类型不匹配、表已存在） | 返回 `Result<T>` / `Status` |
| 事务需要中止（以后的死锁、冲突） | 抛出 `TxnAbortException`，由执行器顶层捕获并调用 `TxnManager::Abort` |
| 不变量被破坏（校验和错误、页类型不符、内部逻辑错误） | `MINIDB_CHECK` 断言，打印上下文后终止 |

`Result<T>` 自行实现（约 50 行），持有 `T` 或 `Status{code, message}`。PageGuard 采用 RAII，异常传播时会自动释放 pin 和锁存。

### 4.3 配置项

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `buffer_pool_frames` | 1024（8 MB） | 演示淘汰时可调小 |
| `replacer` | `clock` | 可选 `lru2` |
| `log_level` | `info` | `debug` 时输出每次缓冲区和空间操作 |
| `log_file` | 空 | 非空时日志同时写入文件 |
| `fsm_search` | `hint_linear` | FSM 搜索策略 |

配置从命令行参数读取，不引入配置文件格式依赖。

---

## 5. 磁盘文件布局

### 5.1 整体布局：每个关系一个文件

数据库是一个目录。每个关系（用户表、系统表，以后的每个索引）对应一组文件：

```
tpch_db/
├── sys_tables.dat   sys_tables.fsm
├── sys_columns.dat  sys_columns.fsm
├── supplier.dat     supplier.fsm
├── lineitem.dat     lineitem.fsm
└── ...              （索引实验：<index>.idx，无 .fsm）
```

数据文件（`.dat`）内部仍是段页式，按区（64 页）增长：

```
区 0（系统保留）                      区 1               区 2
┌────────┬────────┬──────────────┐ ┌───────────────┐ ┌───────────────┐
│ 页 0    │ 页 1    │ 页 2 ~ 63     │ │ 页 64 ~ 127    │ │ 页 128 ~ 191   │ ...
│ 文件头   │ 区位图   │ 预留          │ │ 段头页 + 数据页  │ │ 段的数据页       │
└────────┴────────┴──────────────┘ └───────────────┘ └───────────────┘
```

段（segment）是区的集合，段的第一个区的第一页是段头页。目前一个数据文件里只有一个段（表的堆段）；格式允许一个文件里有多个段（区位图按区分配，段各自维护区列表）。

**为什么从 v0.1 的单文件改为每个关系一个文件**

| 考虑 | 单文件（SQLite、DuckDB） | 每关系一个文件（PostgreSQL；InnoDB 自 5.6.6 起默认 `file_per_table`） |
|---|---|---|
| `DROP TABLE` | 区回收到位图，文件不缩小 | 直接 `unlink`，空间立即还给操作系统 |
| 容量上限 | 区位图限制整个库约 32 GB | 同样的位图只限制单个关系，整库不受限 |
| 故障隔离 | 一处损坏可能影响全库 | 损坏局限在一个关系的文件 |
| FSM 不写日志（11.2） | FSM 页与数据页混在段内，恢复时要区分 | FSM 在独立文件里，恢复实验中可以整体丢弃、按数据页重建（PostgreSQL 的 `_fsm` fork 就是这样做的） |
| 原子的跨表提交 | 天然在一个文件里 | 依赖 WAL；而 WAL 本来就是独立文件，恢复实验无论如何都要实现 |
| 段页式要求 | 满足 | 满足：文件 → 区位图 → 段的区列表 → 页掩码 → 页 |

对本课程而言，每关系一个文件更简单（DDL 不需要在共享文件里协调空间），也和主流系统一致；代价是每个关系至少占用约 1.5 MB（数据文件的区 0 与第一个段区各 512 KB，FSM 文件 512 KB），10 张表约 15 MB，可以接受。

### 5.2 通用页头（所有页共用，32 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `checksum` | CRC32C，计算时该字段视为 0 |
| 4 | 2 | `page_type` | 见下表 |
| 6 | 2 | `flags` | 预留 |
| 8 | 4 | `page_id` | 页的自身编号，读入时校验 |
| 12 | 4 | `segment_id` | 所属段，系统页为 0；FSM 页为所属关系的 table_id |
| 16 | 8 | `page_lsn` | 最后修改本页的日志 LSN；实验一恒为 0 |
| 24 | 8 | 预留 | |

页类型：

| 值 | 类型 |
|---|---|
| 1 | `FILE_HEADER` |
| 2 | `EXTENT_BITMAP` |
| 3 | `SEGMENT_HEADER` |
| 4 | `EXTENT_LIST`（段头区列表溢出页） |
| 5 | `FSM` |
| 6 | `HEAP` |
| 7 | `BTREE_INTERNAL`（预留） |
| 8 | `BTREE_LEAF`（预留） |

校验规则：DiskManager 写页时对页的副本计算校验和后写出（不修改缓冲帧，见 8.6）；读页后校验校验和与 `page_id`，校验时把校验和字段视为 0，不复制整页。全零页视为"已分配未写入"的合法页（为恢复实验预留此语义；FSM 页遇到全零页的处理见 5.6）。

CRC32C 在支持 SSE4.2 的 x86-64 CPU 上使用 `crc32` 指令（运行时探测，代码按 `target("sse4.2")` 单独编译，不需要 `-msse4.2`，因此在任何机器上都能运行），否则回退到查表实现。硬件实现比逐字节查表快一个数量级以上；在 200 万行的合成导入中，全表扫描时间从 861 ms 降到 221 ms。

### 5.3 文件头页（页 0）

数据文件和 FSM 文件使用同一种文件头：

| 偏移 | 长度 | 字段 |
|---|---|---|
| 32 | 8 | `magic` = `"MINIDBF2"` |
| 40 | 4 | `format_version` = 2 |
| 44 | 4 | `page_size` = 8192 |
| 48 | 4 | `pages_per_extent` = 64 |
| 52 | 4 | `file_extents`（文件当前包含的区数） |
| 56 | 4 | `file_kind`：1 = 数据文件，2 = FSM 文件 |
| 60 | 4 | `table_id`（所属关系） |
| 64 | 4 | `segment_header_page`（数据文件：段头页号；FSM 文件：无效） |
| 68 | 4 | `fsm_page_count`（FSM 文件：已初始化的 FSM 页数） |
| 72 | 1 | `clean_shutdown` |

v0.1 中的全局计数器（`next_table_id` 等）不再放在文件头：`next_table_id` 打开数据库时由 `sys_tables` 中的最大 id 推出；`next_txn_id` 与 `checkpoint_lsn` 留给恢复实验，届时放在日志文件的控制块中（与 PostgreSQL 的 `pg_control` 类似）。

文件扩展（包括创建文件时的第一个区）：优先用 `fallocate` 为新区预留真实磁盘块（文件不是稀疏的，碎片更少，磁盘满时在扩展时就报错，而不是在之后某次写页时才报错）；文件系统不支持时回退到 `ftruncate`。

### 5.4 区位图页（数据文件的页 1）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 32 | 4 | `alloc_hint`（下次分配从此位开始找） |
| 36 | 4 | 预留 |
| 40 | 8152 | 位图，每位对应一个区（1 = 已分配），共 65216 位 |

可管理 65216 × 512 KB ≈ 31.8 GB，作为**单个关系**的上限。

### 5.5 段头页

段头页位于段第一个区的第一页（该页在页掩码中标记为已用）。

**段元信息（192 字节）**

| 字段 | 长度 | 说明 |
|---|---|---|
| `segment_id` | 4 | |
| `segment_type` | 2 | 1 = HEAP，2 = INDEX（预留） |
| `reserved` | 2 | |
| `extent_slots` | 4 | 区列表中已使用的槽位数（含空洞） |
| `extent_count` | 4 | 实际拥有的区数 |
| `page_count` | 4 | 已分配页数 |
| `overflow_page` | 4 | 区列表溢出页链表头 |
| `insert_hint` | 4 | 上次插入的段内页号，FSM 搜索起点 |
| 其余 | | 预留（v0.1 的 `fsm_page_count`、`fsm_pages[32]` 已移到 FSM 文件，见 5.6） |

**区列表项（每项 16 字节）**

| 字段 | 长度 | 说明 |
|---|---|---|
| `extent_id` | 4 | `kInvalid` 表示空洞 |
| `reserved` | 4 | |
| `used_mask` | 8 | 64 位页掩码，1 = 该页已分配 |

段头页内可放 (8160 − 192) / 16 = 498 项，约 249 MB；超出部分放入溢出页（`EXTENT_LIST` 类型，页体偏移 32 为 `next`、36 为本页项数，40 起为区列表项，每页 509 项，链表连接）。溢出页从段自身分配。

**段内页号（spno）**：`spno = 区列表下标 × 64 + 区内偏移`。FSM 以 spno 为下标。为保证 spno 稳定，**区被回收时在列表中留下空洞（extent_id 置为无效），以后新分配的区优先填入空洞，列表不做压缩**。

### 5.6 FSM 文件

每个关系的 FSM 存放在独立的 `<name>.fsm` 文件中（与 PostgreSQL 的 FSM fork 相同的思路）：页 0 是文件头（5.3），页 `1 + i` 是第 i 个 FSM 页。FSM 页的页体为 8160 个 `uint8_t`，第 j 项对应 `spno = i × 8160 + j` 的数据页：

- 值 = `可用字节数 / 32`（0 ~ 254）；
- 非堆页（段头页、溢出页）、未分配的页、空洞区里的页，值都为 0。

FSM 页按需创建（`EnsureCapacity`），数量没有上限，因此单段大小只受区位图限制（约 31.8 GB），不再是 v0.1 的 32 个 FSM 页 ≈ 2 GB。

FSM 只是提示：

- 修改不写日志；
- 读到全零的 FSM 页（文件头已记录该页、但页本身崩溃前没有刷盘）时当作"没有空闲空间"，写入前重新初始化页头，而不是报损坏。这只是容错：这些数据页的空闲空间要等它们被删除或更新时才会重新记入 FSM。打开时 `clean_shutdown == 0` 的关系按段扫描数据页、重建 FSM 的功能留给恢复实验；
- 内存中为每个 FSM 页维护一个"最大类别"的上界（不落盘）：一次完整扫描找不到合适页时记下该页的真实最大值，之后 `Update` 只会把它调高。搜索时上界小于所需类别的 FSM 页直接跳过，大表上找不到空闲页时不必每次扫描整个 FSM。

### 5.7 堆页（slotted page）

```
0        32         44                free_lower      free_upper          8192
┌────────┬──────────┬────────────────┬──────────────┬────────────────────┐
│ 通用页头 │ 堆页头     │ 槽数组 →         │   空闲区       │        ← 记录区      │
└────────┴──────────┴────────────────┴──────────────┴────────────────────┘
```

**堆页头（12 字节）**

| 字段 | 长度 | 说明 |
|---|---|---|
| `slot_count` | 2 | 槽数组长度（含空槽） |
| `free_lower` | 2 | 槽数组末尾偏移 |
| `free_upper` | 2 | 记录区起始偏移 |
| `frag_bytes` | 2 | 删除和缩短产生的碎片字节数 |
| `live_count` | 2 | 有效记录数 |
| `reserved` | 2 | |

**槽（4 字节）**

| 位 | 含义 |
|---|---|
| `offset`：16 位 | 记录在页内的偏移 |
| `length`：13 位 | 记录长度（最大 8191） |
| `flags`：3 位 | `USED`、`FORWARD`（本槽存放转发指针）、`MOVED_IN`（本记录由其他页转发过来） |

`flags == 0` 表示空槽，可被复用。

可用空间：`连续空闲 = free_upper − free_lower`，`总可用 = 连续空闲 + frag_bytes`。FSM 记录"总可用"；插入时若连续空闲不足而总可用足够，先做页内整理（compaction）。

### 5.8 容量估算

| 项目 | 数值 |
|---|---|
| 单个关系（数据文件）上限 | 约 31.8 GB（区位图 65216 个区） |
| 单段上限 | 同上（FSM 页数不再限制段大小） |
| 每个关系的最小占用 | 约 1.5 MB（数据文件 2 个区 + FSM 文件 1 个区） |
| 单条记录上限 | 约 8100 字节 |
| supplier 平均记录 | 约 170 字节，约 47 条/页，10000 条约 215 页（1.7 MB） |
| TPC-H SF1 全部表 | 约 1.2 GB 数据文件 |

### 5.9 I/O 方式的选择

- **自管缓冲池，不用 `mmap`**：用 `mmap` 时操作系统决定何时换出、何时写回页面，无法保证 WAL 规则（11.3），缺页时还会在任意位置阻塞，并有 TLB shootdown 开销；见 Crotty、Leis、Pavlo，"Are You Sure You Want to Use MMAP in Your Database Management System?"（CIDR 2022）。PostgreSQL、InnoDB 和 LeanStore 都自管缓冲池。
- **`pread`/`pwrite` + 内核页缓存**：实验一的规模（SF1 约 1 GB）基本能放进页缓存，双重缓存的代价可以接受，实现也最简单。
- **`O_DIRECT`、io_uring 暂不引入**：两者都要求自己处理对齐、预读和批量提交，收益要在数据远大于内存、而且 I/O 并发很高时才显现；并发实验之后如果有需要，只改 DiskManager 即可（缓冲池通过 `ReadPage`/`WritePage` 访问磁盘，接口不变）。

---

## 6. 类型系统与记录格式

### 6.1 类型映射

| SQL 类型 | 内部类型 | 存储长度 | 说明 |
|---|---|---|---|
| `INTEGER` / `INT` | `INT32` | 4 | |
| `LONG` / `BIGINT` | `INT64` | 8 | |
| `FLOAT` | `DOUBLE` | 8 | **SQL 标准中 FLOAT 默认为双精度**；TPC-H 价格字段有 8 位有效数字，单精度会丢失精度 |
| `DOUBLE` | `DOUBLE` | 8 | |
| `DATE` | `DATE` | 4 | 自 1970-01-01 起的天数（int32） |
| `CHAR(n)` | `CHAR` | n | 定长，不足用空格填充，输出时去掉尾部空格 |
| `VARCHAR(n)` | `VARCHAR` | 变长 | 写入时检查字节长度 ≤ n |

字符串按字节处理，不做字符集转换；TPC-H 数据均为 ASCII。

浮点数输出采用能精确读回原值的最短十进制串（`std::to_chars`，与 PostgreSQL 12 起的默认行为一致），因此 `4192.40` 显示为 `4192.4`。浮点数不保存原文的小数位数，不按固定两位小数输出，否则 `0.001` 会变成 `0.00`；需要固定小数位的金额应声明为 `DECIMAL(15,2)`，这也是 TPC-H 规范本身的类型。

### 6.2 Schema

```cpp
struct Column {
  std::string name;
  TypeId      type;
  uint16_t    length;      // CHAR/VARCHAR 的 n，其余为 0
  bool        nullable;
};

class Schema {
  std::vector<Column> columns_;
  // 派生信息（构造时计算）：
  std::vector<uint16_t> fixed_offset_;   // 定长列在定长区的偏移
  std::vector<uint16_t> var_index_;      // 变长列的序号
  uint16_t fixed_size_;
  uint16_t null_bitmap_size_;
};
```

### 6.3 记录布局

```
┌──────────────────── 记录头 18 字节 ──────────────────┐
│ txn_id (8) │ undo_ptr (8) │ info (2)                 │
├─────────────────────────────────────────────────────┤
│ null 位图 ceil(列数 / 8) 字节                          │
├─────────────────────────────────────────────────────┤
│ 定长区：所有定长列按 schema 顺序紧密排列                   │
├─────────────────────────────────────────────────────┤
│ 变长偏移表：每个 VARCHAR 一个 uint16，记录该列的结束偏移     │
├─────────────────────────────────────────────────────┤
│ 变长数据区                                            │
└─────────────────────────────────────────────────────┘
```

- `txn_id`：最后修改该记录的事务；实验一写入当前事务 id=0；
- `undo_ptr`：指向 undo 记录；实验一为 0；
- `info`：位 0 = `DELETED`（逻辑删除标记，供 2PL/MVCC 使用），其余预留；
- 不做内存对齐，所有字段读写统一用 `memcpy`，避免未对齐访问的未定义行为；
- NULL 值的定长列仍占位（内容为 0），变长列长度为 0。

### 6.4 转发指针

当更新后的记录在原页放不下时：

1. 在其他页插入新版本，该槽标记 `MOVED_IN`；
2. 原槽改为 `FORWARD`，记录内容替换为 6 字节的目标 RID（`page_id` + `slot`）；
3. 原 RID 保持不变，所有外部引用（以后的索引、锁、undo）继续有效。

规则：

- 转发链最多一跳。再次更新且目标页放不下时，在第三个位置插入，更新原槽的转发指针，删除旧目标；
- 通过 RID 访问时遇到 `FORWARD` 自动跟随一次；
- 顺序扫描时**跳过 `MOVED_IN` 记录**，遇到 `FORWARD` 时跟随并以原 RID 输出，保证每条记录恰好输出一次；
- 记录最小长度（18 字节）大于转发指针（6 字节），因此原槽总能容纳转发指针。

---

## 7. 空闲空间管理

### 7.1 三层结构

| 层级 | 数据结构 | 分配单位 | 位置 |
|---|---|---|---|
| 文件 → 段 | 区位图 | 区（512 KB） | 页 1 |
| 段 → 页 | 区列表 + 64 位页掩码 | 页（8 KB） | 段头页及溢出页 |
| 页 → 记录 | FSM + 页内槽目录 | 字节 | FSM 页、堆页 |

### 7.2 区的分配与回收（SpaceManager）

**分配 `AllocateExtent()`**

1. 从 `alloc_hint` 开始扫描区位图，找第一个为 0 的位；
2. 置位，更新 `alloc_hint`；
3. 若区号 ≥ `file_extents`，扩展文件（`ftruncate` 到新长度）并更新文件头；
4. 记录日志 `[SPACE] alloc extent=N`。

**回收 `FreeExtent(id)`**：清位，若 `id < alloc_hint` 则更新 `alloc_hint`。文件不缩小。

### 7.3 页的分配与回收（Segment）

**分配 `AllocatePage()`**

1. 从上次成功分配的区列表下标开始（内存中的 `alloc_hint_slot`，不落盘）循环遍历区列表，找 `used_mask != ~0` 的有效项；
2. 找到则取最低的 0 位，置位，`page_count++`；
3. 都满则 `AllocateExtent()`，填入第一个空洞（这些 spno 的 FSM 项在页被回收时已清零），没有空洞才追加到列表末尾；
4. 段元信息写回段头页（及溢出页）；返回物理页号。新页由 BufferPool 直接在内存中初始化，不从磁盘读。

**回收 `FreePage(page_id)`**

1. 段头页和溢出页不能通过 `FreePage` 回收；
2. 清对应位，`page_count--`，FSM 项置 0，从缓冲池丢弃该页的帧；
3. 若该区 `used_mask == 0` 且不是段的第一个区，调用 `FreeExtent`，列表项置为空洞。`FreeExtent` 对已经空闲的区报错（防止重复回收）。

**删除段 `DropSegment()`**：回收所有区（含溢出页所在区），为以后的 `DROP TABLE` 预留。

### 7.4 记录级空间（FSM + 页内管理）

**插入时找页**

1. `need = 记录长度 + 4（槽，若无空槽可复用）`，`need_cat = ceil(need / 32)`；
2. 从 `insert_hint` 开始顺序扫描 FSM，找第一个 `cat ≥ need_cat` 的页；到末尾后回绕一次，最后扫描起始 FSM 页中 hint 之前的部分（只有一个 FSM 页的表，即小于约 64 MB 的表，也必须扫这一段，否则 hint 之前释放的空间永远不会被复用）。最大类别上界小于 `need_cat` 的 FSM 页整页跳过（5.6）；
3. 找不到则 `AllocatePage()` 新建堆页；
4. 在页内插入：优先复用空槽；连续空闲不足但总可用足够时先整理；
5. 用页的实际可用空间更新 FSM，更新 `insert_hint`。

**FSM 自我修正**：FSM 只是提示。如果 FSM 显示某页够用而实际不够（例如崩溃后 FSM 落后于数据页），插入失败时用实际值修正该项后继续搜索。因此 FSM 修改不需要写日志，这与 PostgreSQL 的处理方式一致。

**删除**

1. 实验一中删除立即生效：槽置空，`frag_bytes += 长度`，`live_count--`，更新 FSM；
2. 若 `live_count == 0`，调用 `FreePage` 回收整页。`FORWARD` 槽计入 `live_count`，所以条件里已经包含"页中没有转发槽"。段的第一个堆页（段头页之后的那一页）保留，避免表在空与非空之间反复变化时频繁地分配、回收页。

**页内整理（compaction）**：把有效记录按偏移从高到低依次搬到页尾，重算 `free_upper`，`frag_bytes = 0`。槽号不变，因此 RID 不变。

### 7.5 后续实验需要注意的点

- 有事务以后，删除分两步：事务中只设置记录头的 `DELETED` 标记（逻辑删除），提交时才真正释放空间；中止时清除标记即可。这样回滚不需要重新申请空间，也避免 RID 在事务结束前被复用；
- 区位图、段头页、区列表的修改必须写日志（第 11 章），FSM 不写。

---

## 8. 缓冲区管理

### 8.1 数据结构

```cpp
struct Frame {
  page_id_t          page_id = kInvalidPageId;
  std::atomic<int>   pin_count{0};
  bool               dirty = false;
  std::shared_mutex  latch;               // 页锁存，实验一单线程下也正常加锁
  alignas(4096) std::byte data[kPageSize];
};

class BufferPool {
  std::vector<Frame>                           frames_;
  std::unordered_map<page_id_t, frame_id_t>    page_table_;
  std::list<frame_id_t>                        free_list_;
  std::unique_ptr<Replacer>                    replacer_;
  std::mutex                                   mutex_;   // 保护 page_table_、free_list_、replacer_
  BufferStats                                  stats_;
};
```

### 8.2 查找与读入 `Fetch(page_id)`

1. 加 `mutex_`，查 `page_table_`；
2. **命中**：`pin_count++`，通知 replacer 访问，`stats.hit++`；
3. **未命中**：
   1. 从 `free_list_` 取空帧；没有则 `replacer_->Evict()` 选牺牲帧（只会选 `pin_count == 0` 的帧），都不可用则返回错误「缓冲池已满」。若随后读盘失败，该帧放回 `free_list_`，不计入 `evict`；
   2. 牺牲帧若为脏页：调用 WAL 钩子 `log_->FlushUntil(page_lsn)`（实验一为空操作），然后写回磁盘，`stats.flush++`；
   3. 从 `page_table_` 删除旧映射，插入新映射；
   4. 从磁盘读入新页，校验校验和与 `page_id`；
   5. `pin_count = 1`，`stats.miss++`，若发生淘汰则 `stats.evict++`；
4. 释放 `mutex_` 后再获取页锁存（读或写），**持有 `mutex_` 时不等待页锁存**。

实验一中磁盘 I/O 在 `mutex_` 内进行，实现最简单；并发实验时再改为"先占位、释放全局锁、再做 I/O"。

### 8.3 淘汰策略

```cpp
class Replacer {
 public:
  virtual void RecordAccess(frame_id_t) = 0;
  virtual void SetEvictable(frame_id_t, bool) = 0;   // pin_count 变为 0 时可淘汰
  virtual std::optional<frame_id_t> Evict() = 0;
  virtual void Remove(frame_id_t) = 0;
  virtual ~Replacer() = default;
};
```

- **CLOCK（默认）**：每帧一个引用位，访问时置 1；时钟指针扫描，遇到 1 清为 0 继续，遇到可淘汰且为 0 的帧则淘汰。实现简单，PostgreSQL 使用类似算法；
- **LRU-2（可选）**：按倒数第二次访问时间淘汰，访问不足两次的帧优先淘汰。对顺序扫描大表有抵抗力，可在验收时与 CLOCK 对比命中率。

### 8.4 PageGuard

```cpp
class ReadPageGuard {    // 持有 pin + 共享锁存
 public:
  const std::byte* Data() const;
  page_id_t PageId() const;
  ~ReadPageGuard();      // 释放锁存，Unpin
  // 仅可移动，不可复制
};

class WritePageGuard {   // 持有 pin + 独占锁存
 public:
  std::byte* MutableData();   // 调用即标记脏页
  ~WritePageGuard();
};
```

所有模块只通过 Guard 访问页，禁止裸指针跨函数保存。Guard 析构时自动 unpin，杜绝"漏 unpin 导致缓冲池耗尽"一类问题。

### 8.5 新页 `NewPage(segment)`

1. 由 `Segment::AllocatePage()` 得到页号（该过程本身会通过缓冲池修改段头页）；
2. 在缓冲池中取帧但不读磁盘，清零并写入通用页头；
3. 返回 `WritePageGuard`，帧标记为脏。只计 `new_page`，不计 `miss`（没有读盘）。

### 8.6 刷盘

- `FlushPage(id)`：写回单页（若脏）；
- `FlushAll()`：写回全部脏页，`fsync`；
- 实验一的刷盘时机：淘汰时、关闭时、shell 的 `flush` 命令；
- `DiskManager::WritePage` 接收 `const std::byte*`，在一个 8 KB 的栈上副本里计算校验和后写出，刷盘不会修改帧内容；
- 已知限制（并发实验处理）：`FlushPage`/`FlushAll` 在持有 `mutex_` 时直接写出被 pin 的帧，不获取页锁存，并清除脏标记。单线程下没有问题；多线程下可能写出写了一半的页，或者在写者仍持有 `WritePageGuard` 时清掉脏标记。届时改为对被 pin 的帧 `try_lock_shared`，拿不到锁存就跳过、保留脏标记。

### 8.7 统计

`BufferStats` 记录 `hit`、`miss`、`evict`、`flush`、`new_page`，提供命中率；shell 的 `stats` 命令输出，导入结束时自动打印。

---

## 9. 表堆（TableHeap）

### 9.1 接口

```cpp
class TableHeap {
 public:
  Result<RID>   Insert(Transaction*, const Tuple&);
  Status        Delete(Transaction*, RID);
  Result<RID>   Update(Transaction*, RID, const Tuple&);  // 返回值恒等于传入 rid
  Result<Tuple> Get(Transaction*, RID);
  TableIterator Scan(Transaction*);
};
```

### 9.2 操作流程

**Insert**：编码记录 → FSM 找页（第 7.4 节）→ 页内插入 → 更新 FSM → 返回 RID。

**Get**：取页（读锁存）→ 检查槽 → 若为 `FORWARD`，释放后跟随到目标页 → 解码。

**Update**（先通过原 RID 解析到记录当前所在位置：原槽，或其转发目标）

1. 新记录 ≤ 原记录：原地覆盖，差值计入 `frag_bytes`；
2. 新记录 > 原记录且本页 `总可用 + 原长度 ≥ 新长度`：`HeapPage::Replace` 先把旧记录计入碎片并暂时清空该槽，必要时整理，再写入新记录，**槽号和槽标志不变**（转发目标上的 `MOVED_IN` 也保留）；
3. 否则：新版本以 `USED | MOVED_IN` 插入其他页；若原槽已经是转发槽，删除旧目标；原槽改为（或重定向为）指向新位置的转发指针。转发链始终最多一跳。

直接对 `MOVED_IN` 槽的 RID 做 `Update`/`Delete` 会被拒绝（返回 `InvalidArgument`），因为这样会留下悬空的转发指针；用户可见的只有原 RID（扫描不会输出 `MOVED_IN` 槽）。`ResolveForward` 发现两跳以上的链或悬空的转发指针时返回 `Corruption`。

**Delete**：若为 `FORWARD`，先删目标再删原槽；更新 FSM；按第 7.4 节决定是否回收整页。

**Scan**：按区列表顺序遍历已分配页，只处理 `HEAP` 类型页；页内按槽号顺序输出，跳过空槽和 `MOVED_IN`，跟随 `FORWARD`。迭代器每次只持有一个页的读 Guard。读页失败、记录解析失败等错误不会被静默跳过：`Next` 返回 `false`，调用方必须检查 `iterator.status()`。

扫描期间不能修改正在扫描的表（迭代器持有页的读锁存，同一线程再取写锁存会死锁）；`UPDATE`/`DELETE` 先扫描收集匹配的 RID（以及旧元组），再逐条修改。这样也避免了 Halloween 问题，即被移动到后面页的记录在同一次扫描中被再次匹配。

---

## 10. 系统目录（Catalog）

### 10.1 系统表

两张系统表本身就是普通的堆表，Schema 硬编码在代码中，文件名固定为 `sys_tables.dat/.fsm`、`sys_columns.dat/.fsm`，段头页号记录在各自数据文件的文件头：

```
sys_tables (table_id INT, name VARCHAR(64), data_file VARCHAR(128), fsm_file VARCHAR(128),
            segment_header_page INT, column_count INT, is_system INT)
sys_columns(table_id INT, ordinal INT, name VARCHAR(64), type INT, length INT, scale INT, nullable INT)
```

`scale` 用于 `DECIMAL(p,s)`（`length` 存精度 p）。系统表对 SQL 只读。

以后的索引实验增加 `sys_indexes`，同样方式引导。

### 10.2 接口

```cpp
class Catalog {
 public:
  Result<TableInfo*> CreateTable(Transaction*, std::string name, Schema);
  Result<TableInfo*> GetTable(std::string_view name);
  Status             DropTable(Transaction*, std::string_view name);  // 预留
};

struct TableInfo {
  uint32_t                    table_id;
  std::string                 name;
  Schema                      schema;
  std::unique_ptr<TableHeap>  heap;
};
```

打开数据库时扫描系统表构建内存中的 `TableInfo`。元数据存成普通表的好处是：恢复实验中元数据的持久化与恢复不需要额外的机制。

---

## 11. 事务、并发与恢复的预留设计

本章描述的内容实验一**只实现接口和字段**，行为在后续实验中补全。

### 11.1 事务管理器的实验一形态

```cpp
class Transaction {
  txn_id_t id_;
  TxnState state_;   // RUNNING / COMMITTED / ABORTED
  // 以后：持有的锁集合、undo 链头、读快照
};

class TxnManager {
 public:
  Transaction* Begin();
  void Commit(Transaction*);   // 实验一：只改状态
  void Abort(Transaction*);    // 实验一：不支持，直接报错
};
```

shell 中每条命令自动包在一个事务里执行（自动提交）。所有 TableHeap 接口从实验一起就要求传入 `Transaction*`。

### 11.2 统一的页修改路径

所有修改页内容的函数（堆页插入/删除/更新/整理、区位图置位、段头页修改、以后的 B+ 树节点操作）都遵守同一个模式：

```cpp
// 示意
Status HeapPageInsert(WritePageGuard& page, ByteView record, WriteCtx& ctx, slot_id_t* out) {
  // 1. 生成日志记录（实验一中 ctx.log->Append 返回 kInvalidLsn，几乎零开销）
  lsn_t lsn = ctx.log->Append(LogRecord::HeapInsert(ctx.txn->Id(), page.PageId(), ...));
  // 2. 修改页
  ...
  // 3. 设置 page_lsn
  if (lsn != kInvalidLsn) SetPageLsn(page, lsn);
  return Status::Ok();
}
```

`WriteCtx` 包含 `Transaction*` 和 `LogManager*`。只要所有修改都经过这类函数，恢复实验只需实现 LogManager 和各类日志记录的 redo/undo，不需要改调用方。

日志覆盖范围（恢复实验时实现）：

| 对象 | 是否记日志 | 原因 |
|---|---|---|
| 堆页记录操作 | 是 | 数据本身 |
| 区位图、段头页、区列表 | 是 | 丢失会导致空间泄漏或重复分配 |
| B+ 树节点（含分裂合并） | 是 | |
| FSM | 否 | 只是提示，可自我修正或重建 |
| 文件头的计数器 | 是（或在检查点时持久化） | |

### 11.3 缓冲池与 WAL 的约束

采用 steal / no-force 策略：

- **steal**：未提交事务修改过的脏页可以被淘汰写盘，因此需要 undo；
- **no-force**：提交时不强制刷数据页，只刷日志，因此需要 redo；
- **WAL 规则**：脏页写盘前，日志必须已持久化到该页的 `page_lsn`。实现位置即第 8.2 节的 WAL 钩子。

### 11.4 并发控制路线

| 阶段 | 方案 | 对存储格式的依赖 |
|---|---|---|
| 并发实验基础版 | 严格两阶段锁（S2PL），行级锁 + 表级意向锁；死锁检测用等待图或超时 | RID 稳定（已满足） |
| 可选升级 | MVCC：堆中存最新版本，旧版本在 undo 中；读操作沿 `undo_ptr` 链找可见版本 | 记录头 `txn_id`、`undo_ptr`（已预留） |

### 11.5 latch 与 lock 的区分

| | latch（锁存） | lock（事务锁） |
|---|---|---|
| 保护对象 | 内存数据结构（页、页表） | 逻辑数据（记录、表） |
| 持有时间 | 一次页操作 | 到事务结束 |
| 实现位置 | `Frame::latch`，通过 PageGuard 获取 | LockManager |
| 死锁处理 | 靠加锁顺序避免 | 检测并中止事务 |

锁存加锁顺序约定：

1. 持有缓冲池 `mutex_` 时不得等待任何页锁存；
2. 段头页先于该段的数据页加锁存；
3. 不同时持有 FSM 页和堆页的写锁存（先更新堆页、释放后再更新 FSM）；
4. B+ 树按自顶向下的顺序加锁存（latch crabbing）。

### 11.6 恢复

计划实现简化版 ARIES：分析 → redo（物理逻辑日志，按 `page_lsn` 判断是否需要重做）→ undo（逻辑 undo，写补偿日志 CLR）；检查点为模糊检查点。入口位于 `Database::Open` 的第 2 步。

---

## 12. 后续实验的接入点

| 实验 | 新增模块 | 复用的现有接口 | 对现有代码的改动 |
|---|---|---|---|
| 索引 | `index/BPlusTree`，`sys_indexes`；每个索引一个 `<index>.idx` 文件（与 PostgreSQL 相同，B+ 树自己管理空闲页，不需要 `.fsm`） | DiskManager、SpaceManager、Segment（类型 INDEX）、BufferPool、PageGuard、RID | Catalog 增加索引元数据；执行层在增删改时同步维护索引 |
| 查询处理 | `parser/`、`binder/`、`planner/`、`execution/`（火山模型：SeqScan、IndexScan、Filter、Projection、Join、Aggregate、Sort） | TableHeap::Scan/Get、Catalog | PRAGMA 升级为系统视图或表函数（13.2） |
| 并发控制 | `concurrency/LockManager`、多线程测试框架 | Transaction、RID、页锁存 | 缓冲池 I/O 移出全局锁；刷盘对被 pin 的帧加读锁存（8.6）；删除改为逻辑删除 + 提交时物理删除 |
| 故障恢复 | `log/LogManager` 实装、日志记录类型、`recovery/` | 统一页修改路径、page_lsn、WAL 钩子、`clean_shutdown` | 无结构性改动 |

查询处理相关的选择（解析器手写还是引入库、火山模型还是向量化）对存储层没有约束，到该实验时再决定。

---

## 13. 可观测性：日志与统计

可观测性分成两类，和 PostgreSQL 一样：**日志**记录"发生了什么"（相当于 `log_*` 配置项），**PRAGMA 快照**展示"现在是什么状态"（相当于 `pg_stat_*` 视图和 pageinspect 函数）。验收时两种都要用到。

### 13.1 日志

```
[时间戳][级别][模块] 事件 key=value ...
```

输出示例（取自 supplier 演示，有删节）：

```
[01:43:20.038][DEBUG][BUF] EVICT frame=6 file=part.dat pid=64 dirty=0
[01:43:20.038][DEBUG][BUF] MISS file=supplier.dat pid=282 -> frame=6
[01:43:20.038][DEBUG][FSM] file=supplier.fsm spno=218 cat 133 -> 127
[01:43:20.038][DEBUG][HEAP] INSERT rid=(282,21) len=202
[01:43:20.038][DEBUG][HEAP] UPDATE rid=(65,0) -> FORWARD (282,21)
[01:43:20.202][DEBUG][HEAP] DELETE rid=(130,4) page now empty
[01:43:20.202][DEBUG][SPACE] FREE file=supplier.dat page=130 seg=6
[01:43:20.202][INFO ][LOAD] lineitem rows=100000 pages=1728 hit_rate=99.98%
```

模块：`DISK`、`BUF`、`SPACE`、`FSM`、`HEAP`、`CAT`、`LOAD`、`DB`。

级别使用约定：

- `DEBUG`：缓冲区未命中、淘汰、新页、刷盘（命中只计数，不逐条打印，否则一次导入会输出几十万行）；区、页的分配与回收；FSM 类别变化；记录的增删改。用于 supplier 演示；
- `INFO`：`COPY` 每导入 10 万行输出一次进度，结束时输出摘要。用于 SF1 大规模导入；
- `WARN` / `ERROR`：异常情况。

日志器是唯一允许的全局对象，支持同时输出到 stderr 和文件，级别可用 `PRAGMA log_level = debug` 动态修改。日志宏在级别关闭时只做一次比较，不构造、不格式化消息（与 glog 的 `LOG_IF` 写法相同）。导入路径上每行都有 DEBUG 日志调用，短路之后，200 万行的合成导入从约 77 万行/秒提高到约 91 万行/秒。

### 13.2 PRAGMA：以结果集形式查看引擎状态

主流系统都把引擎内部状态暴露成 SQL 可查询的结果集，而不是做成客户端专用的打印命令：SQLite 用 `PRAGMA` 和 `dbstat` 虚拟表，PostgreSQL 用 `pg_stat_*`、`pg_buffercache` 视图和 pageinspect 的 `heap_page_items(get_raw_page(...))`，MySQL 用 `information_schema.INNODB_BUFFER_PAGE`，DuckDB 用 `PRAGMA storage_info`。这样做的好处是：

- 任何客户端都能用：shell、嵌入式 C API、测试都通过 `mydb_prepare`/`mydb_step` 读取，C API 不用为每个功能增加函数；
- 结果是结构化的行，集成测试可以直接断言；
- 查询处理实验之后可以自然升级为真正的系统视图或表函数，支持 `WHERE` 和 `JOIN`。

MiniDB 的解析器没有表函数，因此采用 SQLite 风格的 `PRAGMA name[(args)] [= value]`（实现在 `src/db/pragma.cpp`）：

| PRAGMA | 返回 | 对应的数据结构 |
|---|---|---|
| `pragma_list` | 所有 PRAGMA 及说明 | |
| `buffer_stats` | 帧数、已用、pin、脏页、hit/miss/命中率、evict/flush/new_page | 第 8 章 |
| `buffer_frames` | 每个已用帧：文件、页号、页类型、pin 数、脏标记 | 帧与页表 |
| `table_info(t)` | 文件与大小、段头页、区数、区列表长度、页数、行数、转发数、空槽、空闲字节、填充率、FSM 页数 | 5.1、5.5 |
| `extents(t)` | 区列表：下标、区号、首页、已用页数、64 位页掩码（`H` 段头页，`1` 已分配，`.` 空闲；空洞显示 `(hole)`） | 5.5、7.3 |
| `extent_bitmap(t)` | 数据文件的区位图，每行 64 个区，以及 `alloc_hint` | 5.4、7.2 |
| `free_space(t)` | 每个堆页：spno、FSM 类别、FSM 记录的空闲字节与实际空闲字节、行数 | 5.6、7.4 |
| `free_space_summary(t)` | FSM 空闲空间直方图（1 KB 一档） | 5.6 |
| `page_header(t, p)` | 通用页头与堆页头字段、FSM 类别 | 5.2、5.7 |
| `page_slots(t, p)` | 槽目录：偏移、长度、标志（`USED`/`FORWARD`/`MOVED_IN`/`EMPTY`）、转发目标 | 5.7、6.4 |
| `flush` | 写回所有脏页并 `fsync` | 8.6 |
| `log_level [= l]` | 查看或设置日志级别 | 13.1 |

另外，`rid` 是一个伪列，写作 `"页号:槽号"`（类似 PostgreSQL 的 `ctid`）：`SELECT rid, * FROM t LIMIT 5`，`SELECT * FROM t WHERE rid = '65:0'`。按 RID 访问会跟随转发指针，`MOVED_IN` 槽和已回收页上的 RID 不可见（返回空结果）。

---

## 14. 工具：shell 与批量导入

### 14.1 设计原则

数据库 CLI 的通行做法（sqlite3、psql、mysql、duckdb）是两层分工：

1. **引擎能力一律是 SQL**：导入（PostgreSQL/DuckDB 的 `COPY`，MySQL 的 `LOAD DATA`）、统计与内部结构（13.2）、配置（`SET`/`PRAGMA`）。嵌入式使用者、测试和 shell 走同一条路径；
2. **客户端元命令**只做纯客户端的功能（计时、回显、读脚本、帮助），或者作为 SQL 的简写。元命令有前缀（sqlite3 用 `.`，psql 用 `\`）、按行解析、不需要分号，客户端在交给 SQL 解析器之前就能区分它们。

v0.1 设计的 `load`、`scan`、`dump` 这类裸命令和 SQL 混在同一个输入流里，会产生歧义，因此改为上面的方式。

### 14.2 SQL 入口

```sql
CREATE TABLE ... ;  DROP TABLE ... ;          -- 能直接执行实验文档 Listing 1 的 DDL
INSERT / SELECT / UPDATE / DELETE             -- WHERE 支持单个比较：= <> < <= > >=
SELECT rid, * FROM t WHERE rid = '65:0';      -- RID 伪列
COPY t FROM 'path.tbl' [DELIMITER '|'];       -- 批量导入（14.4）
PRAGMA name[(args)] [= value];                -- 引擎状态（13.2）
SHOW TABLES;  DESCRIBE t;
```

`UPDATE`/`DELETE` 先全表扫描收集匹配的行，再逐条修改（9.2）。

### 14.3 minidb_shell

```
./build/minidb_shell --db <目录> [--buffer <帧数>] [--log-level <级别>] [--file <脚本>]
```

交互输入、`--file` 和 `.read` 走同一个按行处理的执行器，脚本里可以混写 SQL 和元命令；以 `--` 开头的整行注释在回显模式下会原样打印，脚本可以借此自带讲解。语句以分号结束，字符串和注释里的分号不算。出错的语句会报告错误后继续执行，只要有语句失败，进程退出码就为 1。

| 元命令 | 等价的 SQL / 作用 |
|---|---|
| `.tables`、`.schema t` | `SHOW TABLES`、`DESCRIBE t` |
| `.load t file` | `COPY t FROM 'file' DELIMITER '\|'` |
| `.get t p:s` | `SELECT rid, * FROM t WHERE rid = 'p:s'` |
| `.stats`、`.frames` | `PRAGMA buffer_stats`、`PRAGMA buffer_frames` |
| `.info t`、`.extents t`、`.bitmap t` | `PRAGMA table_info / extents / extent_bitmap` |
| `.fsm t`、`.free t` | `PRAGMA free_space_summary / free_space` |
| `.page t p` | `PRAGMA page_header` + `PRAGMA page_slots` |
| `.flush`、`.log [level]` | `PRAGMA flush`、`PRAGMA log_level [= level]` |
| `.read f`、`.echo on\|off`、`.timer on\|off`、`.help`、`.quit` | 纯客户端 |

### 14.4 COPY（批量导入）

- 在引擎内逐行读取文件，按分隔符切分；dbgen 生成的每行末尾多一个 `|`，字段数比列数多 1 且最后一个字段为空时忽略它；
- 字段按列类型转换（日期 `YYYY-MM-DD` → 天数，`DECIMAL` 按 scale 定点化）；可空列的空字段视为 NULL；
- 整个 `COPY` 是一条语句、一个事务；出错时报告 `文件:行号` 和原因，之前已导入的行保留（实验一没有回滚，恢复实验以后改为整体回滚）；
- 每 10 万行输出一次 INFO 进度，结束时返回摘要：行数、耗时、吞吐、页数变化、文件区数，以及本次导入期间的缓冲池 hit/miss/evict/flush/new_page 和命中率；
- 性能：Release 构建、1024 帧缓冲池，100 万行合成 lineitem 数据导入约 2 秒（约 50 万行/秒），页填充率 99%。

### 14.5 验收演示脚本

`scripts/demo_supplier.txt`（需要先用 dbgen 生成 `test/tpc-h/supplier.tbl`）：

```
rm -rf /tmp/demo_db
./build/minidb_shell --db /tmp/demo_db --buffer 64 --file scripts/demo_supplier.txt
```

步骤见 15.4。

---

## 15. 测试方案

### 15.1 存储层测试（`tests/test_storage.cpp` → `minidb_storage_tests`）

直接调用内部 C++ 接口，自包含的 `CHECK` 断言，构建时开启 ASan/UBSan（`dev` 预设）：

| 测试 | 覆盖点 |
|---|---|
| CRC32C | 硬件与查表实现在各种长度上结果一致；`Extend` 分段计算；标准校验值 |
| Schema | 多个宽 `CHAR` 列累加不回绕，超过 8100 字节的 Schema 被拒绝 |
| 转发 | 变长更新产生转发后扫描不重复、不遗漏；再次更新仍为一跳；缩短；"先收集再删除"全部成功；删空后保留第一个堆页 |
| 页内增长 | 本页有空间时增长不产生转发，其他记录不受影响 |
| FSM 回绕 | hint 之前的页释放出的空间会被复用，不分配新页 |
| 区空洞 | 整区回收后，新分配的区填入空洞，区列表不增长 |
| 重启 + 全零 FSM 页 | 关闭后把 FSM 页清零，重新打开后扫描和插入正常 |
| 缓冲池 | 读盘失败的帧放回空闲链表；4 帧缓冲池在大量淘汰下读写正确 |

### 15.2 集成测试（`tests/test_minidb.cpp` → `minidb_tests`）

通过 C API 进行，缓冲池 8 帧以强制淘汰：

1. 执行 `test/tpc-h/schema.sql`（实验文档 Listing 1 的 DDL，价格字段为 `FLOAT`），检查目录布局与系统表；
2. 多种类型的插入、查询往返（`CHAR`、`VARCHAR`、`FLOAT`、`DECIMAL`、`DATE`、`INTEGER`）；
3. `UPDATE`：页内增长、多列赋值、NOT NULL 与系统表的报错；`DELETE` 带与不带 `WHERE`；
4. 2 万行批量插入，其中 500 行变长更新触发转发，扫描行数不变；
5. `COPY` 导入 dbgen 格式文件（行尾 `|`）；格式错误时报告行号；系统表、不存在的文件报错；
6. `rid` 伪列：扫描得到的 RID 可直接查回（包括被转发的行）；指向区位图页、越界页、非法格式的 RID；
7. 各 PRAGMA 的结果形状与关键值（转发数、段头页标记、页类型），`flush`、`log_level`，未知 PRAGMA 与参数错误；
8. 关闭 → 重新打开 → 再次比对；`DROP TABLE` 删除文件。

另有手动运行的 `minidb_verify_tpch <库目录> <tbl 目录>`（`tests/verify_tpch.cpp`）：把每张表扫描出的每一行按列类型格式化，与 `.tbl` 原文逐行比对（按多重集合，因为扫描顺序取决于页的放置位置）。FLOAT 按数值比较，CHAR 去掉填充空格，其余逐字节比较。

### 15.3 规模测试（实测，课程提供的 SF1 数据）

数据为实验文档 3.4 节的 `benchmark_data.zip`（SF1，8 张表共 8,661,245 行，约 1.0 GB；行尾没有 dbgen 默认的 `|`，部分字段带首尾空格）。Release 构建，1024 帧缓冲池，`scripts/load_tpch.txt`：

| 表 | 行数 | 堆页数 | 区数 | 导入耗时 | 吞吐（行/s） |
|---|---|---|---|---|---|
| region | 5 | 1 | 1 | < 1 ms | |
| nation | 25 | 1 | 1 | < 1 ms | |
| supplier | 10,000 | 212 | 4 | 0.015 s | 66 万 |
| customer | 150,000 | 3,448 | 54 | 0.27 s | 56 万 |
| part | 200,000 | 3,917 | 62 | 0.32 s | 62 万 |
| partsupp | 800,000 | 16,676 | 261 | 1.01 s | 79 万 |
| orders | 1,500,000 | 24,038 | 376 | 2.33 s | 64 万 |
| lineitem | 6,001,215 | 111,265 | 1,739 | 12.6 s | 47 万 |

- 全部导入耗时 17.2 s，进程峰值内存 9 MB（缓冲池 8 MB），数据库目录 1.3 GB；
- lineitem 的区列表有 1,739 项，超过段头页内联的 498 项，走了溢出页路径；
- 重新打开后用 `minidb_verify_tpch` 逐行比对（15.2），8 张表全部一致；
- 冷缓冲池下 lineitem 全表扫描（11 万页）约 2.1 s。

比对中发现并修复了一个输出问题：`ValueToString` 会截掉所有字符串末尾的空格，导致 dbgen 注释字段末尾的空格在输出时丢失（存储本身正确）。现在 VARCHAR 原样输出，字符串比较仍按 PAD SPACE 语义忽略末尾空格。

待补充：随机增删改与内存模型的差分测试。

### 15.4 验收演示脚本

`scripts/demo_supplier.txt`，缓冲池 64 帧，开启 `.echo` 与 `.timer`：

1. `.read test/tpc-h/schema.sql`，`.tables`、`.schema supplier`：每个关系一对 `.dat`/`.fsm` 文件；
2. `.load supplier ...`：COPY 摘要显示页数、区数、缓冲池淘汰与刷盘；`.stats`、`.info`；
3. `.bitmap`、`.extents`、`.fsm`：文件 → 区 → 页 → 字节三层空间结构；
4. `SELECT rid, ...`、`.get`、`.page supplier 65`：RID 与槽目录；
5. 把一行改长：`.page` 中原槽变为 `FORWARD` 并指向新位置；`.flush` 后在 `debug` 级别用原 RID `.get`，日志中可见缓冲区的 `EVICT`/`MISS`，结果是新内容。（UPDATE 本身要全表扫描，在 64 帧缓冲池下会产生数百行 MISS/EVICT，所以不在 debug 级别下执行）；
6. 删除前 3000 行：`.extents` 中出现空闲页、`.fsm` 中出现空页；在 `debug` 级别插入一行，FSM 把它导向已释放的空间，页数不增长；
7. 清空整表：区位图从 `11111` 变为 `11000`，区列表出现空洞；重新导入后空洞被填回、区号复用；
8. `.stats`、`.frames`、`.flush`。

SF1 规模的演示：`--log-level info` 下 `.load lineitem test/tpc-h/lineitem.tbl`，观察进度日志与最终摘要。

---

## 16. 实施计划

| 日期 | 内容 | 产出 |
|---|---|---|
| 10/22（四） | 设计评审，冻结磁盘格式 | 本文档 v1.0 |
| 10/23（五） | 工程骨架、DiskManager、通用页头、BufferPool、CLOCK、PageGuard | 对应单元测试通过 |
| 10/24（六） | 类型系统、Tuple 编解码、HeapPage | 对应单元测试通过 |
| 10/25（日） | SpaceManager、Segment、FSM | 对应单元测试通过 |
| 10/26（一） | TableHeap、Catalog、shell、loader；supplier 端到端 | 集成测试 1、2 通过 |
| 10/27（二） | 随机差分测试、SF1 规模测试、修 bug、日志与 dump 命令 | 演示脚本可运行 |
| 10/28（三） | 实验报告、打包提交（23:59 前） | PDF 报告 + 源码 + 可执行文件 |
| 10/30（五） | 14:00 验收 | |

LRU-2、溢出页链表等非关键功能如时间不足可后移，但其接口和格式按本文档保留。

**实现状态（10/27）**：第 4 ~ 10、13、14 章已实现，包括 `COPY`、`PRAGMA`、`rid` 伪列、shell 元命令和演示脚本。已用课程提供的 SF1 数据完成全量导入与逐行比对（15.3）。尚未实现：LRU-2；随机差分测试；`Dockerfile`。

实验报告结构对应：实验分工 → 第 16 章；实验环境 → 第 2 章；设计与实现 → 第 3 ~ 11 章；使用手册 → 第 14 章；测试方案和结果 → 第 15 章及实测数据。

---

## 17. 风险与待定问题

### 17.1 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 助教环境编译失败 | 无法验收 | C++20、依赖内置、静态链接、Dockerfile、README 写明编译器版本 |
| 时间不足 | 功能不全 | 按第 16 章顺序推进，先保证 supplier 端到端；SF1 和 LRU-2 可降级 |
| 浮点精度 | 数值与源数据不一致 | FLOAT 按 double 存储；输出用能精确读回的最短十进制串（`std::to_chars`），`4192.40` 显示为 `4192.4`，校验按数值比较；需要固定小数位时用 `DECIMAL(p,s)` |
| 大规模导入时日志拖慢速度 | SF1 导入过慢 | `info` 级别只输出汇总 |
| 段头页区列表溢出逻辑出错 | 大表损坏 | 单元测试中把内联容量临时调小以覆盖溢出路径；200 万行的合成导入（约 580 个区）（581 个区列表项，含 1 个溢出页）已验证溢出页的写入、关闭后重新打开、扫描全部正确 |

### 17.2 待定问题

1. 实验以个人还是小组形式完成？若为小组，按第 3.2 节的模块边界分工；
2. 后续四次实验的具体内容和顺序，需要与课程安排核对第 12 章；
3. 验收是否需要在 Windows 原生环境运行（目前假设 WSL 即可）；
4. 项目名称 MiniDB 是否保留。

---

## 18. 附录：核心接口草案

```cpp
namespace minidb {

// ---------- storage ----------
class DiskManager {
 public:
  static Result<std::unique_ptr<DiskManager>> Open(const std::string& path, bool create);
  Status ReadPage(page_id_t, std::byte* out);          // 校验 checksum 与 page_id
  Status WritePage(page_id_t, const std::byte* data);  // 对副本计算 checksum 后写出
  Status ExtendTo(uint32_t extents);
  Status Sync();
};

// ---------- buffer ----------
class BufferPool {
 public:
  BufferPool(DiskManager&, LogManager&, const BufferConfig&);
  Result<ReadPageGuard>  FetchRead(page_id_t);
  Result<WritePageGuard> FetchWrite(page_id_t);
  Result<WritePageGuard> NewPage(Segment&, PageType);
  Status FlushPage(page_id_t);
  Status FlushAll();
  const BufferStats& Stats() const;
};

// ---------- space ----------
class SpaceManager {
 public:
  Result<extent_id_t> AllocateExtent(WriteCtx&);
  Status              FreeExtent(WriteCtx&, extent_id_t);
  Result<std::unique_ptr<Segment>> CreateSegment(WriteCtx&, SegmentType);
  Result<std::unique_ptr<Segment>> OpenSegment(page_id_t header_page);
};

class Segment {
 public:
  Result<page_id_t> AllocatePage(WriteCtx&);
  Status            FreePage(WriteCtx&, page_id_t);
  Result<page_id_t> FindPageWithSpace(uint16_t need);    // FSM 搜索
  void              UpdateFreeSpace(page_id_t, uint16_t free_bytes);
  PageIterator      Pages();                             // 按区列表遍历已分配页
  Status            Drop(WriteCtx&);
};

// ---------- record ----------
class Tuple {
 public:
  static Result<Tuple> FromValues(const Schema&, std::vector<Value>);
  static Result<Tuple> Decode(const Schema&, ByteView);
  void   EncodeTo(std::vector<std::byte>& out) const;
  Value  Get(size_t col) const;
};

// ---------- db ----------
class Database {
 public:
  static Result<std::unique_ptr<Database>> Open(const Config&);
  Status Close();
  Catalog&    catalog();
  TxnManager& txns();
  BufferPool& buffer_pool();
 private:
  // 声明顺序即构造顺序，析构顺序相反
  std::unique_ptr<DiskManager>  disk_;
  std::unique_ptr<LogManager>   log_;
  std::unique_ptr<BufferPool>   pool_;
  std::unique_ptr<SpaceManager> space_;
  std::unique_ptr<Catalog>      catalog_;
  std::unique_ptr<TxnManager>   txns_;
};

}  // namespace minidb
```
