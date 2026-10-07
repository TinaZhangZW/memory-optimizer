# 从 guest 手动测量页面恢复时间

先按 [冷页检测手册](../../cold_pages_detection/doc/how-to-run.md) 启动 64 GiB VM、填充约 59 GiB，并查询当前 RAMBlock。无需先回收一批不相关的页；本实验另建 1 GiB 受控区域，host 回收这一区域后再通知 guest 访问。同一页的回收和恢复一一对应。

所有代码块在目标机 host 的同一个 Bash 终端中执行，guest 命令由 vm-ssh.sh 转发。不要直接运行历史 results 下绑定旧 PID/目录的 run.sh。

## 1. 编译实验目录内的工具

```bash
cd /home/tina/workspace/poc/cpd
set -euo pipefail
read -r -p '本次冷页检测结果目录的绝对路径: ' CPD_OUT
source "$CPD_OUT/ram.env"
[[ $(cat run/linux-vm/qemu.pid) == "$PID" ]]
ps -p "$PID" -o pid,args

OUT="$PWD/experiments/page_restoration/results/manual-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
gcc -static -O2 -Wall -Wextra -Werror \
  experiments/page_restoration/tests/guest-restore-bench.c -o "$OUT/guest-restore-bench"
gcc -O2 -Wall -Wextra -Werror \
  experiments/page_restoration/tests/page-state.c -o "$OUT/page-state"
bash scripts/vm-ssh.sh \
  'cat > /tmp/guest-restore-bench && chmod +x /tmp/guest-restore-bench' < "$OUT/guest-restore-bench"
```

本实验目录携带已测试的新版源码，支持 1–1024 MiB 和 batch/latency 两种模式；不要用远端仓库里旧的 256 MiB 版本。page-state 读取 host QEMU pagemap，须 sudo 执行。

## 2. 开启 zswap，选择内容与计时模式

```bash
cat /proc/swaps
cat /sys/module/zswap/parameters/enabled > "$OUT/zswap.before"
echo Y | sudo tee /sys/module/zswap/parameters/enabled
if [[ -e /sys/module/kvm/parameters/force_age_flush ]]; then
  cat /sys/module/kvm/parameters/force_age_flush > "$OUT/force_age_flush.before"
  echo Y | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
{ uname -a; cat /proc/swaps; cat /sys/module/zswap/parameters/{enabled,compressor,max_pool_percent}; } > "$OUT/environment.txt"

PATTERN=sparse
MODE=batch
```

PATTERN 可改为 sparse/mixed/random：每个 4 KiB 页分别有 8/2048/4096 字节伪随机数据，其余为零。MODE=batch 只测整遍读取，MODE=latency 额外逐页计时；为了避免逐页计时污染整批结果，两种模式使用独立进程分别运行。
确认 swap 可用。默认使用当前 zswap 算法并记录，历史为 lzo。恢复实验期间不要并发运行其他扫描器/回收器。

## 3. 启动受控 guest 区域

```bash
bash scripts/vm-ssh.sh 'free -h; cat /proc/swaps'
# 除已有填充负载外，应至少还有约 1.5 GiB MemAvailable。
UNIT="restore-manual-$(date +%s)"
DIR=$(bash scripts/vm-ssh.sh 'mktemp -d /tmp/restore-bench.XXXXXX')
printf 'UNIT=%s\nDIR=%s\nPATTERN=%s\nMODE=%s\n' "$UNIT" "$DIR" "$PATTERN" "$MODE" > "$OUT/guest.env"

bash scripts/vm-ssh.sh "sudo systemd-run --unit='$UNIT' \
  --property='WorkingDirectory=$DIR' --property=LimitMEMLOCK=infinity \
  /tmp/guest-restore-bench 1024 '$PATTERN' '$MODE'"

bash scripts/vm-ssh.sh "for ((n=0;n<120;n++)); do
  if [[ -f '$DIR/pages.tsv' ]]; then cat '$DIR/pages.tsv'; exit; fi
  systemctl is-active --quiet '$UNIT' || exit 1
  sleep 1
done; exit 1" > "$OUT/guest-pages.tsv"
[[ $(wc -l < "$OUT/guest-pages.tsv") == 262144 ]]
bash scripts/vm-ssh.sh "sudo journalctl -u '$UNIT' -n 5 --no-pager"
```

pages.tsv 是页 index 与 GPA，**不是 host PFN/HVA**。Guest mlock 保持 guest 驻留，但不阻止 host 回收 QEMU backing。元数据和时钟调用位于目标区域之外。DIR 每个进程用全新目录，避免旧命令/结果被误读。

## 4. 从当前 QMP 地址空间转换 GPA→HVA

```bash
{ printf '{"execute":"qmp_capabilities"}\n';
  printf '{"execute":"human-monitor-command","arguments":{"command-line":"info mtree -f"}}\n';
} | timeout 5 nc -U run/linux-vm/qmp.sock > "$OUT/mtree.json" || [[ $? == 124 ]]
jq -r '.return | strings' "$OUT/mtree.json" > "$OUT/mtree.txt"
cat "$OUT/mtree.txt"
```

**下面公式只用于这台当前 q35 VM**。执行前确认 mtree 中 system RAM 主区域包含：

```text
0000000000100000-000000007fffffff ... ram @0000000000100000
0000000100000000-000000107fffffff ... ram @0000000080000000
```

即低 RAM 到 2 GiB，高 RAM 从 GPA 4 GiB 开始、对应 RAMBlock 偏移 2 GiB。若这些范围改变，不使用下列公式；改按实际 RAMBlock/alias 转换或逐页 QMP gpa2hva。

```bash
RAM_BASE="0x${RAM_RANGE%:*}"
awk -v base="$RAM_BASE" 'BEGIN{print "index\tgpa\thva";b=strtonum(base)}
{
  g=strtonum($2)
  if(g>=1048576 && g<2147483648)o=g
  else if(g>=4294967296 && g<70866960384)o=g-2147483648
  else {print "GPA 不在已确认的 RAM 范围" > "/dev/stderr";exit 1}
  printf "%s\t%s\t0x%x\n",$1,$2,b+o
}' "$OUT/guest-pages.tsv" > "$OUT/mapping.tsv"
awk -F '\t' 'NR>1{print $3}' "$OUT/mapping.tsv" > "$OUT/pages.hva"

# 均匀抽取 64 个页，使用 QMP 验证转换。
awk -F '\t' 'NR>1 && $1%4096==0 {
  printf "{\"execute\":\"human-monitor-command\",\"id\":\"%s\",\"arguments\":{\"command-line\":\"gpa2hva %s\"}}\n",$1,$2
}' "$OUT/mapping.tsv" > "$OUT/qmp-requests.json"
{ printf '{"execute":"qmp_capabilities"}\n'; cat "$OUT/qmp-requests.json";
} | timeout 5 nc -U run/linux-vm/qmp.sock > "$OUT/qmp-mappings.json" || [[ $? == 124 ]]
jq -r 'select(.id != null) | [.id,(.return | capture("is (?<hva>0x[0-9a-fA-F]+)").hva)] | @tsv' \
  "$OUT/qmp-mappings.json" > "$OUT/qmp-check.tsv"
[[ $(wc -l < "$OUT/qmp-check.tsv") == 64 ]]
awk -F '\t' 'NR==FNR{v[$1]=$2;next}$1 in v{if(v[$1]!=$3)exit 1}' \
  "$OUT/qmp-check.tsv" "$OUT/mapping.tsv"
```

所有检查通过后才能回收。Guest 测试进程重新创建或 VM 重启后必须重新导出/转换。抽查不证明 guest GPA 永远不迁移；发现映射变化或内容校验失败应作废该轮并重新准备。

## 5. 定义命令和状态采集函数

```bash
guest_command() {
  local tag=$1
  # tag 使用唯一的字母/数字/下划线/连字符，不重复使用。
  bash scripts/vm-ssh.sh "cd '$DIR'
    printf '%s\n' '$tag' > command.tmp
    mv command.tmp command
    for ((n=0;n<120;n++)); do
      if [[ -f '$tag.done' ]]; then cat '$tag.done'; exit; fi
      systemctl is-active --quiet '$UNIT' || exit 1
      sleep 1
    done
    exit 1" > "$OUT/$tag.done"
  grep -q ' errors=0 ' "$OUT/$tag.done"
  if [[ $MODE == latency ]]; then
    bash scripts/vm-ssh.sh "cat '$DIR/$tag.latency.tsv'" > "$OUT/$tag.latency.tsv"
  fi
  cat "$OUT/$tag.done"
}

snapshot() {
  grep -E '^(pswpin|pswpout|zswpin|zswpout|zswpwb) ' /proc/vmstat > "$1.vmstat"
  sudo bash -c 'for f in /sys/kernel/debug/zswap/*; do
    [[ -f $f ]] || continue
    printf "%s " "${f##*/}"; cat "$f"
  done' > "$1.zswap"
}
```

命令先写临时文件再 rename，done 最后发布。服务死亡、超时、errors 非零都算失败。batch 的 latency TSV 只有表头，这是预期行为。debugfs 未挂载时按回收手册挂载。

## 6. 测 resident baseline

```bash
sudo "$OUT/page-state" "$PID" "$OUT/pages.hva" \
  > "$OUT/baseline.pagemap.tsv" 2> "$OUT/baseline.state"
grep -q 'present=262144 swapped=0 absent=0' "$OUT/baseline.state"
guest_command baseline
```

输出 first_total_ns/second_total_ns 均为 guest 内计时，除以 1e9 得秒。首次/第二次访问都是无回收对照，不含 SSH 往返。

## 7. 回收同一区域，然后触发恢复

```bash
ROUND=round1
test ! -e "$OUT/$ROUND.done"
sudo "$OUT/page-state" "$PID" "$OUT/pages.hva" \
  > "$OUT/$ROUND.before.pagemap.tsv" 2> "$OUT/$ROUND.before.state"
grep -q 'present=262144 swapped=0 absent=0' "$OUT/$ROUND.before.state"
snapshot "$OUT/$ROUND.before"

sudo ./memory-optimizer/task-refs -p "$PID" --scan idle-bitmap --backend zswap \
  --addresses "$OUT/pages.hva" --ram-range "$RAM_RANGE" \
  --bitmap-max-pages 262144 --bitmap-batch-bytes 4096 --bitmap-threads 1 \
  --max-pageout-pages 262144 -l 1 -i 1 -c 0 -m cold -o "$OUT/$ROUND.scan" \
  > "$OUT/$ROUND.pageout.log" 2>&1
grep -E '^pageout:|^pageout stats:' "$OUT/$ROUND.pageout.log"

sudo "$OUT/page-state" "$PID" "$OUT/pages.hva" \
  > "$OUT/$ROUND.swapped.pagemap.tsv" 2> "$OUT/$ROUND.swapped.state"
snapshot "$OUT/$ROUND.swapped"
cat "$OUT/$ROUND.swapped.state"
awk 'NR>1{s+=$3;n++}END{if(n!=262144 || s<262144*.999)exit 1}' \
  "$OUT/$ROUND.swapped.pagemap.tsv"

guest_command "$ROUND"

sudo "$OUT/page-state" "$PID" "$OUT/pages.hva" \
  > "$OUT/$ROUND.restored.pagemap.tsv" 2> "$OUT/$ROUND.restored.state"
snapshot "$OUT/$ROUND.restored"
grep -q 'present=262144 swapped=0 absent=0' "$OUT/$ROUND.restored.state"
awk 'NR==FNR{v[$1]=$2;next}{printf "%s delta=%d\n",$1,$2-v[$1]}' \
  "$OUT/$ROUND.swapped.vmstat" "$OUT/$ROUND.restored.vmstat"
```

回收 syscall 的成功字节数不是恢复样本数；用 swapped.pagemap.tsv 保存实际回收后状态。全区域整批计时允许至多 0.1% 的页已经 resident，并必须报告实际比例；需要严格全页冷启动时要求 present=0、swapped=262144。达不到门槛不触发恢复，先检查映射、候选、并发访问和 guest 服务。

Guest 随机打乱顺序，每页读取一个字节，立即再读一遍，两遍结束后才全页 hash 校验。第一遍是恢复访问，第二遍是即时热对照；两遍均包含访存和调度。后续校验/文件输出不在计时内。检查 errors=0、恢复后全部 present，以及 pswpin/pswpout/zswpwb 无增量。zswpin 是 host 全局计数，可能含其他 guest 活动。

## 8. 查看单页延迟（仅 latency 模式）

```bash
awk -F '\t' '
ARGIND==1{if(FNR>1)hva[$1]=$3;next}
ARGIND==2{if(FNR>1 && $3==1)swapped[$1]=1;next}
ARGIND==3 && FNR>1 && hva[$1] in swapped{a[++n]=$2;s+=$2}
END{
  if(!n)exit 1
  asort(a)
  printf "实际 swapped 样本 %d；mean %.3f us；P50 %.3f us；P95 %.3f us；P99 %.3f us\n",n,s/n/1000,a[int(n*.5+.999999)]/1000,a[int(n*.95+.999999)]/1000,a[int(n*.99+.999999)]/1000
}' "$OUT/mapping.tsv" "$OUT/$ROUND.swapped.pagemap.tsv" "$OUT/$ROUND.latency.tsv"
```

只纳入访问前实际 swapped 的页，避免 resident 页拉低均值。单页计时包含 clock_gettime 开销；整批模式避免逐页时钟采样，但仍包含循环。不要从端到端结果直接推导纯解压 CPU 时间。

## 9. 重复、换模式和清理

确认上一轮全部恢复后，设置 ROUND=round2，重复第 7/8 步，再做 round3。这里不需重启 guest 测试程序，因为上一轮读取和 hash 已恢复同一区域；每轮必须重新回收。

换 MODE 或 PATTERN 时先停止本次服务，然后从第 1 步用新 OUT、新 DIR、新 UNIT 创建进程并重做映射与 baseline。不要直接修改 shell 变量后继续用旧进程。

```bash
bash scripts/vm-ssh.sh "sudo systemctl stop '$UNIT'"
cat "$OUT/zswap.before" | sudo tee /sys/module/zswap/parameters/enabled
if [[ -f "$OUT/force_age_flush.before" ]]; then
  cat "$OUT/force_age_flush.before" | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
```

失败退出时也需执行本节清理，保留诊断文件；不要因为命令文件被消费就推断测量完成。最后按检测手册恢复最初 aging 开关并按需停止 59 GiB 填充负载。

历史 1 GiB 整批首次访问：sparse 约 3.787 s、mixed 2.058 s、random 1.414 s。random 在该内核 zswap 中按原大小保存，不需要普通解压路径，不能解释为“随机数据解压更快”。本实验测 guest 端到端恢复延迟，包含 VM exit、host 恢复、KVM 映射和调度，不含 host 冷页检测/回收时间。
