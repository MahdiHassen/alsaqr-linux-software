#!/bin/bash
# Link every apmu-os component with kmod/apmu_link.c and with ld.lld at the same
# addresses and section order; the code and data must match byte for byte,
# except component_id (only our linker fills it in). Run in the build VM:
#   make test-link [APMU_OS=../../../apmu-os]
set -e
AOS=$(cd "${1:-../../../apmu-os}" && pwd); T=$(mktemp -d); trap 'rm -rf $T' EXIT
source "$AOS/source.sh" >/dev/null
cc -Wall -Wextra -O1 test/test_link.c kmod/apmu_link.c -o $T/link
nm() { $LLVM_BIN/llvm-nm "$AOS/build/output.elf" | awk -v s=$1 '$3==s{print "0x"$1}'; }
EXP="debug_printf $(nm debug_printf) queue_push_get_buffer $(nm queue_push_get_buffer) queue_push_buffer $(nm queue_push_buffer)"
TEXT=$((0x10427000 + 0xc00)); DATA=$((0x10429000 + 0x2000)); fail=0
for c in $(ls "$AOS/components"); do
  o=$AOS/build/$c.o
  $T/link $o $TEXT $DATA $T/t.bin $T/d.bin $EXP >/dev/null || { echo "FAIL $c: link"; fail=1; continue; }
  data=$($LLVM_BIN/llvm-readelf -SW $o | awk '/ (PROGBITS|NOBITS) /{f=$(NF-3); if (f ~ /A/ && f !~ /X/) for(i=1;i<=NF;i++) if($i ~ /^\./){print $i; break}}')
  { echo "SECTIONS {"; printf "  .text 0x%x : { *(.text) }\n" "$TEXT"
    printf "  .data 0x%x : {" $DATA; for s in $data; do printf " . = ALIGN(4); *(%s)" $s; done; echo " }"; echo "}"; } > $T/l.ld
  $LLVM_BIN/ld.lld -T $T/l.ld $(for s in debug_printf queue_push_get_buffer queue_push_buffer; do echo --defsym $s=$(nm $s); done) $o -o $T/ref.elf 2>/dev/null
  $LLVM_BIN/llvm-objcopy -O binary --only-section=.text $T/ref.elf $T/rt.bin
  $LLVM_BIN/llvm-objcopy -O binary --only-section=.data $T/ref.elf $T/rd.bin
  id=$(( $($LLVM_BIN/llvm-nm $T/ref.elf | awk '$3=="component_id"{print "0x"$1}') - DATA ))
  # Our data image may be longer than ld.lld's (trailing .bss); compare ld.lld's length.
  bad=$(cmp -l $T/rt.bin $T/t.bin 2>&1 | wc -l)
  bad=$((bad + $(cmp -l -n $(stat -c %s $T/rd.bin) $T/rd.bin $T/d.bin | awk -v id=$id '$1-1<id || $1-1>=id+4' | wc -l)))
  if [ $bad -eq 0 ]; then echo "ok   $c"; else echo "FAIL $c: $bad bytes differ"; fail=1; fi
done
exit $fail
