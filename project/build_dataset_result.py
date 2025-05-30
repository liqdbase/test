import pandas as pd
import numpy as np
from collections import Counter
import os

def extract_features(trace_path):
    with open(trace_path) as f:
        lines = f.readlines()

    lbas = []
    ops = []

    for line in lines:
        line = line.strip()
        if line == "" or line.startswith("#"): continue

        parts = line.split()
        if len(parts) != 2:
            print(f" 무시된 라인: '{line}'")
            continue

        try:
            lba = int(parts[0])
            op = parts[1].upper()
            if op not in ("R", "W"):
                print(f" 잘못된 작업 코드 무시: '{op}'")
                continue
        except Exception as e:
            print(f" 파싱 실패: {line}")
            continue

        lbas.append(lba)
        ops.append(op)


    total = len(lbas)
    if total == 0:
        return {k: 0 for k in [
            "read_ratio", "avg_reuse_distance", "max_reuse_distance",
            "access_locality", "unique_address_ratio", "entropy",
            "rw_switch_rate", "seq_access_ratio"
        ]}

    read_ratio = ops.count("R") / total
    rw_switch_count = sum(1 for i in range(1, total) if ops[i] != ops[i-1])
    rw_switch_rate = rw_switch_count / (total - 1)

    last_seen = {}
    reuse_distances = []
    seq_count = 0
    for i, lba in enumerate(lbas):
        if lba in last_seen:
            reuse_distances.append(i - last_seen[lba])
        last_seen[lba] = i

        if i > 0 and abs(lba - lbas[i-1]) in (1, 2):  # stride=1 or 2
            seq_count += 1

    avg_reuse = np.mean(reuse_distances) if reuse_distances else 0
    max_reuse = np.max(reuse_distances) if reuse_distances else 0

    unique_addresses = len(set(lbas))
    unique_ratio = unique_addresses / total
    locality = 1 - unique_ratio

    cnt = Counter(lbas)
    probs = [c / total for c in cnt.values()]
    entropy = -sum(p * np.log2(p) for p in probs if p > 0)

    seq_access_ratio = seq_count / (total - 1)

    return {
        "read_ratio": round(read_ratio, 4),
        "avg_reuse_distance": round(avg_reuse, 2),
        "max_reuse_distance": max_reuse,
        "access_locality": round(locality, 4),
        "unique_address_ratio": round(unique_ratio, 4),
        "entropy": round(entropy, 4),
        "rw_switch_rate": round(rw_switch_rate, 4),
        "seq_access_ratio": round(seq_access_ratio, 4)
    }

# ----------------------------
# 메인 로직: result.csv 기반 구성
# ----------------------------
results_df = pd.read_csv("results.csv")

#  Trace 이름 정규화 함수
def normalize_trace_name(name):
    name = name.strip().lower()
    if not name.endswith(".txt"):
        name += ".txt"
    return name

#  Trace 정규화 적용
results_df["Trace"] = results_df["Trace"].apply(normalize_trace_name)

# features 추출
features = []
# build_dataset.py 일부 수정
for trace in results_df["Trace"]:
    # 보정: .txt 확장자 없으면 추가
    if not trace.endswith(".txt"):
        trace += ".txt"
    path = f"traces/{trace}"
    
    # 경로 존재 확인 (방어)
    if not os.path.exists(path):
        print(f" 파일 없음: {path}, 기본값으로 채움")
        feats = {k: 0 for k in [
            "read_ratio", "avg_reuse_distance", "max_reuse_distance",
            "access_locality", "unique_address_ratio", "entropy",
            "rw_switch_rate", "seq_access_ratio"
        ]}
        feats["Trace"] = trace
        features.append(feats)
        continue
    
    feats = extract_features(path)
    feats["Trace"] = trace
    features.append(feats)


features_df = pd.DataFrame(features)

# 병합
merged = pd.merge(features_df, results_df[["Trace", "BestPolicy"]], on="Trace")
print(f" 병합된 행 수: {len(merged)}")
if len(merged) == 0:
    print(" 병합 실패: Trace 이름 불일치 가능성 있음")
    print("예시 (features_df):", features_df["Trace"].unique()[:3])
    print("예시 (results_df):", results_df["Trace"].unique()[:3])

# 저장
final_df = merged.drop(columns=["Trace"])
final_df.to_csv("final_dataset.csv", index=False)
print(" 머신러닝용 최종 데이터셋 저장 완료: final_dataset.csv")

