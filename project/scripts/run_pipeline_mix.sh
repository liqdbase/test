#!/bin/bash

# 설정
NUM_WORKLOADS=1000
REQUESTS=10000
WRITE_RATIO=0.5
ZONE_SIZE=256
BUFFER_SIZE=6000
MAX_REQ=65536
TRACE_DIR="./traces"
OUTPUT_CSV="results.csv"
T="./t_mix"
SIM="./simulator"

POLICIES=(FIFO LRU LFU LRU_ARC LFU_ARC CLOCK_T1 CLOCK_T3 CLOCK_PRO_T1_B4_LOGS_B2 CLOCK_PRO_T3_B2_LOGS_B4)

mkdir -p $TRACE_DIR

# CSV 헤더 생성
echo -n "Trace" > $OUTPUT_CSV
for p in "${POLICIES[@]}"; do
    echo -n ",$p" >> $OUTPUT_CSV
done
echo ",BestPolicy" >> $OUTPUT_CSV

# 반복: 워크로드 생성 + 정책 실행
for i in $(seq 1 $NUM_WORKLOADS); do
    trace_file="$TRACE_DIR/trace_$i.txt"
    echo "[+] 워크로드 생성: $trace_file"
    $T $ZONE_SIZE $REQUESTS $WRITE_RATIO $trace_file

    echo -n "trace_$i" > tmp_row.csv

    best_rate=0
    best_policies=()

    for policy in "${POLICIES[@]}"; do
        output=$($SIM $BUFFER_SIZE $policy $trace_file $MAX_REQ 2>/dev/null)
        rate=$(echo "$output" | grep "Hit Rate" | awk '{print $3}' | tr -d '%')

        # 방어 처리
        if [[ -z "$rate" ]]; then rate="0.00"; fi

        echo -n ",$rate" >> tmp_row.csv

        rate_num=$(printf "%.2f" "$rate")
        if (( $(echo "$rate_num > $best_rate" | bc -l) )); then
            best_rate=$rate_num
            best_policies=("$policy")
        elif (( $(echo "$rate_num == $best_rate" | bc -l) )); then
            best_policies+=("$policy")
        fi
    done

    # 최고 정책 중 랜덤 선택
    best_policy=$(printf "%s\n" "${best_policies[@]}" | shuf -n 1)

    echo ",$best_policy" >> tmp_row.csv
    cat tmp_row.csv >> $OUTPUT_CSV
    echo "[✔] 저장 완료: trace_$i → $best_policy ($best_rate%)"
done

rm -f tmp_row.csv
echo " 전체 결과 CSV 저장 완료: $OUTPUT_CSV"
