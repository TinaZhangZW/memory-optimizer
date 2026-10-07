# 在 64 GiB VM 中手动测量 1 GiB 冷页回收

先完成 [冷页检测手册](../../cold_pages_detection/doc/how-to-run.md)，保持 VM 和 guest 填充负载运行。本实验在 host 调用 process_madvise(MADV_PAGEOUT)，记录 syscall 和回收阶段耗时。下面命令都在目标机 host 的同一个 Bash 中执行。

## 1. 设置本次目录，读取当前 VM 信息

```bash
cd /home/tina/workspace/poc/cpd
set -euo pipefail
read -r -p '冷页检测结果目录的绝对路径: ' CPD_OUT
test -s "$CPD_OUT/all-ram.hva"
source "$CPD_OUT/ram.env"
[[ $(cat run/linux-vm/qemu.pid) == "$PID" ]]
ps -p "$PID" -o pid,args

OUT="$PWD/experiments/page_reclaiming/results/manual-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
cp "$CPD_OUT/ram.env" "$OUT/ram.env"
echo "$OUT"
```

VM 重启过必须返回检测手册重新查询 PID/RAMBlock，不复用历史 results 下的 ram.env。不同 shell 不会自动继承变量。

## 2. 开启 zswap 并记录环境

```bash
cat /proc/swaps
sudo swapon --show
cat /sys/module/zswap/parameters/{enabled,compressor,max_pool_percent}

cat /sys/module/zswap/parameters/enabled > "$OUT/zswap.before"
echo Y | sudo tee /sys/module/zswap/parameters/enabled
if [[ -e /sys/module/kvm/parameters/force_age_flush ]]; then
  cat /sys/module/kvm/parameters/force_age_flush > "$OUT/force_age_flush.before"
  echo Y | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
{ uname -a; cat /proc/swaps; cat /sys/module/zswap/parameters/{enabled,compressor,max_pool_percent}; } > "$OUT/environment.txt"
```

zswap 需要有效 swap 设备提供 swap slots。目标机已有约 32 GiB 分区，先确认，不要临时格式化其他设备。历史测量为 lzo；记录当前实际算法，不在实验中途切换。只有部分页写入 zswap 或发生磁盘 swap 时必须单独说明。

定义一个只记录状态的函数，后面重复使用：

```bash
snapshot() {
  local prefix=$1
  sudo cat "/proc/$PID/smaps_rollup" > "$prefix.smaps"
  grep -E '^(pswpin|pswpout|zswpin|zswpout|zswpwb) ' /proc/vmstat > "$prefix.vmstat"
  sudo bash -c 'for f in /sys/kernel/debug/zswap/*; do
    [[ -f $f ]] || continue
    printf "%s " "${f##*/}"; cat "$f"
  done' > "$prefix.zswap"
}
```

debugfs 应挂载在 /sys/kernel/debug；若未挂载，可运行 sudo mount -t debugfs debugfs /sys/kernel/debug。某些内核计数不同，空 zswap 快照不能当作全部计数为零。

## 3. 为本轮重新检测冷页

```bash
ROUND=round1
test ! -e "$OUT/$ROUND.pageout.log"

sudo ./memory-optimizer/task-refs -p "$PID" --scan idle-bitmap --backend none \
  --addresses "$CPD_OUT/all-ram.hva" --ram-range "$RAM_RANGE" \
  --bitmap-max-pages 16777216 --bitmap-batch-bytes 4096 --bitmap-threads 1 \
  -l 1 -i 1 -o "$OUT/$ROUND.prepare" > "$OUT/$ROUND.prepare.log" 2>&1

awk -F '\t' 'NR>1 && $5==1 && $2==0 {
  print $1; if(++n==300000)exit
}' "$OUT/$ROUND.prepare.pages.tsv" > "$OUT/$ROUND.candidates.hva"
[[ $(wc -l < "$OUT/$ROUND.candidates.hva") == 300000 ]]
```

pages.tsv 列分别为 HVA、accessed_rounds、idle_rounds、valid_rounds、complete。选取 300000 个有效冷页，预留少量可能变热/迁移的余量，实际回收尝试上限为 262144 页。不要仅复用上轮候选；guest 重建负载后，同一 HVA 内的页面用途和访问状态可能不同。

## 4. 回收并记录时间

```bash
snapshot "$OUT/$ROUND.before"
sudo /usr/bin/time -f 'elapsed_s=%e user_s=%U system_s=%S max_rss_kib=%M' \
  -o "$OUT/$ROUND.command.time" \
  ./memory-optimizer/task-refs -p "$PID" --scan idle-bitmap --backend zswap \
  --addresses "$OUT/$ROUND.candidates.hva" --ram-range "$RAM_RANGE" \
  --bitmap-max-pages 300000 --bitmap-batch-bytes 4096 --bitmap-threads 1 \
  --max-pageout-pages 262144 -l 1 -i 1 -c 0 -m cold -o "$OUT/$ROUND.scan" \
  > "$OUT/$ROUND.pageout.log" 2>&1
snapshot "$OUT/$ROUND.after"
grep -E '^pageout:|^pageout stats:' "$OUT/$ROUND.pageout.log"
cat "$OUT/$ROUND.command.time"
```

每页一次 syscall。确认 attempted_pages=262144、submitted_bytes=1073741824、errors=0、complete=1、timing_valid=1；不足量的轮次不计入 1 GiB 对比。选取的候选可能来自 guest 各类 RAM，不是固定的一组应用页面。

| 字段 | 含义 |
|---|---|
| syscall_wall_ms | 所有 syscall 区间的 wall time 累计，包含等待/调度 |
| syscall_cpu_ms | 调用线程 CPU 时间，包含区间内计时开销 |
| submit_wall_ms | 提交循环，含映射复查、syscall 和统计 |
| total_wall_ms | 回收阶段，额外包含返回后 pagemap 验证 |
| command.time | 整条命令，还包含这次小范围冷页检测 |
| swapped_after | 返回后复查时具有 swap PTE 的页数 |

这些时间重叠，不能相加。CPU 计时区间包围 wall 时钟读取，262144 次采样会积累开销，所以 syscall_cpu_ms 可能比 syscall_wall_ms 大；不是纯内核回收 CPU 时间。submitted_bytes 是成功处理 advice 的量，不保证原 PFN 全部释放。swap PTE 也不独立证明物理页释放。

查看 swap 计数增量：

```bash
awk 'NR==FNR{v[$1]=$2;next}{printf "%s delta=%d\n",$1,$2-v[$1]}' \
  "$OUT/$ROUND.before.vmstat" "$OUT/$ROUND.after.vmstat"
cat "$OUT/$ROUND.before.zswap" "$OUT/$ROUND.after.zswap"
```

预期 zswpout 增长；pswpout/pswpin/zswpwb 不增长时支持无磁盘 I/O 的结论。零页可走 zeromap，不能用 zswpout 少于提交页数直接认定失败。压缩池自身占内存，释放原页 1 GiB 不等于净节省 1 GiB。

## 5. 重复三轮

第 1 轮结束后，guest 有些页仍换出，不能原样再次回收计时。先重建负载：

```bash
bash scripts/vm-ssh.sh 'sudo systemctl stop idle-memory-fill'
bash scripts/vm-ssh.sh 'sudo systemd-run --unit=idle-memory-fill --property=LimitMEMLOCK=infinity /tmp/guest-memory-fill 59'
bash scripts/vm-ssh.sh 'sudo journalctl -u idle-memory-fill -n 20 --no-pager'
```

确认 guest 有足够内存，并等待新的 ready。然后设置 ROUND=round2，从第 3 步重新执行；再做 round3。若启动失败先查看服务日志并按需 sudo systemctl reset-failed idle-memory-fill。重建 59 GiB 负载不保证全部 host RAM 已恢复；本流程仅选取重新确认 resident/eligible 的冷页。保留每轮 RSS、Swap、候选数和内容状态，避免将差异完全归因于回收算法。

## 6. 可选：单独验证同步回收量

不要在正式计时轮开启 tracing。准备新的 ROUND=traced，按第 5 步重建负载，再执行第 3 步生成候选，然后：

```bash
test -e /sys/kernel/tracing/events/vmscan/mm_vmscan_reclaim_pages/format
sudo trace-cmd record -B cpd-reclaim-manual -F \
  -e vmscan:mm_vmscan_reclaim_pages -o "$OUT/reclaim.dat" \
  ./memory-optimizer/task-refs -p "$PID" --scan idle-bitmap --backend zswap \
  --addresses "$OUT/$ROUND.candidates.hva" --ram-range "$RAM_RANGE" \
  --bitmap-max-pages 300000 --bitmap-batch-bytes 4096 --bitmap-threads 1 \
  --max-pageout-pages 262144 -l 1 -i 1 -c 0 -m cold -o "$OUT/$ROUND.scan" \
  > "$OUT/$ROUND.pageout.log" 2>&1 || {
    echo 'trace-cmd 返回错误；检查日志和数据文件，不把该轮纳入计时均值'
  }
sudo trace-cmd report -i "$OUT/reclaim.dat" > "$OUT/reclaim.trace" 2> "$OUT/reclaim-report.err" || true
awk '/mm_vmscan_reclaim_pages:/ {
  events++;for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="nr_reclaimed")reclaimed+=a[2]}
} END{print "events="events,"nr_reclaimed="reclaimed}' "$OUT/reclaim.trace"
grep -iE 'lost|missed' "$OUT/reclaim.trace" "$OUT/reclaim-report.err" || true
cat "$OUT/reclaim-report.err"
```

-F 按执行该命令的进程过滤。核对事件来自本次 task-refs PID、没有丢事件，统计 nr_reclaimed。该计数只验证 tracing 轮，不能替代其他轮的逐页证明。
目标机旧 trace-cmd 2.7 在新内核上可能报告实例删除 EBUSY 或 report 格式兼容提示。先保留日志和数据；有错误不能未经核对就当作成功。历史一次解析出了完整 262144 个单页回收事件。

仅清理本实验创建的专用实例，不动其他 tracing 会话：

```bash
sudo bash -s <<'EOF'
dir=/sys/kernel/tracing/instances/cpd-reclaim-manual
if [[ -d $dir ]]; then
  echo 0 > "$dir/tracing_on"
  echo 0 > "$dir/events/enable"
  rmdir "$dir"
fi
EOF
```

## 7. 收尾或继续恢复实验

```bash
cat "$OUT/zswap.before" | sudo tee /sys/module/zswap/parameters/enabled
if [[ -f "$OUT/force_age_flush.before" ]]; then
  cat "$OUT/force_age_flush.before" | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
```

关闭 zswap 新存入功能不会删除已有压缩对象，已有页仍可恢复。继续 [恢复实验](../../page_restoration/doc/how-to-run.md) 时保留 VM/填充负载；恢复手册会另行开启 zswap。若所有实验结束，按检测手册恢复最初 aging 状态并停止负载。

历史结果：7.3.0-rc4+ 三轮 syscall 平均 1.754 秒，回收阶段含复查平均 2.415 秒。仅为相应内容/状态下的参考，不能直接外推全部 64 GiB。
