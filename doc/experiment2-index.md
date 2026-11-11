# 实验二：外存 B+ 树索引

本实验在实验一 MiniDB 上实现非唯一、单列 B+ 树索引，沿用 C++20、8 KB 页、DiskManager、CRC32C 和共享 CLOCK 缓冲池。支持 `INTEGER`、`BIGINT`、`CHAR(n)`、`VARCHAR(n)`，可对已有表建索引，并随 SQL 插入、删除、更新和 COPY 导入维护。索引关闭重开后可继续查询。

报告作者请先阅读 [报告交接说明](experiment2-report-handoff.md)，其中包含实测结果表、图源、截图顺序和源码导航。

## 实验要求对应

| 实验要求 | 实现与验收入口 |
| --- | --- |
| 索引初始建立 | `CREATE INDEX name ON table(column)` 扫描已有 heap 建树 |
| 增、删、改 | `INSERT` / `COPY` 插入索引项；`DELETE` 精确删除；`UPDATE` 按旧、新键维护 |
| 基于索引查找 | `SELECT ... WHERE column = / < / <= / > / >= value` 自动选用该列索引，结果消息显示 `INDEX SCAN name`；`!=` / `<>` 几乎命中全表，仍走堆扫描 |
| 外存及内外存交换 | 每个索引一个 `.idx` 文件，全部节点读写经 BufferPool；小缓冲池测试强制换页 |
| 两种数据类型 | TPC-H supplier 的 `s_suppkey INTEGER`、`s_name CHAR(25)` |
| 图形化结构展示 | `PRAGMA index_tree(name)` 输出带分支和叶链箭头的命令行树图；`index_dot` 输出 Graphviz 图源 |
| 模块接口与核心源码 | `src/index/bplus_tree.h/.cpp`、`src/db/index.cpp` |
| 测试规模 | 1,500,000 行真实 orders 上三个索引的建立、查找和增删改；10,000 行真实 supplier；20,000 次重开后逐条索引检索；随机测试、三层以上内部节点与最小扇出测试 |

## 编译、测试、演示

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6
ctest --test-dir build --output-on-failure

# 在仓库根目录执行；选择尚不存在的新数据库目录。
./build/minidb_shell --db /tmp/minidb_index_demo --buffer 16 --file scripts/demo_index.txt

# 独立自动验收，逐条检查真实 supplier 数据并输出树图和 DOT。
./build/minidb_verify_index_tpch test/tpc-h/supplier.tbl /tmp/minidb_index_acceptance

# 重开刚才的数据库。
./build/minidb_shell --db /tmp/minidb_index_demo --buffer 16
```

本机 CMake 不在默认 PATH，但已安装在：

```sh
export PATH="/Users/lostgreen/Library/Python/3.9/lib/python/site-packages/cmake/data/bin:$PATH"
```

实验数据不提交 Git。当前 `test/tpc-h/*.tbl` 链接到工作目录旁的 `benchmark_data`；其他机器需准备自己的 TPC-H 文件或更改 COPY 路径。验收程序拒绝覆盖已有数据库。

交互查询示例：

```sql
CREATE INDEX supplier_key ON supplier(s_suppkey);
CREATE INDEX supplier_name ON supplier(s_name);
SELECT rid, s_suppkey, s_name FROM supplier WHERE s_suppkey = 42;
SELECT s_suppkey, s_name FROM supplier WHERE s_name = 'Supplier#000000042';
SELECT s_suppkey, s_name FROM supplier WHERE s_suppkey >= 9995 LIMIT 6;
PRAGMA indexes(supplier);
PRAGMA index_tree(supplier_key);
PRAGMA index_stats(supplier_key);
PRAGMA index_check(supplier_name);
PRAGMA buffer_stats;
DROP INDEX supplier_name;
```

`CREATE INDEX IF NOT EXISTS`、`DROP INDEX IF EXISTS` 可用于重复执行；`DROP TABLE` 同时删除该表的索引。没有适用索引时 SELECT 沿用全表扫描。UPDATE/DELETE 与 SELECT 共用同一个行定位函数（`Database::FindRows`）：WHERE 列有索引时用索引定位，否则扫描 heap；两者都先收集全部命中行再修改，然后增量维护索引。未指定 ORDER BY 的 SQL 不保证顺序；索引路径按键、RID 顺序输出。

## 文件和页面设计

索引存放在数据库的 `indexes/` 子目录；文件名用索引名的十六进制编码，避免 SQL 引号标识符被解释成路径。索引名称在数据库内唯一，最长 100 字节。

- 页 0：复用 MiniDB 文件头，`FileKind::kIndex`，记录所属 table ID、extent 数。
- 页 1：索引格式版本、索引名、表名、列序号、键类型和长度、根页、下一个未分配页、空闲链表头、索引项数。
- 页 2 起：内部节点、叶节点或空闲页；每页保留公共 32 字节页头和 CRC32C。
- 节点头：键数量、叶链后继、内部节点最左子页；条目从偏移 64 开始。
- 整数键统一保存为有符号 64 位；字符串保存 2 字节实际长度及按声明长度预留的字节区。叶条目保存键和 RID（4 字节页号、2 字节槽号）；内部条目额外保存右子页号。

内部键是 `(key, RID)` 分隔下界。比较器与 SQL 共享整数及字符串比较语义，重复键由 RID 区分；相同键和 RID 不允许重复插入。NULL 不保存到索引，沿用现有 WHERE 不支持 NULL 比较的规则。CHAR 从 tuple 解码后去掉填充空格；VARCHAR 保留存储内容，比较使用引擎原有 PAD SPACE 规则。BIGINT 使用精确整数比较，避免 Apple Silicon 的浮点精度丢失。

节点最大键数取偶数：`floor((8192 - 64) / (key_bytes + 6 + 4))` 向下取偶。整数为 450，CHAR(25) 为 218。非根节点至少半满；内部节点孩子数等于键数加一。字符串声明长度支持 1..4000 字节，最大长度时容量为 2，仍满足分裂和合并不变量。

文件按 64 页 extent 扩容，扩容同时持久化文件头。删除产生的空闲页串成持久化链表，分配优先复用空闲页。正常删除不缩短物理文件，统计中的 `free_pages` 表示可复用页。

## 算法与接口

`BPlusTree::Create/Open` 创建或重开文件；`BulkLoad(entries)` 供 `CREATE INDEX` 使用：对全部条目排序后自底向上逐层写页（节点预留 1/8 空间）；`Insert(key, RID)`、`Erase(key, RID)` 维护条目；`Find(key)` 返回等值 RID；`Scan(CompareOp, key, visitor)` 支持范围与提前终止；`Validate` 检查结构；`Draw/Dot` 展示树。

插入从根沿分隔键二分定位，叶节点按 `(key, RID)` 有序插入。叶满时对半分裂并连接叶链，将右叶首键复制到父节点；内部节点分裂将中间键提升。分裂逐层传播，根分裂时创建新根。

删除精确定位 `(key, RID)`。节点不足半满时，先从富余的左兄弟借位，再尝试右兄弟；均不能借位则合并，并从父节点移除一个分隔键和子指针。内部节点借位/合并会旋转或下移父分隔键。根只剩一个孩子时收缩根。叶分隔键允许保留有效的历史下界，因此删除首键但未触发重平衡时不必向上更新全部祖先。

等值和右侧范围先从根定位下界叶，再顺叶链读取，故能找到跨叶的全部重复键。左侧范围和不等查询从最左叶开始，遇到右边界即结束。查询通过索引 RID 回表读取 tuple；`LIMIT` 可让 visitor 提前终止。

SQL 建索引先写 `.building` 文件，填充成功并 flush 后改名为 `.idx`，打开时只发现完成的 `.idx` 文件。索引元数据核对 table ID、列序号、类型和长度。写入维护使用 tuple 解码后的标准值，heap 转发更新仍使用稳定的原 RID。

复杂度：单条插入、删除、点定位访问 `O(log_F N)` 层节点；节点内操作受固定页容量限制。范围扫描访问定位路径及匹配叶页，回表成本取决于结果数量。建索引采用外部归并排序加自底向上装载（PostgreSQL、InnoDB 建 B 树索引的同类做法）：扫描 heap 把 `(key, RID)` 写入一块固定大小的排序缓冲（默认 64 MB，`PRAGMA index_sort_memory = <KB>`）；缓冲满时排序并作为一个有序段追加到临时文件；全部读完后用小顶堆对各段做多路归并，每段占一个读缓冲（预算的 1/8，限制在 8–64 KB）；归并路数不超过预算能负担的读缓冲数，段数更多时先把相邻的若干段归并成更长的段（多趟归并），直到一次归并能完成；最后一次归并输出直接填叶子页，页满即写出并把页号和最小键交给上一层，每层内存中只有一个正在填充的节点。总条数在归并前已知，所以各层节点数和每个节点的条数预先算好（预留 1/8 空间且不低于半满）。数据放得下时不产生临时文件，直接在内存排序。排序 `O(N log N)`，写树 `O(N)`，内存占用与表大小无关。节点在内存中保持页面的字节编码，查找在页内二分比较，插入、删除、分裂、合并都是定长槽位的移动，不把整页解码成对象。

## 结构检查和图形展示

`PRAGMA index_check(name)` / `index_stats(name)` 校验：节点容量与半满约束、键严格有序、父子分隔范围、全部叶深相同、叶链完整、项数一致、空闲页不重叠、已分配页均可达。成功输出 `valid=ok` 和高度、节点/叶页数、空闲页数、根页号。

`index_tree` 以 `|--`、`` `-- `` 和 `->` 展示父子关系及叶后继；`index_dot` 以实线表示父子，蓝色虚线表示叶链。图默认最多展示 128 个节点、每节点少量样例键，以保持大型数据集可读。统计/结构验证始终检查整棵树。

自动验收程序在新数据库目录输出 `supplier_key.tree/.dot` 和 `supplier_name.tree/.dot`。安装 Graphviz 后可运行 `dot -Tsvg supplier_key.dot -o supplier_key.svg` 生成 SVG。

## 自动测试

`minidb_index_tests` 覆盖：

1. 10,000 整数项、5,000 字符串项的固定种子乱序插入、重复值、六种比较运算（`=`、`<`、`<=`、`>`、`>=`、`<>`），与独立集合参考模型比对。
2. 大量删除、随机删除顺序、借位、合并、根收缩、清空后重新插入及空闲页复用。
3. 2–4 帧小缓冲池、跨多个 extent、flush 后销毁对象重新打开、空树重开。
4. 容量为 2 的长字符串索引，迫使内部节点形成五层以上树；字符串含 NUL、超长和类型错误输入。
5. BIGINT 最小/最大值及超过 `2^53` 的相邻整数精确查找。
6. 批量建树：多种规模与三种扇出，分别在内存内排序和 2 KB 预算（上百个有序段的外部归并）下建成，校验全部不变量并继续增删；跨有序段的重复项检测；临时文件清理。
7. 同一批 UPDATE/DELETE 在有索引和无索引的表上逐条对照表内容，包括修改被过滤的列、重复键、NULL 和转发行。
8. SQL 建索引前后的相同查询结果；重复索引、非法类型/列、NULL、CHAR 填充、范围、LIMIT、多个同列索引。
7. COPY、键更新、非键更新触发 heap 转发、删除、重开、DROP INDEX 和 DROP TABLE。
8. 磁盘页篡改后的校验和错误检测。

`minidb_verify_index_tpch` 使用真实 supplier：全量加载并建立两种索引，关闭重开后对每条源记录分别按整数和字符串查询，与原始数据比对；范围结果与删除索引后的全表扫描比对；验证增删改及再次重开。

## 集成边界

当前实现是实验二的单进程、非唯一、单列索引。未实现唯一/复合索引、并发树修改、WAL 和崩溃恢复，事务能力沿用实验一。普通 SQL 类型/长度错误在 heap 写入前校验；一旦实际索引维护失败，会尝试移除该表的派生索引并返回错误，避免后续查询使用过期索引。操作系统 I/O 故障或进程异常中断时不保证语句原子性；正常 flush/close 和重新打开已测试。

本文件提供设计、接口、使用手册与测试说明；课程要求的 Word/PDF 实验报告和小组分工需在提交材料中另外整理。
