g++ -std=c++20 -c QuickView/main.cpp -I. -I./QuickView 2> compile_errors.txt || true
cat compile_errors.txt | head -n 30
