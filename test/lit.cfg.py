import os

import lit.formats
from lit.llvm import llvm_config
from lit.llvm.subst import FindTool, ToolSubst

# -- Suite identity -----------------------------------------------------------
config.name = "JSON-SCHEMA-MLIR"
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# `--split-input-file` chunks are separated by `// -----`.
config.suffixes = [".mlir"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.schema_obj_root, "test")

# -- Environment --------------------------------------------------------------
config.environment["FILECHECK_OPTS"] = "-enable-var-scope --allow-unused-prefixes=false"
llvm_config.with_system_environment(["HOME", "INCLUDE", "LIB", "TMP", "TEMP"])
llvm_config.use_default_substitutions()

# Exclude non-test inputs from discovery.
config.excludes = ["CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py", "Inputs"]

# -- Tool substitutions -------------------------------------------------------
tool_dirs = [config.schema_tools_dir, config.llvm_tools_dir]

tools = [
	ToolSubst("schema-opt", unresolved="fatal"),
	ToolSubst("%schema-opt", command=FindTool("schema-opt"), unresolved="fatal"),
	ToolSubst("mlir-opt", unresolved="ignore"),
	"FileCheck",
	"count",
	"not",
]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# Convenience: `// RUN: %schema_canonicalize %s | FileCheck %s`
config.substitutions.append(
	("%schema_canonicalize", "schema-opt --schema-canonicalize --split-input-file")
)

# Lowering suite substitutions.
config.substitutions.append(("%rt_prefix", "__schema_rt_"))
config.substitutions.append(
	("%lower_to_std", "schema-opt --lower-schema-to-std --split-input-file")
)
config.substitutions.append(
	("%lower_to_llvm", "schema-opt --schema-to-llvm-pipeline --split-input-file")
)
config.available_features.add("schema-lowering")