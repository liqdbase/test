#!/bin/bash

# 설정 값
TRACE_FILE="trace_gen.txt"
BUFFER_SIZE=6000
MAX_REQUESTS=65536
SIMULATOR="./sim/simulator"
OUTPUT_CSV="results_trace_gen.csv"

# 사용할 정책 리스트
POLICIES=(FIFO LRU LFU LRU_ARC LFU_ARC CLOCK_T1 CLOCK_T3 CLOCK_PRO_T1_B4_LOGS_B2 CLOCK_PRO_T3_B2_LOGS_B4)

# CSV 헤더 생성
if [ ! -f "$OUTPUT_CSV" ]; then
    echo -n "Trace" > $OUTPUT_CSV
    for policy in "${POLICIES[@]}"; do
        echo -n ",$policy" >> $OUTPUT_CSV
    done
    echo ",BestPolicy" >> $OUTPUT_CSV
fi

# 새 행 시작
echo -n "$(basename $TRACE_FILE)" > tmp_row.csv

best_policy=""
best_rate=0

# 각 정책별 실행
for policy in "${POLICIES[@]}"; do
    # 실행 및 결과 캡처
    output=$($SIMULATOR $BUFFER_SIZE $policy $TRACE_FILE $MAX_REQUESTS)
    
    rate=$(echo "$output" | grep "Hit Rate" | awk '{print $3}' | tr -d '%')
    echo -n ",$rate" >> tmp_row.csv

    # 최고 히트율 갱신
    rate_num=$(printf "%.2f" "$rate")
    if (( $(echo "$rate_num > $best_rate" | bc -l) )); then
        best_rate=$rate_num
        best_policy=$policy
    fi
done

# 최종 결과 저장
echo ",$best_policy" >> tmp_row.csv
cat tmp_row.csv >> $OUTPUT_CSV

# 정리
rm -f tmp_row.csv

echo " CSV 저장 완료: $OUTPUT_CSV"
