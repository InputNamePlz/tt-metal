# SPDX-FileCopyrightText: © 2023 Tenstorrent USA, Inc.

# SPDX-License-Identifier: Apache-2.0

import pkgutil
import sys
from importlib.util import module_from_spec

__all__ = []

# Windows bring-up scaffolding (TTNN_BUILD_FULL=OFF): the trimmed _ttnn extension
# only registers a subset of the op families, so operation modules whose C++
# bindings are missing are skipped instead of aborting the whole import.
import ttnn._ttnn

_TTNN_TRIMMED_BUILD = not hasattr(ttnn._ttnn.operations, "moreh")

for loader, module_name, is_pkg in pkgutil.walk_packages(__path__):
    spec = loader.find_spec(module_name)
    _module = module_from_spec(spec)
    # Register the module in sys.modules before executing it
    sys.modules[f"{module_name}"] = _module
    try:
        spec.loader.exec_module(_module)
    except AttributeError:
        if not _TTNN_TRIMMED_BUILD:
            raise
        # Trimmed build: this op family's C++ bindings are not present.
        del sys.modules[f"{module_name}"]
        continue
    __all__.append(module_name)
    globals()[module_name] = _module
