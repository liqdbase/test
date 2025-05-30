import pickle
import pandas as pd # DataFrame 변환 예시를 위해 import

try:
    with open(r'C:\Users\qw011\Desktop\project\model\xgboost\xgb_model.pkl', 'rb') as f:
        loaded_object = pickle.load(f)
    # 이제 'loaded_object'는 pkl 파일로부터 로드된 파이썬 객체를 담고 있습니다.
    print(f"성공적으로 로드된 객체 타입: {type(loaded_object)}")
except FileNotFoundError:
    print("오류: PKL 파일을 찾을 수 없습니다.")
except pickle.UnpicklingError:
    print("오류: 피클링 해제 실패. 파일이 손상되었거나 유효한 피클 파일이 아닐 수 있습니다.")
except EOFError:
    print("오류: 예상치 못하게 파일 끝에 도달했습니다. 파일이 불완전할 수 있습니다.")
except Exception as e:
    print(f"예상치 못한 오류 발생: {e}")