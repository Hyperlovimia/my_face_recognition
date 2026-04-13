#!/bin/sh
#
# 【重要】本脚本仅适合在 Linux/WSL 等完整 POSIX shell 下使用。
# 根据 archive/260408_RTSMART_SINGLE_UART_CHANGELOG.md：RT-Smart 板端 msh 不支持
# $(...)、${var:-def} 等常见 shell 语法，请勿在板子上执行本文件。
#
# 在 K230 / RT-Smart 上请直接在 msh 里运行 ELF，例如：
#   /data/fas_test.elf /data/face_antispoof.kmodel /data/test.jpg
#
# 在开发机上（交叉编译后）示例：
#   ./build/bin/fas_test.elf utils/face_antispoof.kmodel test.jpg
#或: FAS_TEST_ELF=/path/to/fas_test.elf ./utils/run_fas_test.sh ...
#

set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
BIN="${FAS_TEST_ELF:-${SCRIPT_DIR}/../build/bin/fas_test.elf}"
if [ ! -x "$BIN" ] && [ -x "${SCRIPT_DIR}/../k230_bin/fas_test.elf" ]; then
  BIN="${SCRIPT_DIR}/../k230_bin/fas_test.elf"
fi
exec "$BIN" "$@"
