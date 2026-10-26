# MiniDB 设计文档

> 版本：v0.1（草案）
> 适用范围：数据库系统实现课程实验一（存储管理），同时作为后续四次实验的总体架构基线
> 状态：待评审，评审通过后冻结第 4 ~ 7 章的磁盘格式

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
- 多文件、表空间、分区；
- 压缩、加密；
- `O_DIRECT`、io_uring 等异步 I/O；
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
| GoogleTest | 单元测试 | 源码放入 `third_party/googletest`，不在线拉取 |
| （无其他） | | CRC32C、日志、命令解析均自行实现 |

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
├── Dockerfile
├── README.md
├── docs/
│   └── design.md              # 本文档
├── third_party/
│   └── googletest/
├── src/
│   ├── common/                # 类型、配置、Result、日志、CRC32C
│   ├── storage/               # DiskManager、通用页头、文件头页、区位图页
│   ├── space/                 # SpaceManager、Segment、段头页、FSM
│   ├── buffer/                # BufferPool、Replacer、PageGuard
│   ├── record/                # 类型系统、Schema、Tuple 编解码
│   ├── table/                 # HeapPage、TableHeap、迭代器
│   ├── catalog/               # Catalog、系统表引导
│   ├── txn/                   # Transaction、TxnManager（实验一为最小实现）
│   ├── log/                   # LogManager（实验一为空实现）
│   └── db/                    # Database：组件装配与启动/关闭流程
├── tools/
│   ├── shell/                 # 交互式命令行
│   └── loader/                # .tbl 批量导入
├── tests/
│   ├── unit/
│   └── integration/
└── scripts/
    └── demo_supplier.txt      # 验收演示脚本
```

构建产物：`libminidb_core.a`（核心库）、'libminidb.so'、`minidb_shell`、`minidb_tests`。所有工具和测试只通过核心库的公开头文件访问系统。

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

**创建新库**（文件不存在）：

1. 创建文件，写入文件头页（页 0）和区位图页（页 1），区 0 标记为已用；
2. 创建两个系统表段 `sys_tables` 与 `sys_columns`，把它们的段头页号写入文件头；
3. `FlushAll`，设置 `clean_shutdown = 1`。

**打开已有库**：

1. 读取并校验文件头（魔数、版本、页大小、区大小）；
2. 若 `clean_shutdown == 0`，实验一打印警告；恢复实验中在此处执行 ARIES 恢复；
3. 置 `clean_shutdown = 0` 并立即写回；
4. 扫描系统表，加载 Catalog 到内存。

**关闭**：

1. 确认没有活跃事务；
2. `BufferPool::FlushAll()`，`fsync`；
3. 置 `clean_shutdown = 1`，写回文件头，`fsync`。

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

页号到文件偏移：`offset = (uint64_t)page_id * kPageSize`，必须使用 64 位运算。
区号到页号：`first_page = extent_id * kPagesPerExtent`。区 0 包含页 0（文件头）和页 1（区位图），整个区 0 保留给系统使用。

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

### 5.1 整体布局

单个数据文件，按区（64 页）增长：

```
区 0（系统保留）                      区 1               区 2
┌────────┬────────┬──────────────┐ ┌───────────────┐ ┌───────────────┐
│ 页 0    │ 页 1    │ 页 2 ~ 63     │ │ 页 64 ~ 127    │ │ 页 128 ~ 191   │ ...
│ 文件头   │ 区位图   │ 预留          │ │ 属于某个段       │ │ 属于某个段       │
└────────┴────────┴──────────────┘ └───────────────┘ └───────────────┘
```

段（segment）是区的集合，每张表、每个索引各占一个段。段的第一个区的第一页是段头页。

### 5.2 通用页头（所有页共用，32 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `checksum` | CRC32C，计算时该字段视为 0 |
| 4 | 2 | `page_type` | 见下表 |
| 6 | 2 | `flags` | 预留 |
| 8 | 4 | `page_id` | 页的自身编号，读入时校验 |
| 12 | 4 | `segment_id` | 所属段，系统页为 0 |
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

校验规则：DiskManager 写页前计算校验和；读页后校验校验和与 `page_id`。全零页视为"已分配未写入"的合法页（不会出现在实验一的正常流程中，但为恢复实验预留此语义）。

### 5.3 文件头页（页 0）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 32 | 8 | `magic` = `"MINIDB01"` |
| 40 | 4 | `format_version` = 1 |
| 44 | 4 | `page_size` = 8192 |
| 48 | 4 | `pages_per_extent` = 64 |
| 52 | 4 | `file_extents`（文件当前包含的区数） |
| 56 | 4 | `sys_tables_seg_page`（系统表段头页号） |
| 60 | 4 | `sys_columns_seg_page` |
| 64 | 4 | `next_segment_id` |
| 68 | 4 | `next_table_id` |
| 72 | 8 | `next_txn_id` |
| 80 | 8 | `checkpoint_lsn`（预留） |
| 88 | 1 | `clean_shutdown` |

### 5.4 区位图页（页 1）

页体 8160 字节 = 65280 位，每位对应一个区（1 = 已分配）。可管理 65280 × 512 KB ≈ 31.9 GB，作为单文件上限。另在页体之前保存 `alloc_hint`（上次分配位置），用于加速查找。

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
| `fsm_page_count` | 4 | |
| `fsm_pages[32]` | 128 | FSM 页的物理页号 |
| 其余 | | 预留 |

**区列表项（每项 16 字节）**

| 字段 | 长度 | 说明 |
|---|---|---|
| `extent_id` | 4 | `kInvalid` 表示空洞 |
| `reserved` | 4 | |
| `used_mask` | 8 | 64 位页掩码，1 = 该页已分配 |

段头页内可放 (8160 − 192) / 16 = 498 项，约 249 MB；超出部分放入溢出页（`EXTENT_LIST` 类型，每页 509 项，链表连接）。

**段内页号（spno）**：`spno = 区列表下标 × 64 + 区内偏移`。FSM 以 spno 为下标。为保证 spno 稳定，**区被回收时在列表中留下空洞（extent_id 置为无效），以后新分配的区优先填入空洞，列表不做压缩**。

### 5.6 FSM 页

页体为 8160 个 `uint8_t`，第 i 项对应 `spno = 页序号 × 8160 + i` 的页：

- 值 = `可用字节数 / 32`（0 ~ 254）；
- 非堆页（段头页、FSM 页、溢出页）和未分配的页值为 0。

32 个 FSM 页可覆盖 261120 页 ≈ 2 GB，即单段上限。FSM 页从段自身分配。

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
| 单文件上限 | 约 31.9 GB |
| 单段上限 | 约 2 GB（受 FSM 页数限制，可通过增加 `fsm_pages` 扩展） |
| 单条记录上限 | 约 8100 字节 |
| supplier 平均记录 | 约 170 字节，约 47 条/页，10000 条约 215 页（1.7 MB） |
| TPC-H SF1 全部表 | 约 1.2 GB 数据文件 |

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

1. 遍历区列表，找 `used_mask != ~0` 的项（从上次位置开始）；
2. 找到则取最低的 0 位，置位，`page_count++`；
3. 都满则 `AllocateExtent()`，填入第一个空洞或追加到列表末尾；
4. 返回物理页号。新页由 BufferPool 直接在内存中初始化，不从磁盘读。

**回收 `FreePage(page_id)`**

1. 清对应位，`page_count--`，FSM 项置 0；
2. 若该区 `used_mask == 0` 且不是段的第一个区，调用 `FreeExtent`，列表项置为空洞。

**删除段 `DropSegment()`**：回收所有区（含溢出页所在区），为以后的 `DROP TABLE` 预留。

### 7.4 记录级空间（FSM + 页内管理）

**插入时找页**

1. `need = 记录长度 + 4（槽，若无空槽可复用）`，`need_cat = ceil(need / 32)`；
2. 从 `insert_hint` 开始顺序扫描 FSM，找第一个 `cat ≥ need_cat` 的页（到末尾后回绕一次）；
3. 找不到则 `AllocatePage()` 新建堆页；
4. 在页内插入：优先复用空槽；连续空闲不足但总可用足够时先整理；
5. 用页的实际可用空间更新 FSM，更新 `insert_hint`。

**FSM 自我修正**：FSM 只是提示。如果 FSM 显示某页够用而实际不够（例如崩溃后 FSM 落后于数据页），插入失败时用实际值修正该项后继续搜索。因此 FSM 修改不需要写日志，这与 PostgreSQL 的处理方式一致。

**删除**

1. 实验一中删除立即生效：槽置空，`frag_bytes += 长度`，`live_count--`，更新 FSM；
2. 若 `live_count == 0` 且页中没有 `FORWARD` 槽，调用 `FreePage` 回收整页（段的第一个堆页保留）。

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
   1. 从 `free_list_` 取空帧；没有则 `replacer_->Evict()` 选牺牲帧（只会选 `pin_count == 0` 的帧），都不可用则返回错误「缓冲池已满」；
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
3. 返回 `WritePageGuard`，帧标记为脏。

### 8.6 刷盘

- `FlushPage(id)`：写回单页（若脏）；
- `FlushAll()`：写回全部脏页，`fsync`；
- 实验一的刷盘时机：淘汰时、关闭时、shell 的 `flush` 命令。

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

**Update**

1. 新记录 ≤ 原记录：原地覆盖，差值计入 `frag_bytes`；
2. 新记录 > 原记录且本页总可用足够：页内删除旧记录 + 整理 + 写入新记录，槽号不变；
3. 否则：按第 6.4 节建立转发。

**Delete**：若为 `FORWARD`，先删目标再删原槽；更新 FSM；按第 7.4 节决定是否回收整页。

**Scan**：按区列表顺序遍历已分配页，只处理 `HEAP` 类型页；页内按槽号顺序输出，跳过空槽和 `MOVED_IN`，跟随 `FORWARD`。迭代器每次只持有一个页的读 Guard。

---

## 10. 系统目录（Catalog）

### 10.1 系统表

两张系统表本身就是普通的堆表，它们的 Schema 硬编码在代码中，段头页号记录在文件头：

```
sys_tables (table_id INT, name VARCHAR(64), seg_header_page INT, column_count INT)
sys_columns(table_id INT, ordinal INT, name VARCHAR(64), type INT, length INT, nullable INT)
```

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
| 索引 | `index/BPlusTree`，`sys_indexes` | Segment（类型 INDEX）、BufferPool、PageGuard、RID | Catalog 增加索引元数据；执行层在增删改时同步维护索引 |
| 查询处理 | `parser/`、`binder/`、`planner/`、`execution/`（火山模型：SeqScan、IndexScan、Filter、Projection、Join、Aggregate、Sort） | TableHeap::Scan/Get、Catalog | shell 命令替换为 SQL 入口 |
| 并发控制 | `concurrency/LockManager`、多线程测试框架 | Transaction、RID、页锁存 | 缓冲池 I/O 移出全局锁；删除改为逻辑删除 + 提交时物理删除 |
| 故障恢复 | `log/LogManager` 实装、日志记录类型、`recovery/` | 统一页修改路径、page_lsn、WAL 钩子、`clean_shutdown` | 无结构性改动 |

查询处理相关的选择（解析器手写还是引入库、火山模型还是向量化）对存储层没有约束，到该实验时再决定。

---

## 13. 可观测性：日志与统计

### 13.1 日志格式

```
[时间戳][级别][模块] 事件 key=value ...
```

示例：

```
[10:21:03.512][DEBUG][BUF]   MISS pid=1203 frame=17 victim=877 dirty=1 -> FLUSH
[10:21:03.512][DEBUG][SPACE] ALLOC extent=42 seg=3 file_extents=43
[10:21:03.513][DEBUG][FSM]   seg=3 spno=130 cat 211 -> 205
[10:21:03.513][DEBUG][HEAP]  INSERT rid=(1203,14) len=171
[10:21:03.540][DEBUG][HEAP]  UPDATE rid=(1203,14) -> FORWARD (1544,2)
[10:21:05.001][INFO ][BUF]   stats hit=98213 miss=2311 evict=1287 flush=1203 hit_rate=97.7%
```

模块：`DISK`、`BUF`、`SPACE`、`FSM`、`HEAP`、`CAT`、`TXN`。

级别使用约定：

- `DEBUG`：每次缓冲区访问、空间分配回收、记录操作。用于 supplier 演示；
- `INFO`：每导入 N 条输出一次进度和统计摘要。用于 SF1 大规模导入；
- `WARN` / `ERROR`：异常情况。

日志器是唯一允许的全局对象，支持同时输出到控制台和文件，级别可在 shell 中动态修改。

### 13.2 调试命令

| 命令 | 输出 |
|---|---|
| `stats` | 缓冲池计数器、命中率、文件大小、各段页数 |
| `dump page <id>` | 页头、槽目录、空闲空间 |
| `dump segment <table>` | 区列表、页掩码、FSM 分布直方图 |
| `dump bitmap` | 区位图使用情况 |

这些命令同时也是验收时讲解数据结构的工具。

---

## 14. 工具：shell 与 loader

### 14.1 shell 命令

```
CREATE TABLE ... ;                         -- 支持 DDL 列表 1 的语法
load <table> <path.tbl>                    -- 批量导入，字段以 '|' 分隔
scan <table> [limit <n>]
get <table> <page>:<slot>
insert <table> <v1>|<v2>|...
delete <table> where <col> = <value>
update <table> set <col> = <value> where <col> = <value>
stats | flush | dump ... | log level <debug|info|warn>
.quit
```

- `CREATE TABLE` 由一个小型递归下降解析器处理（约 200 行），只支持实验所需的列类型和 `NOT NULL`；
- `delete` / `update` 的条件只支持单列等值，执行方式为全表扫描 + 过滤；
- 查询处理实验时整体替换为 SQL 入口，现有命令保留为调试命令。

### 14.2 loader

- 按行读取 `.tbl`，按 `|` 切分（TPC-H 每行末尾多一个 `|`，需忽略）；
- 按 Schema 解析各字段（日期 `YYYY-MM-DD` → 天数）；
- 每 1000 条一个事务（实验一中只影响日志粒度）；
- 结束时输出：记录数、耗时、吞吐、数据文件大小、缓冲池统计。

---

## 15. 测试方案

### 15.1 单元测试

| 模块 | 测试点 |
|---|---|
| DiskManager | 读写一致；校验和错误能被发现；大于 4 GB 偏移的读写 |
| Replacer | CLOCK、LRU-2 的淘汰顺序；pin 住的帧不被淘汰 |
| BufferPool | 4 帧的小缓冲池：命中、淘汰、脏页写回、全部 pin 住时报错、Guard 析构后 unpin |
| SpaceManager / Segment | 区分配回收、文件扩展、空洞复用、溢出页、spno 稳定 |
| FSM | 类别计算、hint 搜索与回绕、自我修正 |
| HeapPage | 插入、删除、空槽复用、整理后 RID 不变 |
| Tuple | 每种类型的编解码往返；NULL；CHAR 填充；VARCHAR 超长报错；日期边界 |
| TableHeap | 增删改查；更新触发转发；转发后再更新；扫描不重复不遗漏 |
| Catalog | 建表后重启仍可读 |

### 15.2 集成测试

1. **正确性**：导入 supplier，记录数 = 10000；逐条把记录重新格式化为 `.tbl` 行，与源文件逐行比对；
2. **持久化**：导入 → 关闭 → 重新打开 → 再次比对；
3. **随机差分测试**：随机执行增删改（含大量变长更新以触发转发和整理），与内存中的 `std::map` 模型比对，每轮结束后重启再比对；
4. **空间回收**：插入大量记录后全部删除，检查页和区被回收，区位图恢复。

### 15.3 规模测试

- 导入 TPC-H SF1 全部 8 张表（lineitem 约 600 万行）；
- 缓冲池设为 1024 帧，记录耗时、文件大小、命中率；
- 对比 CLOCK 与 LRU-2 在 `scan lineitem` 交替访问小表时的命中率（可选）。

### 15.4 验收演示脚本

`scripts/demo_supplier.txt`：

1. 设置日志级别 `debug`，缓冲池 64 帧；
2. `CREATE TABLE SUPPLIER ...`；
3. `load supplier supplier.tbl`，展示区分配、页分配、FSM 变化、缓冲区淘汰；
4. `scan supplier limit 5`、`get`；
5. `update` 一条记录使其变长，展示转发；
6. `delete` 一批记录，展示 FSM 更新与页回收；
7. `stats`、`dump segment supplier`；
8. 切换为 `info` 级别，展示 SF1 导入结果。

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

实验报告结构对应：实验分工 → 第 16 章；实验环境 → 第 2 章；设计与实现 → 第 3 ~ 11 章；使用手册 → 第 14 章；测试方案和结果 → 第 15 章及实测数据。

---

## 17. 风险与待定问题

### 17.1 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 助教环境编译失败 | 无法验收 | C++20、依赖内置、静态链接、Dockerfile、README 写明编译器版本 |
| 时间不足 | 功能不全 | 按第 16 章顺序推进，先保证 supplier 端到端；SF1 和 LRU-2 可降级 |
| 浮点精度 | 数值与源数据不一致 | FLOAT 按 double 存储；输出统一保留两位小数 |
| 大规模导入时日志拖慢速度 | SF1 导入过慢 | `info` 级别只输出汇总 |
| 段头页区列表溢出逻辑出错 | 大表损坏 | 单元测试中把内联容量临时调小以覆盖溢出路径 |

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
  Status WritePage(page_id_t, std::byte* data);        // 写前计算 checksum
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
