import pandas as pd
import numpy as np
from sklearn.utils import resample

#  파일 로드
original_df = pd.read_csv("all_final_dataset.csv")
print(f"원본 샘플 수: {len(original_df)}")

#  1. 노이즈 기반 수치 증강
noise_df = original_df.copy()
noise_level = 0.01

for col in noise_df.columns[:-1]:  # 마지막 컬럼 제외 (BestPolicy)
    std = original_df[col].std()
    noise = np.random.normal(0, noise_level * std, size=len(original_df))
    noise_df[col] += noise

print(" 노이즈 기반 증강 완료")

#  2. 클래스 불균형 보정 (undersampled 클래스 upsampling)
class_counts = original_df['BestPolicy'].value_counts()
max_class_size = class_counts.max()

upsampled_list = []
for policy in class_counts.index:
    class_subset = original_df[original_df['BestPolicy'] == policy]
    upsampled = resample(class_subset, replace=True, n_samples=max_class_size, random_state=42)
    upsampled_list.append(upsampled)

balanced_df = pd.concat(upsampled_list)
print(" 클래스 균형 보정 완료")

#  3. 가상 혼합 샘플 생성
synth_size = 500
synth_A = original_df.sample(n=synth_size, random_state=1).reset_index(drop=True)
synth_B = original_df.sample(n=synth_size, random_state=2).reset_index(drop=True)
synth_mix = synth_A.copy()
synth_mix.iloc[:, :-1] = (synth_A.iloc[:, :-1] + synth_B.iloc[:, :-1]) / 2
synth_mix['BestPolicy'] = 'SYNTHETIC'

print(" 가상 혼합 샘플 생성 완료")

#  최종 병합
augmented_df = pd.concat([original_df, noise_df, balanced_df, synth_mix], ignore_index=True)
augmented_df.to_csv("final_dataset_augmented.csv", index=False)
print(f" 최종 증강 데이터셋 저장 완료: {len(augmented_df)} samples")
