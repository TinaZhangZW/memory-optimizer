# 在目标机手动测量 64 GiB VM 冷页检测

三项实验依次为 [冷页检测](how-to-run.md)、[页面回收](../../page_reclaiming/doc/how-to-run.md)、[页面恢复](../../page_restoration/doc/how-to-run.md)。以下命令在 **10.17.173.249 的 host Bash** 中执行，guest 命令通过 vm-ssh.sh 运行。使用同一个终端保留变量。代码块中的 EOF 必须顶格，不能复制提示符。

本实验检测 guest RAM 的访问状态，不限定 guest 的可执行代码页；历史讨论中的 code page detection 在此指 cold page detection。

## 1. 编译并创建结果目录

```bash
cd /home/tina/workspace/poc/cpd
set -euo pipefail

uname -r
test -e /sys/kernel/mm/page_idle/bitmap
make -C memory-optimizer -j8 task-refs
gcc -O2 -Wall -Wextra memory-optimizer/tools/no-thp-exec.c -o /tmp/no-thp-exec
gcc -static -O2 -Wall -Wextra memory-optimizer/tests/guest-memory-fill.c -o /tmp/guest-memory-fill

CPD_OUT="$PWD/experiments/cold_pages_detection/results/manual-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$CPD_OUT"
echo "$CPD_OUT"
```

目标机之前测量使用 7.3.0-rc4+。若内核不同，应记录版本，不能直接当作相同环境。需要 gcc/g++、make、GNU awk、jq、nc，以及 sudo 权限；静态编译需要静态 C 库。不要在未确认内核配置时更换内核。

## 2. 启动或复用 VM

远端 scripts/vm.conf 的 QEMU 应为 /usr/local/bin/qemu-system-x86_64。当前实验磁盘、seed 和 SSH key 位于 run/linux-vm，已经准备好；不必重新创建镜像。

```bash
sudo env MEMORY=64G CPUS=8 /tmp/no-thp-exec bash scripts/vm-start.sh

PID=$(cat run/linux-vm/qemu.pid)
ps -p "$PID" -o pid,args
sudo cat "/proc/$PID/cmdline" | tr '\0' '\n'
grep THP_enabled "/proc/$PID/status"
bash scripts/vm-ssh.sh 'free -h; cat /proc/swaps'
```

已有 VM 时 vm-start.sh 会复用，不改变它的内存/CPU配置。确认参数包含 -m 64G、-smp 8，THP_enabled=0，guest swap 未启用。需要重建时先确认该 VM 可停止，再运行 sudo bash scripts/vm-stop.sh 和上面的启动命令。

## 3. 填充 guest 内存

先检查已有负载；不要在已经占用 59 GiB 时再启动一个相同负载：

```bash
bash scripts/vm-ssh.sh 'systemctl status idle-memory-fill --no-pager || true'
bash scripts/vm-ssh.sh 'sudo journalctl -u idle-memory-fill -n 10 --no-pager'
```

如果已有负载刚做过 host 回收，只访问热区不会恢复全部内存。重新填充时停止它，确认恢复测试服务也已停止：

```bash
bash scripts/vm-ssh.sh 'sudo systemctl stop idle-memory-fill'
```

首次启动或确认旧负载已停止后：

```bash
bash scripts/vm-ssh.sh \
  'cat > /tmp/guest-memory-fill && chmod +x /tmp/guest-memory-fill' < /tmp/guest-memory-fill

bash scripts/vm-ssh.sh 'bash -s' <<'EOF'
set -euo pipefail
available_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
gib=$((available_kib / 1048576 - 2))
if (( gib > 59 )); then gib=59; fi
if (( gib < 1 )); then echo '可用内存不足' >&2; exit 1; fi
echo "Populate ${gib} GiB"
sudo systemd-run --unit=idle-memory-fill \
  --property=LimitMEMLOCK=infinity /tmp/guest-memory-fill "$gib"
EOF

bash scripts/vm-ssh.sh 'sudo journalctl -u idle-memory-fill -n 20 --no-pager'
```

等待日志出现 ready pid=... resident_gib=59 hot_mib=256；Running as unit 不能证明填充完成。负载逐页写入非零值，mlock 防止 guest OS 换出，持续读取前 256 MiB，其余停止访问。若容量低于 59 GiB，应记录并检查 guest 的其他内存使用者。

## 4. 查询当前 RAMBlock，生成 HVA 列表

不要复用历史 PID/HVA。下面会自动读取本次 QMP 查询；若 timeout 返回 124，仍需检查完整返回内容。

```bash
{ printf '{"execute":"qmp_capabilities"}\n';
  printf '{"execute":"x-query-ramblock"}\n';
} | timeout 5 nc -U run/linux-vm/qmp.sock > "$CPD_OUT/qmp.json" || [[ $? == 124 ]]

jq -r '.return["human-readable-text"] // empty' "$CPD_OUT/qmp.json" > "$CPD_OUT/ramblocks.txt"
cat "$CPD_OUT/ramblocks.txt"
RAM_BASE=$(awk '$1=="ram" {print $7}' "$CPD_OUT/ramblocks.txt")
[[ $RAM_BASE == 0x* ]]
awk '$1=="ram" {if($5!="0x0000001000000000" || $6!="0x0000001000000000")exit 1;found=1}END{if(!found)exit 1}' "$CPD_OUT/ramblocks.txt"

RAM_BYTES=$((64 * 1024 * 1024 * 1024))
printf -v RAM_RANGE '%x:%x' "$((RAM_BASE))" "$((RAM_BASE + RAM_BYTES))"
printf 'PID=%s\nRAM_RANGE=%s\n' "$PID" "$RAM_RANGE" > "$CPD_OUT/ram.env"

awk -v base="$RAM_BASE" 'BEGIN {
  start=strtonum(base)
  for(i=0;i<16777216;i++)printf "0x%x\n",start+i*4096
}' > "$CPD_OUT/all-ram.hva"
wc -l "$CPD_OUT/all-ram.hva"
```

预期 16,777,216 个 HVA。列表覆盖 64 GiB，但实际 resident/eligible/valid 容量以扫描结果为准，不能由列表推断所有 EPT/NPT 映射均有效。

## 5. 记录并设置 aging 开关

目标机的实验内核提供 force_age_flush，之前 N/Y 两种状态都通过冷热页抽查。为与后续回收、恢复流程一致，这里使用 Y；不是所有 upstream 内核都提供此开关。

```bash
if [[ -e /sys/module/kvm/parameters/force_age_flush ]]; then
  ORIGINAL_FLUSH=$(cat /sys/module/kvm/parameters/force_age_flush)
  printf '%s\n' "$ORIGINAL_FLUSH" > "$CPD_OUT/force_age_flush.before"
  echo Y | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
{ uname -a; cat /proc/swaps; cat /sys/module/zswap/parameters/enabled;
  grep -E 'THP_enabled|VmRSS' "/proc/$PID/status";
} > "$CPD_OUT/environment.txt"
```

## 6. 运行单线程检测

```bash
sleep 10
RUN=first
sudo /usr/bin/time -f 'elapsed_s=%e user_s=%U system_s=%S max_rss_kib=%M' \
  -o "$CPD_OUT/$RUN.time" \
  ./memory-optimizer/task-refs -p "$PID" --scan idle-bitmap --backend none \
  --addresses "$CPD_OUT/all-ram.hva" --ram-range "$RAM_RANGE" \
  --bitmap-max-pages 16777216 --bitmap-batch-bytes 4096 --bitmap-threads 1 \
  -l 1 -i 1 -o "$CPD_OUT/$RUN" > "$CPD_OUT/$RUN.log" 2>&1
cat "$CPD_OUT/$RUN.log" "$CPD_OUT/$RUN.time"
```

运行时没有持续输出属正常，日志在结果目录。7.3.0-rc4+ 的历史整条命令约 57 秒，仅供估计。--backend none 不回收；全部写 bitmap 后等待 1 秒，再读取。各页标记时间不同，不共享严格相同的观察窗口。

```bash
awk -F '\t' '
NR==1{for(i=1;i<=NF;i++)c[$i]=i}
$1=="1"{
  if($(c["complete"])!=1 || $(c["write_errors"]) || $(c["read_errors"]) ||
     $(c["write_short_calls"]) || $(c["read_short_calls"]))exit 1
  w=$(c["write_wall_ns"])/1e9;r=$(c["read_wall_ns"])/1e9
  printf "有效容量 %.3f GiB；写 %.6f s；读 %.6f s；写读合计 %.6f s\n",$(c["valid_hvas"])/262144,w,r,w+r
  printf "pwrite %s 次；pread %s 次；idle %s 页；accessed %s 页\n",$(c["write_calls"]),$(c["read_calls"]),$(c["idle_hvas"]),$(c["accessed_hvas"])
}' "$CPD_OUT/$RUN.bitmap-stats.tsv"
```

写读时间不包含 HVA/PFN 查询、筛选、显式等待、后续复查和输出；elapsed_s 包含它们，不能相加。accessed 是观察到访问的页数，不是访问次数。有效容量应接近 60–61 GiB，明显偏小先检查负载、驻留和 THP；总 accessed 接近热区页数不是逐页正确性的证明。

重复时换 RUN=repeat1/ repeat2，并重新执行第 6 步的命令，避免覆盖。线程比较仅改 --bitmap-threads 为 2/4，顺序执行。PFN 稀疏时每次 I/O 可小于 4096 B；4096 是批上限，覆盖最多 32768 个 PFN。

## 7. 下一项实验、换终端和收尾

继续回收/恢复时保留 VM 和填充负载，记下 CPD_OUT。新终端先 cd 到远端根目录，再设置该实际目录并 source "$CPD_OUT/ram.env"；检查 PID 对应本实验 QEMU，VM 重启后必须重新查询 RAMBlock 和生成列表。

三项都结束后，按需恢复开关并停止负载：

```bash
if [[ -f "$CPD_OUT/force_age_flush.before" ]]; then
  cat "$CPD_OUT/force_age_flush.before" | sudo tee /sys/module/kvm/parameters/force_age_flush
fi
bash scripts/vm-ssh.sh 'sudo systemctl stop idle-memory-fill'
# 如需关闭 VM：sudo bash scripts/vm-stop.sh
```

OUT/CPD_OUT 为空会生成 /ram.env 之类错误路径；PID 为空会使 task-refs 打印帮助。启动等待 SSH 时查看 run/linux-vm/serial.log；host-only 静态网络无需 DHCP/DNS即可 SSH。若 guest wait-online 等 DNS，应配置对应的 networkd override，不必运行 dhclient。不要在已有 QEMU 运行时重复创建 TAP。
