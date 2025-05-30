import pandas as pd
import numpy as np
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import LabelEncoder
from sklearn.metrics import accuracy_score
from xgboost import XGBClassifier, plot_importance
from catboost import CatBoostClassifier, Pool
import matplotlib.pyplot as plt
import seaborn as sns
import joblib

# 데이터 로드
df = pd.read_csv("DATASET/final_dataset_augmented.csv")

# 목표 변수 (output)
y = df["BestPolicy"]
label_encoder = LabelEncoder()
y_encoded = label_encoder.fit_transform(y)

# 입력 특징 (CatBoost 중요도 기반 특성만 선택)
feature_names = [
    "read_ratio",
    "avg_reuse_distance",
    "max_reuse_distance",
    "access_locality",
    "unique_address_ratio",
    "entropy",
    "rw_switch_rate",
    "seq_access_ratio"
]
X = df[feature_names]

# 중요도 가중치
cat_importance = np.array([4.10, 7.63, 5.37, 4.44, 4.26, 8.73, 7.11, 8.06])  
weights = cat_importance / cat_importance.max()  # 정규화

# 가중치 적용
X_weighted = X * weights

# 학습/테스트 분할
X_train, X_test, y_train, y_test = train_test_split(X_weighted, y_encoded, test_size=0.2, stratify=y_encoded, random_state=42)

# ------------------------------------------
#   XGBoost 학습 + 중요도 분석
# ------------------------------------------
print(" XGBoost 학습 중...")
xgb_model = XGBClassifier(
    eval_metric="mlogloss",
    n_estimators=300,
    max_depth=6,
    learning_rate=0.05,
    subsample=0.8,
    colsample_bytree=0.8,
    random_state=42
)
xgb_model.fit(X_train, y_train)
xgb_preds = xgb_model.predict(X_test)
xgb_acc = accuracy_score(y_test, xgb_preds)
print(f" XGBoost Accuracy: {xgb_acc:.4f}")

# s XGBoost Feature Importance
plt.figure(figsize=(10,6))
plot_importance(xgb_model, importance_type='gain')
plt.title('XGBoost Feature Importance')
plt.xlabel('Importance (gain)')
plt.tight_layout()
plt.savefig("xgboost_importance_weighted.png")
plt.show()

# ------------------------------------------
#   CatBoost 학습 + 중요도 분석
# ------------------------------------------
print(" CatBoost 학습 중...")
cat_model = CatBoostClassifier(
    iterations=300,
    depth=6,
    learning_rate=0.05,
    l2_leaf_reg=3,
    verbose=0,
    random_state=42
)
cat_model.fit(X_train, y_train)
cat_preds = cat_model.predict(X_test)
cat_acc = accuracy_score(y_test, cat_preds)
print(f" CatBoost Accuracy: {cat_acc:.4f}")

#  CatBoost Feature Importance 
cat_importance_new = cat_model.get_feature_importance(Pool(X_train, y_train))

plt.figure(figsize=(10,6))
sns.barplot(x=cat_importance_new, y=feature_names)
plt.title("CatBoost Feature Importance (weighted)")
plt.xlabel("Importance")
plt.tight_layout()
plt.savefig("catboost_importance_weighted.png")
plt.show()


joblib.dump(xgb_model, "xgb_model.pkl")
joblib.dump(label_encoder, "label_encoder.pkl")
