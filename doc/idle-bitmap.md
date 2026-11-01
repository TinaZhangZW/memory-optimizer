# Idle bitmap 冷页采样

`task-refs --scan idle-bitmap` 一次调用采样一个观察窗口（observation window），输出一个 TSV 文件。

## 单次采样

Host 内核需要 `CONFIG_IDLE_PAGE_TRACKING=y`，每次最多采样 64 GiB 的地址范围。
建议使用 `sudo` 运行。采样进程需要：

* 读取目标的 `/proc/PID/maps`、`/proc/PID/pagemap` 和 `/proc/kpageflags`。
* 具备读取 pagemap 中真实 PFN 的权限（需要 `CAP_SYS_ADMIN`）。
* 读写 `/sys/kernel/mm/page_idle/bitmap`。
* 在 `/run/lock/` 中创建并打开锁文件，在输出目录创建和替换文件。

仅具备 `CAP_SYS_ADMIN` 不代表同时拥有上述文件的访问权限。

**目前仅支持内核基础页大小为 4 KiB 的 host，只采样普通 4 KiB 匿名 LRU 页。**
THP、hugetlb 等大页及复合页（包括 2 MiB 大页）尚不支持。这些范围仍按
4 KiB 地址位置保留输出行，但标记为 `observed=0`，不能当作冷页。
如果 host 的内核基础页大小不是 4 KiB，采样器会报错退出。

```bash
make task-refs
sudo ./task-refs --scan idle-bitmap --pid PID \
    --ram-range 0xSTART:0xEND --window-ms 100 --output /tmp/pages.tsv
```

把 PID 和地址替换为实际值。范围是目标进程的 HVA，END 不包含在内；
地址支持十进制或 `0x`/`0X` 前缀的十六进制。
虚拟机应填写 Firecracker/QEMU 的 guest RAM 映射范围。
此模式只采样，不迁移或回收内存。一次调用只有一个观察窗口。

100 ms 是全部 idle 标记写完后，到开始读取之间的最短等待时间。
写入、读取和校验还需要时间，所以整个调用会超过 100 ms；各页的实际观察
区间也会随批次位置变化。stderr 输出 CLOCK_MONOTONIC 纳秒时间。
没有额外主动强制刷新 KVM TLB 的步骤。
idle/accessed bit 观察可能漏掉由已有 TLB 缓存的持续访问，尤其是很小的工作集；
因此结果表示内核接口观察到的访问，不是所有内存访问的精确记录。

## TSV 格式

```text
hva               	refs	observed
0x0000000100000000	0	1
0x0000000100001000	1	1
0x0000000100002000	0	0
```

每个 4 KiB 地址都有一行，地址递增；列之间使用TAB，地址固定宽度。
输出按 HVA 保留每个虚拟页。多个 HVA 可能映射同一个物理页，因此页数乘以
4 KiB 表示虚拟地址覆盖容量，不是按 PFN 去重的物理内存容量。

* `hva`：目标进程的虚拟地址。
* `refs`：本次观察是否发现访问，0 或 1。
* `observed`：是否得到有效观察。1 时 refs=0 表示冷页，refs=1 表示热页；
  0 表示无法分类，此时应忽略 `refs`，不能当作冷页。

仅对可读写、private VMA 内的普通匿名 LRU 页进行有效分类。
未驻留、THP、大页等均为无效观察。
窗口后会重新检查 VMA、PFN 和 page flags；只有前后均符合筛选条件且 PFN
相同的地址才有效。检测到目标退出时整次调用失败。两次检查不能证明映射在
整个窗口内始终未变，PFN 在两次检查间释放后又复用的情况也无法彻底排除。
冷表示窗口内没有观察到访问，不等于 Guest 空闲页，也不代表可以直接丢弃其内容。

成功后原子替换输出文件：读取者看到旧文件或完整的新文件；使用同一路径会替换，
不会追加。新建及替换后的文件权限均为 `0644`，不受 umask 或原文件权限影响。
输出目录必须事先存在。失败返回非零，原输出文件保持不变，调用方应检查退出码。

本采样器通过 `/run/lock/memory-optimizer-idle-PID.lock` 阻止同时采样同一 PID。

## 隔离测试

```bash
make task-refs
bash tests/run-idle-page-map.sh
bash tests/run-idle-bitmap-snapshot.sh
bash tests/run-idle-snapshot.sh
python3 tests/task-refs-idle-cli.py
```
