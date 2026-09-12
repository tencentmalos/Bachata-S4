fails=0
for i in $(seq 1 10); do
  adb -s 9c2841a4 shell "timeout 120 /data/local/tmp/g3_n2/guest; echo __EXIT__=\$?" > /tmp/g3-r0-repro/n2r$i.txt 2>&1
  ec=$(grep -o '__EXIT__=[0-9]*' /tmp/g3-r0-repro/n2r$i.txt|tail -1)
  chk=$(grep -oE 'ALL PASS|[0-9]+ failure' /tmp/g3-r0-repro/n2r$i.txt|head -1)
  g24=$(grep -cE '\[G24[ab].*FAIL|\[g24\] FAIL' /tmp/g3-r0-repro/n2r$i.txt)
  echo "run$i: $ec $chk g24fails=$g24"
  [ "$ec" != "__EXIT__=0" ] && fails=$((fails+1))
  [ "$g24" != "0" ] && fails=$((fails+1))
done
echo "TOTAL_FAILURES=$fails"; echo N2STRESS_DONE
