"""CEE-PEW measurement campaign tooling.

Every quantitative claim in the manuscript must be traceable to a sealed
campaign manifest. The modules in this package implement that chain:

    manifest.py        emit and verify a sealed campaign manifest
    registry_append.py append registry entries that are bound to a manifest
    run_x1_timing.py   per-stage timing analysis with cluster-bootstrap CIs
    run_x8_delivery.py delivery accounting with an explicit drop breakdown

Pure standard library. No network access. No third-party dependencies.
"""

__all__ = ["manifest", "registry_append"]
