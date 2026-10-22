"""Journal-support layer for the CEE-PEW archival paper.

Two public modules:

* :mod:`stats_core`   -- dependency-free interval estimators and tests.
* :mod:`registry_tables` -- LaTeX table generation from ``results/registry.json``.

Nothing in this package mutates the registry, the firmware tree, or any
manuscript source.  It only reads and renders.
"""

__all__ = ["stats_core", "registry_tables"]
__version__ = "1.0.0"
