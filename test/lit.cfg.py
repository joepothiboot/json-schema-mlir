import os
import re
import shlex
import sys

import lit.formats
from lit.llvm import llvm_config
from lit.llvm.subst import FindTool, ToolSubst

# -- Suite identity -----------------------------------------------------------
config.name = "JSON-SCHEMA-MLIR"

# Quotes %s/%S/%p/%t so the suite runs from paths containing spaces.
sys.path.insert(0, os.path.dirname(__file__))
from schema_lit_format import QuotedPathShTest  # noqa: E402

config.test_format = QuotedPathShTest()

# `--split-input-file` chunks are separated by `// -----`. JSON Schema inputs
# live in `.test` files and are unpacked with `split-file`, because JSON has
# no comment syntax to carry RUN lines.
config.suffixes = [".mlir", ".test"]

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
	ToolSubst("schema-translate", unresolved="fatal"),
	ToolSubst("mlir-opt", unresolved="ignore"),
	"FileCheck",
	"count",
	"not",
	"split-file",
]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# Quote tool paths too, so they survive a checkout path containing spaces.
_tool_path = re.compile(
	"(?:%s)/[\\w.+-]+" % "|".join(re.escape(d) for d in tool_dirs if d)
)
config.substitutions = [
	(key, _tool_path.sub(lambda m: shlex.quote(m.group(0)), value))
	for key, value in config.substitutions
]

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