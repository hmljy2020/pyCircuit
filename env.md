# 本机固定开发环境

适用主机：Linux aarch64。工作树：`/home/lc/pyCircuit`。
初始路径来源：`/home/lc/pyCircuit-pr16-replay/AGENTS.md`。

## 使用方式

```bash
source /home/lc/opt/pycircuit-dev/activate.sh
```

激活脚本和编译器入口固定安装在 `/home/lc/opt/pycircuit-dev/`。
Python 使用原有固定环境，不使用临时 test-venv。
所有项目产物均从当前 checkout 构建，不复用其他 worktree 产物。

## 工具路径

| 工具 | 固定位置 |
| --- | --- |
| Clang / LLVM / MLIR 22.1.8 | `/home/lc/opt/llvm-22.1.8` |
| GCC 14 标准库、头文件和 sysroot | `/home/lc/opt/gcc14` |
| C/C++ 编译器入口 | `/home/lc/opt/pycircuit-dev/bin/cc`、`c++` |
| Python 3.11 | `/home/lc/opt/agentic-circuit-toolchain/python-env/bin/python` |
| CMake / Ninja | `/home/lc/opt/agentic-circuit-toolchain/bin` |
| Python lit 入口 | `/home/lc/opt/agentic-circuit-toolchain/python-env/bin/lit` |
| FileCheck / split-file / not / count | `/home/lc/opt/llvm-22.1.8/bin` |
| GTest | `/home/lc/opt/agentic-circuit-toolchain/gtest/lib64/cmake/GTest` |
| zstd 兼容依赖 | `/home/lc/opt/agentic-circuit-toolchain/cmake-deps/zstd-compat` |
| libxml2 头文件 | `/home/lc/opt/agentic-circuit-toolchain/cmake-deps/libxml2-include` |
| libxml2 库 | `/usr/lib64/libxml2.so.2` |

原始源码的 ARM NEON 调用在 GCC 14 下存在向量类型转换编译错误。
改用同一 LLVM 22 的 Clang，并明确使用 GCC 14 的标准库与 sysroot；
没有修改项目源码，也没有添加放宽类型检查的编译选项。
编译器入口自动附带：

```text
--gcc-install-dir=/home/lc/opt/gcc14/lib/gcc/aarch64-conda-linux-gnu/14.4.0
--sysroot=/home/lc/opt/gcc14/aarch64-conda-linux-gnu/sysroot
```

## 激活脚本内容

```bash
export PYC_LOCAL_ROOT=/home/lc/pyCircuit
export LLVM_PREFIX=/home/lc/opt/llvm-22.1.8
export ACIR_LOCAL_TOOLCHAIN=/home/lc/opt/agentic-circuit-toolchain
export ACIR_LOCAL_GCC=/home/lc/opt/gcc14
export PYC_LOCAL_BUILD_DIR="$PYC_LOCAL_ROOT/.pycircuit_out/local-clang22/build"
export PYC_LOCAL_INSTALL_DIR="$PYC_LOCAL_ROOT/.pycircuit_out/local-clang22/install"
export PYC_LOCAL_PYTHON="$ACIR_LOCAL_TOOLCHAIN/python-env/bin/python"
export CC=/home/lc/opt/pycircuit-dev/bin/cc
export CXX=/home/lc/opt/pycircuit-dev/bin/c++
export PATH="/home/lc/opt/pycircuit-dev/bin:$PYC_LOCAL_INSTALL_DIR/bin:$PYC_LOCAL_BUILD_DIR/bin:$LLVM_PREFIX/bin:$ACIR_LOCAL_TOOLCHAIN/bin:$ACIR_LOCAL_TOOLCHAIN/python-env/bin:/home/lc/opt/pycircuit-test-tools/bin:$ACIR_LOCAL_GCC/bin:$PATH"
export LD_LIBRARY_PATH="$ACIR_LOCAL_GCC/lib:$LLVM_PREFIX/lib:$PYC_LOCAL_INSTALL_DIR/lib:$PYC_LOCAL_INSTALL_DIR/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$PYC_LOCAL_ROOT/python/semantic-core/src:$PYC_LOCAL_ROOT/python/pycircuit/src:$PYC_LOCAL_ROOT/python/agentic-circuit/src:$PYC_LOCAL_ROOT/tests/python/agentic-circuit:$PYC_LOCAL_BUILD_DIR/compiler/acir/python${PYTHONPATH:+:$PYTHONPATH}"
export PYC_TOOLCHAIN_ROOT="$PYC_LOCAL_INSTALL_DIR"
export PYC_LOCAL_BUILD_JOBS=${PYC_LOCAL_BUILD_JOBS:-24}

export AC_GATE_TOOLCHAIN_ROOT="$PYC_LOCAL_INSTALL_DIR"
export ACIR_OPT="$PYC_LOCAL_BUILD_DIR/bin/acir-opt"
export ACIR_QUEUE_PLAN="$PYC_LOCAL_BUILD_DIR/bin/acir-queue-plan"
export ACIR_QUEUE_CXXGEN="$PYC_LOCAL_BUILD_DIR/bin/acir-queue-cxxgen"
```

## 配置、构建、安装

根据主机负载设置 `PYC_LOCAL_BUILD_JOBS`；脚本默认 24，可在激活前覆盖。
更换编译器后使用独立的 `local-clang22` 构建目录，避免混用 GCC 缓存。

```bash
"$ACIR_LOCAL_TOOLCHAIN/bin/cmake" \
  -S "$PYC_LOCAL_ROOT" -B "$PYC_LOCAL_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$PYC_LOCAL_INSTALL_DIR" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DACIR_PYTHON_INSTALL_DIR=lib/python3.11/site-packages \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_MAKE_PROGRAM="$ACIR_LOCAL_TOOLCHAIN/bin/ninja" \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DMLIR_DIR="$LLVM_PREFIX/lib/cmake/mlir" \
  -DGTest_DIR="$ACIR_LOCAL_TOOLCHAIN/gtest/lib64/cmake/GTest" \
  -Dzstd_INCLUDE_DIR="$ACIR_LOCAL_TOOLCHAIN/cmake-deps/zstd-compat/include" \
  -Dzstd_LIBRARY="$ACIR_LOCAL_TOOLCHAIN/cmake-deps/zstd-compat/lib/libzstd.a" \
  -Dzstd_STATIC_LIBRARY="$ACIR_LOCAL_TOOLCHAIN/cmake-deps/zstd-compat/lib/libzstd.a" \
  -DLIBXML2_INCLUDE_DIR="$ACIR_LOCAL_TOOLCHAIN/cmake-deps/libxml2-include" \
  -DLIBXML2_LIBRARY=/usr/lib64/libxml2.so.2 \
  -DPython3_EXECUTABLE="$PYC_LOCAL_PYTHON" \
  -DACIR_LIT_EXECUTABLE="$ACIR_LOCAL_TOOLCHAIN/python-env/bin/lit" \
  -DACIR_ENABLE_ASSERTIONS=ON \
  -DPYC_BUILD_MLIR_TOOLS=ON \
  -DPYC_BUILD_RUNTIME_LIB=ON \
  -DPYC_BUILD_AGENTIC_CIRCUIT=ON \
  -DPYC_BUILD_AGENTIC_CIRCUIT_TESTS=ON
"$ACIR_LOCAL_TOOLCHAIN/bin/cmake" --build "$PYC_LOCAL_BUILD_DIR" \
  --parallel "$PYC_LOCAL_BUILD_JOBS"
"$ACIR_LOCAL_TOOLCHAIN/bin/cmake" --build "$PYC_LOCAL_BUILD_DIR" \
  --target pyc-opt --parallel "$PYC_LOCAL_BUILD_JOBS"
"$ACIR_LOCAL_TOOLCHAIN/bin/cmake" --install "$PYC_LOCAL_BUILD_DIR"
```

## Agentic Circuit 测试入口

```bash
source /home/lc/opt/pycircuit-dev/activate.sh
python -m agentic_circuit._cli doctor --json
python -m unittest discover -s tests/python/agentic-circuit/python_frontend -p 'test_*.py'
python -m unittest discover -s tests/python/agentic-circuit/contracts -p 'test_*.py'
python -m unittest discover -s tests/python/agentic-circuit/cli -p 'test_*.py'
cmake --build "$PYC_LOCAL_BUILD_DIR" --target check-acir
ctest --test-dir "$PYC_LOCAL_BUILD_DIR" -R '^(GfsimTests|ACIRTypesTests|ACIROpsTests|ACSimTypesTests|ACSimOpsTests|ACIRModelAnalysisTests|ACIRProcessStatePlanTests|ACIRBindingTests|ACIRToACSimTests|CodeGenTests|CompilerTests|RuleRetirementE2ETests)$' --output-on-failure --parallel "$PYC_LOCAL_BUILD_JOBS"
python -m pytest tests/integration/agentic-circuit/e2e/test_typed_system_transactions.py -v
python -m pytest tests/integration/agentic-circuit/e2e/test_queue_codegen.py -q
```

部分测试硬编码 `.pycircuit_out/acir/dev-llvm22/`。本机在该目录建立了
`bin`、`python`、`gfsim` 符号链接，分别指向当前 checkout 的
`local-clang22/build/bin`、`local-clang22/build/compiler/acir/python`、
`local-clang22/build/compiler/acir/gfsim`，没有复制其他 worktree 的产物。

LLVM 测试辅助工具来自官方 `llvmorg-22.1.8` 源码，使用本机 LLVM 静态库编译。
Verilator 已在用户缩小范围前安装于 `/home/lc/opt/pycircuit-test-tools`，
本次最终验收只覆盖 Agentic Circuit，不要求 Verilator 或 PYC/RTL。

## 验证记录

当前源码未修改。构建安装、doctor、前端、契约、CLI、12 组原生/退休测试及
原子写入端到端测试已通过。ACIR lit 仍有 1 项失败，Queue 端到端仍有 5 项失败；
不能将本次验证表述为完整 AC G0/G1/G2 闭合。
命令、结果和已知失败记录在 `docs/gates/logs/20260907-main-ac-environment/`。
