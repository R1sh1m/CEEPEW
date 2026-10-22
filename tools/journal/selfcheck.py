"""Self-check for :mod:`stats_core`.

Run directly::

    python tools/journal/selfcheck.py

Plain ``assert`` statements only -- no pytest, no unittest, no fixtures.  Every
line printed as ``PASS`` carries the numbers that were actually computed, so the
transcript doubles as a reproducible record of the estimator outputs.
"""

from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:  # allow `python tools/journal/selfcheck.py`
    sys.path.insert(0, _HERE)

import stats_core as sc  # noqa: E402  (path bootstrap must run first)

_PASSED = 0


def _check(label: str, condition: bool, detail: str) -> None:
    """Assert ``condition`` and print a PASS line carrying ``detail``."""
    global _PASSED
    if not condition:
        raise AssertionError(f"FAILED: {label} :: {detail}")
    _PASSED += 1
    print(f"PASS  {label}: {detail}")


def _raises_value_error(fn, *args, **kwargs) -> bool:
    """Return True iff ``fn(*args, **kwargs)`` raises ValueError."""
    try:
        fn(*args, **kwargs)
    except ValueError:
        return True
    except Exception:  # pragma: no cover - any other error type is a failure
        return False
    return False


def check_wilson() -> None:
    lo, hi = sc.wilson_ci(16, 16)
    _check(
        "wilson_ci(16,16) lower in [0.79, 0.82]",
        0.79 <= lo <= 0.82,
        f"lo={lo:.12f}",
    )
    _check(
        "wilson_ci(16,16) upper == 1.0 exactly",
        hi == 1.0,
        f"hi={hi!r}",
    )

    lo2, hi2 = sc.wilson_ci(35, 38)
    # Correct value from the uncorrected Wilson score formula with
    # z = Phi^-1(0.975) = 1.959963984540054.
    ref_lo, ref_hi = 0.7920062854222853, 0.9727854044930122
    _check(
        "wilson_ci(35,38) matches uncorrected Wilson reference",
        abs(lo2 - ref_lo) < 5e-4 and abs(hi2 - ref_hi) < 5e-4,
        f"got=({lo2:.12f}, {hi2:.12f}) ref=({ref_lo:.12f}, {ref_hi:.12f})",
    )
    # The task brief quoted an approximate target of (0.787, 0.972) at tolerance
    # 0.005.  The correct uncorrected Wilson lower endpoint is 0.792006, which
    # misses that quoted target by 5.006e-3 -- just outside the tolerance.  The
    # reference value is asserted above; the deviation is reported, not fudged.
    dev_lo = abs(lo2 - 0.787)
    dev_hi = abs(hi2 - 0.972)
    print(
        f"NOTE  wilson_ci(35,38) vs brief target (0.787, 0.972): "
        f"|dlo|={dev_lo:.6f} (brief tolerance 0.005), |dhi|={dev_hi:.6f} -> "
        f"lower endpoint sits {dev_lo - 0.005:+.1e} outside the brief's tolerance; "
        f"computed value kept (see report)"
    )
    _check(
        "wilson_ci(35,38) upper agrees with brief within 0.005",
        dev_hi < 0.005,
        f"|dhi|={dev_hi:.6f}",
    )


def check_clopper_pearson() -> None:
    lo, hi = sc.clopper_pearson_ci(16, 16)
    _check(
        "clopper_pearson_ci(16,16) lower ~ 0.794 (tol 0.005)",
        abs(lo - 0.794) < 0.005,
        f"lo={lo:.12f}",
    )
    _check(
        "clopper_pearson_ci(16,16) upper == 1.0 exactly (k == n edge)",
        hi == 1.0,
        f"hi={hi!r}",
    )
    # analytic identity for k == n: lower = (alpha/2)^(1/n)
    analytic = (0.025) ** (1.0 / 16.0)
    _check(
        "clopper_pearson_ci(16,16) lower equals (alpha/2)^(1/n)",
        abs(lo - analytic) < 1e-10,
        f"beta={lo:.12f} analytic={analytic:.12f} |d|={abs(lo - analytic):.2e}",
    )

    lo0, hi0 = sc.clopper_pearson_ci(0, 20)
    _check(
        "clopper_pearson_ci(0,20) lower == 0.0 exactly (k == 0 edge)",
        lo0 == 0.0,
        f"lo={lo0!r}",
    )
    _check(
        "clopper_pearson_ci(0,20) upper ~ 0.1684 (tol 0.002)",
        abs(hi0 - 0.1684) < 0.002,
        f"hi={hi0:.12f}",
    )
    # analytic identity for k == 0: upper = 1 - (alpha/2)^(1/n)
    analytic_hi = 1.0 - (0.025) ** (1.0 / 20.0)
    _check(
        "clopper_pearson_ci(0,20) upper equals 1-(alpha/2)^(1/n)",
        abs(hi0 - analytic_hi) < 1e-10,
        f"beta={hi0:.12f} analytic={analytic_hi:.12f} |d|={abs(hi0 - analytic_hi):.2e}",
    )

    mid_lo, mid_hi = sc.clopper_pearson_ci(5, 10)
    _check(
        "clopper_pearson_ci(5,10) brackets p-hat and is ordered",
        mid_lo < 0.5 < mid_hi and 0.0 <= mid_lo <= mid_hi <= 1.0,
        f"=({mid_lo:.12f}, {mid_hi:.12f})",
    )


def check_describe() -> None:
    d = sc.describe([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    _check(
        "describe([1..10]).median == 5.5",
        d["median"] == 5.5,
        f"median={d['median']!r}",
    )
    _check(
        "describe([1..10]).q1 == 3.25",
        d["q1"] == 3.25,
        f"q1={d['q1']!r}",
    )
    _check(
        "describe([1..10]).q3 == 7.75 and iqr == 4.5",
        d["q3"] == 7.75 and d["iqr"] == 4.5,
        f"q3={d['q3']!r} iqr={d['iqr']!r}",
    )
    # sum of squared deviations about the mean for 1..10 is n(n^2-1)/12 = 82.5
    ref_sd = (82.5 / 9.0) ** 0.5
    _check(
        "describe([1..10]).sd is the n-1 sample sd",
        abs(d["sd"] - ref_sd) < 1e-12,
        f"sd={d['sd']:.12f} ref=sqrt(82.5/9)={ref_sd:.12f}",
    )
    one = sc.describe([7.5])
    _check(
        "describe(n=1) degrades gracefully",
        one["n"] == 1 and one["sd"] == 0.0 and one["median"] == 7.5 and one["iqr"] == 0.0,
        f"n={one['n']} sd={one['sd']!r} median={one['median']!r} iqr={one['iqr']!r}",
    )


def check_cluster_bootstrap() -> None:
    values = [1.0, 2.0, 3.0, 10.0, 11.0, 12.0, 100.0, 101.0, 102.0]
    clusters = ["c0", "c0", "c0", "c1", "c1", "c1", "c2", "c2", "c2"]
    a = sc.cluster_bootstrap_ci(values, clusters, stat="median", B=2000, seed=0)
    b = sc.cluster_bootstrap_ci(values, clusters, stat="median", B=2000, seed=0)
    c = sc.cluster_bootstrap_ci(values, clusters, stat="median", B=2000, seed=1)
    _check(
        "cluster_bootstrap_ci same seed -> identical result",
        a == b,
        f"seed=0 -> point={a['point']:.6f} lo={a['lo']:.6f} hi={a['hi']:.6f}",
    )
    _check(
        "cluster_bootstrap_ci different seed -> same point estimate",
        a["point"] == c["point"],
        f"point(seed0)={a['point']!r} point(seed1)={c['point']!r}",
    )
    _check(
        "cluster_bootstrap_ci reports B, G and seed",
        a["B"] == 2000 and a["n_clusters"] == 3 and a["seed"] == 0,
        f"B={a['B']} n_clusters={a['n_clusters']} seed={a['seed']} "
        f"seed1 CI=({c['lo']:.6f}, {c['hi']:.6f})",
    )
    _check(
        "cluster_bootstrap_ci interval brackets the point estimate",
        a["lo"] <= a["point"] <= a["hi"],
        f"lo={a['lo']:.6f} point={a['point']:.6f} hi={a['hi']:.6f}",
    )


def check_tost() -> None:
    equivalent_diffs = [0.10, -0.20, 0.05, 0.00, 0.15, -0.05, 0.08, -0.12, 0.02, 0.06]
    eq = sc.tost_paired(equivalent_diffs, bound=1.0)
    _check(
        "tost_paired clearly-equivalent sample -> equivalent=True",
        eq["equivalent"] is True,
        f"mean_diff={eq['mean_diff']:.6f} se={eq['se']:.6f} df={eq['df']:.1f} "
        f"t=({eq['t_lower']:.4f}, {eq['t_upper']:.4f}) p={eq['p']:.3e}",
    )

    different_diffs = [2.10, 1.90, 2.40, 2.20, 1.80, 2.05, 2.30, 1.95, 2.15, 2.00]
    neq = sc.tost_paired(different_diffs, bound=0.5)
    _check(
        "tost_paired clearly-different sample -> equivalent=False",
        neq["equivalent"] is False,
        f"mean_diff={neq['mean_diff']:.6f} se={neq['se']:.6f} df={neq['df']:.1f} "
        f"t=({neq['t_lower']:.4f}, {neq['t_upper']:.4f}) p={neq['p']:.6f}",
    )

    # Student-t CDF reference: t_{0.975, 10} = 2.2281388519649385
    cdf = sc._t_cdf(2.2281388519649385, 10.0)
    _check(
        "Student-t CDF reproduces t_{0.975,10}",
        abs(cdf - 0.975) < 5e-10,
        f"T_cdf(2.2281388519649385, 10)={cdf:.12f}",
    )
    _check(
        "Student-t CDF symmetry T(0, df) == 0.5",
        sc._t_cdf(0.0, 7.0) == 0.5,
        f"T_cdf(0, 7)={sc._t_cdf(0.0, 7.0)!r}",
    )


def check_holm() -> None:
    res = sc.holm_bonferroni([0.01, 0.04, 0.03])
    # Hand computation, m = 3.  Ascending: 0.01 (x3 -> 0.03), 0.03 (x2 -> 0.06),
    # 0.04 (x1 -> 0.04 -> raised to 0.06 by monotonicity).  Back to input order
    # [0.01, 0.04, 0.03] -> adjusted [0.03, 0.06, 0.06].
    _check(
        "holm_bonferroni([0.01,0.04,0.03]) adjusted == [0.03, 0.06, 0.06]",
        res["adjusted"] == [0.03, 0.06, 0.06],
        f"adjusted={res['adjusted']} (input order preserved)",
    )
    _check(
        "holm_bonferroni([0.01,0.04,0.03]) rejected == [True, False, False] at alpha=0.05",
        res["rejected"] == [True, False, False],
        f"rejected={res['rejected']} alpha={res['alpha']} m={res['m']} order={res['order']}",
    )
    _check(
        "holm_bonferroni is deterministic and single-element safe",
        sc.holm_bonferroni([0.2])["adjusted"] == [0.2],
        f"adjusted([0.2])={sc.holm_bonferroni([0.2])['adjusted']}",
    )


def check_failure_modes() -> None:
    cases = [
        ("wilson_ci(0, 0)", lambda: sc.wilson_ci(0, 0)),
        ("wilson_ci(5, 4)", lambda: sc.wilson_ci(5, 4)),
        ("wilson_ci(-1, 4)", lambda: sc.wilson_ci(-1, 4)),
        ("clopper_pearson_ci(3, 0)", lambda: sc.clopper_pearson_ci(3, 0)),
        ("describe([])", lambda: sc.describe([])),
        ("describe([nan])", lambda: sc.describe([float("nan")])),
        (
            "cluster_bootstrap_ci length mismatch",
            lambda: sc.cluster_bootstrap_ci([1.0, 2.0], ["a"], B=10),
        ),
        ("tost_paired non-positive bound", lambda: sc.tost_paired([0.1, 0.2], 0.0)),
        ("holm_bonferroni out-of-range p", lambda: sc.holm_bonferroni([1.5])),
    ]
    failures = [name for name, fn in cases if not _raises_value_error(fn)]
    _check(
        "invalid inputs raise ValueError (no silent zeros)",
        not failures,
        f"{len(cases)} cases checked, unexpected={failures}",
    )


def main() -> int:
    print("=== stats_core self-check ===")
    check_wilson()
    check_clopper_pearson()
    check_describe()
    check_cluster_bootstrap()
    check_tost()
    check_holm()
    check_failure_modes()
    print(f"=== ALL {_PASSED} CHECKS PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
