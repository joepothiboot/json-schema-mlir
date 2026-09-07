import os
import lit.formats
from lit.llvm import llvm_config

config.name = "JSON-SCHEMA-MLIR"
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)
config.suffixes = [".mlir"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.schema_obj_root, "test")

llvm_config.with_system_environment(["HOME", "INCLUDE", "LIB", "TMP", "TEMP"])
llvm_config.use_default_substitutions()

tool_dirs = [config.schema_tools_dir, config.llvm_tools_dir]
llvm_config.add_tool_substitutions(["schema-opt"], tool_dirs)