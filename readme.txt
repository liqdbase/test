

git clone --recursive https://github.com/dmlc/xgboost.git

cd xgboost

mkdir build

cd build

cmake ..

make -j$(nproc)

(설치)


export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
(세션마다 초기화)

gcc xg.c -o predictor     -I$XGBOOST_ROOT/include     -L$XGBOOST_ROOT/lib -lxgboost     -lm

 LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH ./predictor trace_test.txt output.txt


인공지능 선택시 기존 워크로드를 predictor로, predictor의 결과물인 output.txt를 기존의 test30으로 


