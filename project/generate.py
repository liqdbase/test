import pandas as pd
import sys
import os
import joblib
from build_dataset import extract_features  # 이전 feature 추출 함수

# 명령행 인자 확인


trace_file = "trace_test.txt"
if not os.path.exists(trace_file):
    print(f"입력 파일이 존재하지 않습니다: {trace_file}")
    sys.exit(1)

# 모델 및 인코더 불러오기

# Changed file paths to use raw strings (r"...") to avoid SyntaxError
model = joblib.load(r"C:\Users\qw011\Desktop\project\model\xgboost\label_encoder.pkl")
label_encoder = joblib.load(r"model/xgboost/xgb_model.pkl")


# 특징 추출
features = extract_features(trace_file)
X = pd.DataFrame([features])

# 예측
pred = model.predict(X)
pred_label = label_encoder.inverse_transform(pred)

print("입력 트레이스:", trace_file)
print(f"예측된 최적 캐시 정책 (BestPolicy): {pred_label[0]}")

