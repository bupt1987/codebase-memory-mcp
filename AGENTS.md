# AGENTS.md

## 语言与执行约定

- 默认使用中文回复用户。
- 优先使用 `rg` / `rg --files` 搜索代码和文件。
- 工作区可能已有用户改动；不要回滚、覆盖或清理无关改动。

## 项目概况

这是 `codebase-memory-mcp`，当前主实现是纯 C 二进制。

主要目录：

- `src/`：核心 C 实现，包括 foundation、store、cypher、mcp、pipeline、discover、watcher、cli、ui。
- `internal/cbm/`：tree-sitter AST 提取、语言规格、旧抽取层和 vendored tree-sitter runtime。
- `vendored/`：sqlite3、yyjson、mongoose、mimalloc、xxhash、tre 等内嵌依赖。
- `tests/`：C 测试。
- `scripts/`：构建、测试、lint、发布辅助脚本。
- `graph-ui/`：React / Three.js 图可视化前端。
- `pkg/go`、`pkg/pypi`、`pkg/npm`：各语言包发布/绑定相关内容。

除非任务明确要求改包发布或前端，否则优先在 C 侧目录里定位问题。

## 构建与测试

推荐使用项目脚本作为完整构建/测试入口：

```bash
scripts/build.sh
scripts/test.sh
scripts/lint.sh
```

`scripts/build.sh` 和 `scripts/test.sh` 会读取 `scripts/env.sh`，自动选择架构、编译器和并行度。
其中 `scripts/test.sh` 最终会执行 `make -j"$NPROC" -f Makefile.cbm test`；`NPROC` 由 `scripts/env.sh` 自动检测 CPU 数，检测失败时默认是 `4`，也就是 `-j4`。需要手动覆盖时可以用 `NPROC=8 scripts/test.sh`。

需要直接调用 C Makefile 时，使用 `Makefile.cbm`：

```bash
make -f Makefile.cbm cbm
make -f Makefile.cbm test
make -f Makefile.cbm test-foundation
```

### 并行 make

`Makefile.cbm` 的编译目标可以并行执行。构建较慢的测试 runner 时，优先使用本机 CPU 数作为并行度：

```bash
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$NPROC" -f Makefile.cbm build/c/test-runner
```

如果只想手动指定并行度：

```bash
make -j8 -f Makefile.cbm build/c/test-runner
```

说明：

- `-j` 控制并行 job 数。
- 如果瓶颈在单个最终链接步骤，`-j` 对最后阶段帮助有限，但能并行编译依赖对象。

## 本地打包约定

本地打包只打 macOS arm64 标准包，除非用户明确要求，不要打 Linux、Windows、x86_64 或 UI 包。

产物要求：

- 目标平台：`darwin-arm64`
- 压缩格式：`.tar.xz`
- 输出目录：`bin/`
- 推荐文件名：`bin/codebase-memory-mcp-darwin-arm64.tar.xz`

推荐打包命令：

```bash
scripts/build.sh --arch arm64
codesign --sign - --force build/c/codebase-memory-mcp
cp LICENSE install.sh build/c/
mkdir -p bin
tar -cJf bin/codebase-memory-mcp-darwin-arm64.tar.xz -C build/c codebase-memory-mcp LICENSE install.sh
shasum -a 256 bin/codebase-memory-mcp-darwin-arm64.tar.xz
```

不要沿用 CI 发布包的 `.tar.gz` 后缀；本地 mac arm64 包固定使用 `.tar.xz`。

## 开发注意事项

- 这是纯 C 主项目；不要把核心实现改回 Go。
- 改语言支持时，优先检查 `internal/cbm/lang_specs.c`、`internal/cbm/extract_*.c` 和 `src/pipeline/pass_*.c`。
- 改存储或查询行为时，重点检查 `src/store/`、`src/cypher/`、`src/mcp/` 和相关 `tests/test_store_*.c`、`tests/test_mcp.c`。
- 改 pipeline 行为时，重点检查 `src/pipeline/` 和 `tests/test_pipeline.c`、`tests/test_integration.c`、`tests/test_incremental.c`。
- 改 UI 嵌入或可视化时，检查 `graph-ui/`、`src/ui/` 和 `Makefile.cbm` 的 `cbm-with-ui` / `embed` 目标。

## 验证原则

- 小改动至少运行相关最小测试。
- 最后的全量验证统一使用 `scripts/test.sh`；不要单独运行 `build/c/test-runner` 作为最终完成标准。
- 影响 C 核心、pipeline、store 或 MCP 行为时，优先运行：

```bash
scripts/test.sh
```

- 只需要快速构建测试二进制时，使用并行 make：

```bash
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$NPROC" -f Makefile.cbm build/c/test-runner
```
