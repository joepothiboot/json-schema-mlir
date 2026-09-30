"""lit test format for the json-schema-mlir suite.

Lives in its own module (not lit.cfg.py) because lit pickles the format to
hand tests to worker processes.
"""

import os
import shlex

import lit.formats
import lit.TestRunner


class QuotedPathShTest(lit.formats.ShTest):
    """ShTest that shell-quotes %s, %S, %p and %t.

    lit substitutes paths unquoted, so a checkout whose path contains a space
    splits every RUN line. lit applies a format's extra substitutions before
    the config's own, so the config's are repeated first to keep keys such as
    `%schema_canonicalize` from being eaten by `%s`. `shlex.quote` leaves
    ordinary paths unchanged.
    """

    def execute(self, test, litConfig):
        source = test.getSourcePath()
        source_dir = os.path.dirname(source)
        _, tmp_base = lit.TestRunner.getTempPaths(test)
        quoted = [
            ("%s", shlex.quote(source)),
            ("%S", shlex.quote(source_dir)),
            ("%p", shlex.quote(source_dir)),
            ("%t", shlex.quote(tmp_base + ".tmp")),
        ]
        return lit.TestRunner.executeShTest(
            test,
            litConfig,
            self.execute_external,
            list(test.config.substitutions) + quoted,
            self.preamble_commands,
        )
