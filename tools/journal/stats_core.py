"""Dependency-free statistics primitives for the CEE-PEW archival paper.

Standard library only (``math``, ``random``, ``statistics``, ``json``).  No
numpy, no scipy, no compiled extensions.  Every estimator here is the exact
estimator quoted in the paper; the docstring of each public function names the
estimator, states its assumptions, and says that this is the definition used in
the paper.

Conventions
-----------
* Two-sided intervals are central intervals at ``level`` (default 0.95), i.e.
  tail mass ``(1 - level) / 2`` in each tail.
* ``n <= 0``, ``k < 0``, ``k > n``, empty samples, and non-finite inputs raise
  :class:`ValueError`.  Nothing is silently coerced to zero.
* No module-level mutable state: every function is pure apart from the explicit
  RNG seeded inside :func:`cluster_bootstrap_ci`.
"""

from __future__ import annotations

import math
import random
from typing import Callable, Iterable, Sequence

__all__ = [
    "wilson_ci",
    "clopper_pearson_ci",
    "describe",
    "cluster_bootstrap_ci",
    "tost_paired",
    "holm_bonferroni",
]

# Numerical constants for the incomplete-beta kernel (Numerical Recipes 3rd ed.,
# section 6.4).  EPS is at the double-precision floor; MAX_ITER is generous
# because the continued fraction converges in <30 iterations for every shape
# parameter pair used here.
_BETA_EPS = 3.0e-16
_BETA_FPMIN = 1.0e-300
_BETA_MAX_ITER = 500
_BISECT_MAX_ITER = 300
_BISECT_TOL = 1.0e-15


# ---------------------------------------------------------------------------
# argument validation
# ---------------------------------------------------------------------------
def _validate_level(level: float) -> float:
    """Return ``level`` as a float after checking ``0 < level < 1``."""
    lvl = float(level)
    if not math.isfinite(lvl) or not (0.0 < lvl < 1.0):
        raise ValueError(f"level must satisfy 0 < level < 1, got {level!r}")
    return lvl


def _validate_counts(k: int, n: int) -> tuple[int, int]:
    """Return ``(k, n)`` as ints after checking ``0 <= k <= n`` and ``n > 0``."""
    if isinstance(n, bool) or isinstance(k, bool):
        raise ValueError("k and n must be integers, not booleans")
    n_int = int(n)
    k_int = int(k)
    if n_int != n or k_int != k:
        raise ValueError(f"k and n must be integral, got k={k!r}, n={n!r}")
    if n_int <= 0:
        raise ValueError(f"n must be a positive integer, got n={n!r}")
    if k_int < 0 or k_int > n_int:
        raise ValueError(f"k must satisfy 0 <= k <= n, got k={k!r}, n={n!r}")
    return k_int, n_int


def _as_floats(values: Iterable[float], what: str = "sample") -> list[float]:
    """Materialise ``values`` as a non-empty list of finite floats."""
    if isinstance(values, (str, bytes)):
        raise ValueError(f"{what} must be a sequence of numbers, not a string")
    try:
        raw = list(values)
    except TypeError as exc:  # pragma: no cover - defensive
        raise ValueError(f"{what} must be iterable, got {type(values).__name__}") from exc
    if not raw:
        raise ValueError(f"{what} is empty; at least one observation is required")
    out: list[float] = []
    for i, v in enumerate(raw):
        f = float(v)
        if not math.isfinite(f):
            raise ValueError(f"{what}[{i}] is not finite: {v!r}")
        out.append(f)
    return out


# ---------------------------------------------------------------------------
# normal distribution
# ---------------------------------------------------------------------------
def _norm_cdf(z: float) -> float:
    """Standard normal CDF via ``erf``."""
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def _norm_ppf(p: float) -> float:
    """Standard normal quantile function by bisection on :func:`_norm_cdf`.

    Bisection is used instead of a rational approximation so that the result is
    correct to ~1e-15 for every tail probability, at the cost of ~150
    evaluations.  Monotone, deterministic, and free of tuning constants.
    """
    if not (0.0 < p < 1.0):
        raise ValueError(f"p must satisfy 0 < p < 1, got {p!r}")
    lo, hi = -40.0, 40.0
    for _ in range(_BISECT_MAX_ITER):
        mid = 0.5 * (lo + hi)
        if _norm_cdf(mid) < p:
            lo = mid
        else:
            hi = mid
        if hi - lo < _BISECT_TOL:
            break
    return 0.5 * (lo + hi)


# ---------------------------------------------------------------------------
# regularized incomplete beta
# ---------------------------------------------------------------------------
def _beta_continued_fraction(a: float, b: float, x: float) -> float:
    """Continued fraction for the incomplete beta function (Lentz's method)."""
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < _BETA_FPMIN:
        d = _BETA_FPMIN
    d = 1.0 / d
    h = d
    for m in range(1, _BETA_MAX_ITER + 1):
        m2 = 2 * m
        # even step
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < _BETA_FPMIN:
            d = _BETA_FPMIN
        c = 1.0 + aa / c
        if abs(c) < _BETA_FPMIN:
            c = _BETA_FPMIN
        d = 1.0 / d
        h *= d * c
        # odd step
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < _BETA_FPMIN:
            d = _BETA_FPMIN
        c = 1.0 + aa / c
        if abs(c) < _BETA_FPMIN:
            c = _BETA_FPMIN
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < _BETA_EPS:
            break
    return h


def regularized_incomplete_beta(a: float, b: float, x: float) -> float:
    """``I_x(a, b)``, the regularized incomplete beta function.

    Evaluated as the lgamma-scaled front factor times the Lentz continued
    fraction, switching to the ``1 - I_{1-x}(b, a)`` symmetry whenever the
    continued fraction would otherwise converge slowly (``x`` above the
    standard crossover ``(a + 1) / (a + b + 2)``).
    """
    a_f = float(a)
    b_f = float(b)
    x_f = float(x)
    if not (math.isfinite(a_f) and math.isfinite(b_f)) or a_f <= 0.0 or b_f <= 0.0:
        raise ValueError(f"shape parameters must be finite and positive, got a={a!r}, b={b!r}")
    if not math.isfinite(x_f) or not (0.0 <= x_f <= 1.0):
        raise ValueError(f"x must lie in [0, 1], got x={x!r}")
    if x_f == 0.0:
        return 0.0
    if x_f == 1.0:
        return 1.0
    log_beta = math.lgamma(a_f + b_f) - math.lgamma(a_f) - math.lgamma(b_f)
    front = math.exp(log_beta + a_f * math.log(x_f) + b_f * math.log1p(-x_f))
    if x_f < (a_f + 1.0) / (a_f + b_f + 2.0):
        value = front * _beta_continued_fraction(a_f, b_f, x_f) / a_f
    else:
        value = 1.0 - front * _beta_continued_fraction(b_f, a_f, 1.0 - x_f) / b_f
    # guard against the last-bit excursions of the continued fraction
    return min(1.0, max(0.0, value))


def _beta_ppf(q: float, a: float, b: float) -> float:
    """Quantile of the Beta(a, b) distribution: solve ``I_x(a, b) = q``.

    Monotone bisection on :func:`regularized_incomplete_beta`, which is
    continuous and strictly increasing on (0, 1).  Deterministic and free of
    initial-guess heuristics, so the result is the same on every platform.
    """
    if not (0.0 < q < 1.0):
        raise ValueError(f"quantile probability must satisfy 0 < q < 1, got {q!r}")
    lo, hi = 0.0, 1.0
    for _ in range(_BISECT_MAX_ITER):
        mid = 0.5 * (lo + hi)
        if regularized_incomplete_beta(a, b, mid) < q:
            lo = mid
        else:
            hi = mid
        if hi - lo < _BISECT_TOL:
            break
    return 0.5 * (lo + hi)


# ---------------------------------------------------------------------------
# binomial proportion intervals
# ---------------------------------------------------------------------------
def wilson_ci(k: int, n: int, level: float = 0.95) -> tuple[float, float]:
    """Wilson score interval for a binomial proportion, no continuity correction.

    Estimator (Wilson 1927, score inversion of the normal approximation)::

        centre = (p + z^2 / (2n)) / (1 + z^2 / n)
        half   = z / (1 + z^2 / n) * sqrt(p(1-p)/n + z^2 / (4n^2))

    with ``p = k/n`` and ``z = Phi^{-1}(1 - (1-level)/2)``.  Continuity
    correction is deliberately OFF, so this is the plain (unshifted) score
    interval.  No exactness is claimed at the boundary: the endpoints are
    clamped to [0, 1], and the degenerate cases ``k == 0`` / ``k == n`` are
    forced to the exact analytic endpoints rather than to their floating-point
    images.

    Assumptions: independent Bernoulli trials with a common success probability;
    the nominal level is the large-sample normal-approximation level, which is
    only approximately attained for small ``n``.

    This is the Wilson interval definition used in the paper.
    """
    k_int, n_int = _validate_counts(k, n)
    lvl = _validate_level(level)
    z = _norm_ppf(1.0 - (1.0 - lvl) / 2.0)
    p = k_int / n_int
    denom = 1.0 + z * z / n_int
    centre = (p + z * z / (2.0 * n_int)) / denom
    half = (z / denom) * math.sqrt(p * (1.0 - p) / n_int + z * z / (4.0 * n_int * n_int))
    lo = centre - half
    hi = centre + half
    # k == 0 and k == n have exact analytic endpoints; pin them so callers can
    # test equality without a tolerance.
    lo = 0.0 if k_int == 0 else lo
    hi = 1.0 if k_int == n_int else hi
    return (min(1.0, max(0.0, lo)), min(1.0, max(0.0, hi)))


def clopper_pearson_ci(k: int, n: int, level: float = 0.95) -> tuple[float, float]:
    """Clopper-Pearson (exact beta) interval for a binomial proportion.

    Estimator: invert the equal-tailed binomial test.  With ``alpha = 1 - level``,

        lower = 0                                   if k == 0
              = Beta^{-1}(alpha/2;     k,     n-k+1) otherwise
        upper = 1                                   if k == n
              = Beta^{-1}(1 - alpha/2; k+1,   n-k)   otherwise

    where ``Beta^{-1}(q; a, b)`` is the q-quantile of the Beta distribution,
    obtained by bisection on the regularized incomplete beta function.  The
    resulting interval has coverage ``>= level`` for every true proportion
    (conservative), which is why the paper quotes it alongside Wilson.

    Assumptions: independent Bernoulli trials with a common success probability.
    No continuity correction; endpoints are exact at ``k == 0`` and ``k == n``.

    This is the Clopper-Pearson interval definition used in the paper.
    """
    k_int, n_int = _validate_counts(k, n)
    lvl = _validate_level(level)
    alpha = 1.0 - lvl
    lo = 0.0 if k_int == 0 else _beta_ppf(alpha / 2.0, float(k_int), float(n_int - k_int + 1))
    hi = 1.0 if k_int == n_int else _beta_ppf(1.0 - alpha / 2.0, float(k_int + 1), float(n_int - k_int))
    return (min(1.0, max(0.0, lo)), min(1.0, max(0.0, hi)))


# ---------------------------------------------------------------------------
# descriptive statistics
# ---------------------------------------------------------------------------
def _quantile_linear(sorted_values: Sequence[float], q: float) -> float:
    """Quantile at ``q`` by linear interpolation between order statistics.

    This is the ``numpy.quantile(..., method="linear")`` / R type-7 definition:
    ``h = (n - 1) * q``, interpolate between ``x[floor(h)]`` and
    ``x[ceil(h)]`` with weight ``h - floor(h)``.
    """
    n = len(sorted_values)
    if n == 1:
        return float(sorted_values[0])
    h = (n - 1) * q
    lower = int(math.floor(h))
    upper = min(lower + 1, n - 1)
    frac = h - lower
    return float(sorted_values[lower]) + frac * (float(sorted_values[upper]) - float(sorted_values[lower]))


def describe(values: Iterable[float]) -> dict:
    """Descriptive summary of a sample.

    Returns ``n, mean, sd, min, q1, median, q3, iqr, p95, max``.

    Estimators, exactly as used in the paper:

    * ``mean`` -- arithmetic mean, ``sum(x) / n``.
    * ``sd``   -- sample standard deviation with the ``n - 1`` (Bessel)
      denominator.  For ``n == 1`` the sample sd is undefined; this function
      returns ``0.0`` and the single value for every quantile, so that a
      one-observation pilot run is representable rather than fatal.
    * quantiles -- the linear-interpolation (type-7) definition described in
      :func:`_quantile_linear`; ``p95`` is the 0.95 quantile.
    * ``iqr``  -- ``q3 - q1``.

    Assumptions: the sample is treated as an i.i.d. draw from the population of
    interest; no distributional assumption is made.  The mean and sd are
    moment estimators and are not robust; the median and IQR are reported
    alongside them for that reason.
    """
    vals = _as_floats(values)
    vals.sort()
    n = len(vals)
    mean = sum(vals) / n
    if n > 1:
        var = sum((v - mean) ** 2 for v in vals) / (n - 1)
        sd = math.sqrt(var)
    else:
        sd = 0.0
    q1 = _quantile_linear(vals, 0.25)
    median = _quantile_linear(vals, 0.50)
    q3 = _quantile_linear(vals, 0.75)
    return {
        "n": n,
        "mean": mean,
        "sd": sd,
        "min": vals[0],
        "q1": q1,
        "median": median,
        "q3": q3,
        "iqr": q3 - q1,
        "p95": _quantile_linear(vals, 0.95),
        "max": vals[-1],
    }


# ---------------------------------------------------------------------------
# cluster bootstrap
# ---------------------------------------------------------------------------
_STATISTICS: dict[str, Callable[[Sequence[float]], float]] = {
    "median": lambda xs: _quantile_linear(sorted(xs), 0.50),
    "mean": lambda xs: sum(xs) / len(xs),
}


def cluster_bootstrap_ci(
    values: Sequence[float],
    clusters: Sequence[object],
    stat: str | Callable[[Sequence[float]], float] = "median",
    B: int = 10000,
    level: float = 0.95,
    seed: int = 0,
) -> dict:
    """Cluster (block) bootstrap percentile interval for a pooled statistic.

    Estimator: the resampling unit is the *cluster*, not the observation.  There
    are ``G`` distinct clusters; each replicate draws ``G`` clusters with
    replacement and pools every observation belonging to each drawn cluster, so
    observations inside a cluster always move together.  The replicate
    statistic is computed over the pooled resampled values.  The interval is the
    **percentile** interval: the empirical ``alpha/2`` and ``1 - alpha/2``
    quantiles of the ``B`` replicate statistics, using the same linear
    interpolation as :func:`_quantile_linear`.

    The reported point estimate is the statistic over the *original* pooled
    sample (all observations, once), not the mean or median of the replicates.
    ``n_clusters`` is the number of distinct clusters, and every returned number
    is reproducible from ``(values, clusters, stat, B, level, seed)``.

    Percentile rather than BCa: BCa needs a jackknife-by-cluster acceleration
    term whose small-``G`` behaviour is unstable, and the percentile interval is
    the transparent definition the paper states.  No studentized correction is
    applied.  The interval inherits the usual bootstrap bias -- it is centred on
    the replicate distribution, which can be skewed relative to the point
    estimate for small ``G``.

    Assumptions: clusters are exchangeable (independent of one another); within
    a cluster the observations may be arbitrarily dependent.  ``G`` should be at
    least ~10 for the percentile endpoints to be meaningful -- with fewer
    clusters the interval is reported but should be read as indicative only.

    This is the cluster-bootstrap definition used in the paper.
    """
    vals = _as_floats(values)
    if isinstance(clusters, (str, bytes)):
        raise ValueError("clusters must be a sequence parallel to values, not a string")
    cluster_list = list(clusters)
    if len(cluster_list) != len(vals):
        raise ValueError(
            f"values and clusters must be parallel; got {len(vals)} values and {len(cluster_list)} clusters"
        )
    if isinstance(B, bool) or int(B) != B or int(B) <= 0:
        raise ValueError(f"B must be a positive integer, got {B!r}")
    b_int = int(B)
    lvl = _validate_level(level)

    if callable(stat):
        stat_fn = stat
        stat_name = getattr(stat, "__name__", "callable")
    else:
        key = str(stat).lower()
        if key not in _STATISTICS:
            raise ValueError(f"stat must be one of {sorted(_STATISTICS)} or a callable, got {stat!r}")
        stat_fn = _STATISTICS[key]
        stat_name = key

    # group observations by cluster, preserving first-appearance order so that
    # the RNG consumption order is a function of the input alone
    groups: dict[object, list[float]] = {}
    for value, cluster in zip(vals, cluster_list):
        try:
            groups.setdefault(cluster, []).append(value)
        except TypeError as exc:
            raise ValueError(f"cluster labels must be hashable, got {cluster!r}") from exc
    keys = list(groups.keys())
    g = len(keys)
    if g == 0:  # pragma: no cover - unreachable after _as_floats
        raise ValueError("no clusters found")

    point = float(stat_fn(vals))

    rng = random.Random(seed)
    replicates: list[float] = []
    for _ in range(b_int):
        pooled: list[float] = []
        for _draw in range(g):
            pooled.extend(groups[keys[rng.randrange(g)]])
        replicates.append(float(stat_fn(pooled)))
    replicates.sort()

    alpha = 1.0 - lvl
    lo = _quantile_linear(replicates, alpha / 2.0)
    hi = _quantile_linear(replicates, 1.0 - alpha / 2.0)
    return {
        "point": point,
        "lo": lo,
        "hi": hi,
        "B": b_int,
        "n_clusters": g,
        "n_observations": len(vals),
        "stat": stat_name,
        "level": lvl,
        "seed": seed,
        "method": "cluster bootstrap, percentile",
    }


# ---------------------------------------------------------------------------
# Student-t inference
# ---------------------------------------------------------------------------
def _t_cdf(t: float, df: float) -> float:
    """CDF of Student's t with ``df`` degrees of freedom.

    Uses ``P(T <= t) = 1/2 * I_{df/(df+t^2)}(df/2, 1/2)`` for ``t <= 0`` and its
    complement for ``t > 0``.
    """
    t_f = float(t)
    df_f = float(df)
    if not math.isfinite(df_f) or df_f <= 0.0:
        raise ValueError(f"df must be finite and positive, got {df!r}")
    if not math.isfinite(t_f):
        return 1.0 if t_f > 0 else 0.0
    x = df_f / (df_f + t_f * t_f)
    tail = 0.5 * regularized_incomplete_beta(df_f / 2.0, 0.5, x)
    return tail if t_f <= 0.0 else 1.0 - tail


def tost_paired(diffs: Iterable[float], bound: float, level: float = 0.95) -> dict:
    """Two one-sided t tests (TOST) for equivalence of paired differences.

    Input ``diffs`` are paired differences ``d_i = a_i - b_i``.  The null
    hypotheses are ``H0_lower: mu <= -bound`` and ``H0_upper: mu >= +bound``;
    equivalence at ``alpha = 1 - level`` is declared only when both are rejected.

    Estimators, exactly as used in the paper:

        mean_diff = mean(d),  se = sd(d) / sqrt(n),  df = n - 1
        t_lower = (mean_diff + bound) / se   ->  p_lower = 1 - T_cdf(t_lower, df)
        t_upper = (mean_diff - bound) / se   ->  p_upper = T_cdf(t_upper, df)
        p       = max(p_lower, p_upper)
        equivalent = p < alpha

    ``p`` is the TOST p-value (the conservative maximum of the two one-sided
    p-values).  No multiplicity adjustment is applied here; use
    :func:`holm_bonferroni` across a family of TOST tests.

    Assumptions: the ``d_i`` are i.i.d. normal with finite variance (the t
    reference distribution is exact under normality and approximate otherwise);
    ``bound`` is fixed before seeing the data.  If the paired differences have
    zero sample variance, ``t`` is infinite for any non-zero offset and the test
    degenerates: ``equivalent`` is then true only when ``|mean_diff| < bound``,
    with ``p = 0.0`` (inside) or ``p = 1.0`` (outside).
    """
    vals = _as_floats(diffs, "diffs")
    n = len(vals)
    if n < 2:
        raise ValueError(f"tost_paired needs at least 2 paired differences, got {n}")
    bound_f = float(bound)
    if not math.isfinite(bound_f) or bound_f <= 0.0:
        raise ValueError(f"bound must be finite and strictly positive, got {bound!r}")
    lvl = _validate_level(level)
    alpha = 1.0 - lvl

    mean_diff = sum(vals) / n
    var = sum((v - mean_diff) ** 2 for v in vals) / (n - 1)
    sd = math.sqrt(var)
    df = float(n - 1)
    se = sd / math.sqrt(n)

    if se == 0.0:
        inside = abs(mean_diff) < bound_f
        return {
            "mean_diff": mean_diff,
            "se": 0.0,
            "df": df,
            "t_lower": math.inf if mean_diff > -bound_f else -math.inf,
            "t_upper": math.inf if mean_diff > bound_f else -math.inf,
            "p_lower": 0.0 if inside else 1.0,
            "p_upper": 0.0 if inside else 1.0,
            "p": 0.0 if inside else 1.0,
            "bound": bound_f,
            "level": lvl,
            "n": n,
            "equivalent": bool(inside),
        }

    t_lower = (mean_diff + bound_f) / se
    t_upper = (mean_diff - bound_f) / se
    p_lower = 1.0 - _t_cdf(t_lower, df)
    p_upper = _t_cdf(t_upper, df)
    p = max(p_lower, p_upper)
    return {
        "mean_diff": mean_diff,
        "se": se,
        "df": df,
        "t_lower": t_lower,
        "t_upper": t_upper,
        "p_lower": p_lower,
        "p_upper": p_upper,
        "p": p,
        "bound": bound_f,
        "level": lvl,
        "n": n,
        "equivalent": bool(p < alpha),
    }


# ---------------------------------------------------------------------------
# multiplicity control
# ---------------------------------------------------------------------------
def holm_bonferroni(pvals: Iterable[float], alpha: float = 0.05) -> dict:
    """Holm-Bonferroni step-down adjustment.

    Estimator (Holm 1979): sort the ``m`` p-values ascending, multiply the
    i-th smallest (1-based) by ``m - i + 1``, then enforce monotonicity with a
    running maximum so the adjusted values are non-decreasing in the sorted
    order, and finally clamp to 1.  This is the standard step-down adjusted
    p-value; rejection of hypothesis ``i`` is equivalent to ``adjusted_i <=
    alpha``, and Holm's procedure controls the family-wise error rate under
    arbitrary dependence.

    Returns a dict with ``adjusted`` (adjusted p-values in the caller's input
    order), ``rejected`` (same order), ``alpha``, ``m``, and ``order`` (the
    permutation from input positions to ascending sorted positions).

    Assumptions: each input p-value is valid (uniform under its own null);
    ``m`` is the size of the pre-specified family.  Adding or removing a test
    changes every adjusted value, so the family must be fixed in advance.

    This is the multiplicity-correction definition used in the paper.
    """
    if isinstance(pvals, (str, bytes)):
        raise ValueError("pvals must be a sequence of numbers, not a string")
    raw = list(pvals)
    if not raw:
        raise ValueError("pvals is empty; at least one p-value is required")
    vals: list[float] = []
    for i, p in enumerate(raw):
        f = float(p)
        if not math.isfinite(f) or not (0.0 <= f <= 1.0):
            raise ValueError(f"pvals[{i}] must lie in [0, 1], got {p!r}")
        vals.append(f)
    alpha_f = float(alpha)
    if not math.isfinite(alpha_f) or not (0.0 < alpha_f < 1.0):
        raise ValueError(f"alpha must satisfy 0 < alpha < 1, got {alpha!r}")

    m = len(vals)
    order = sorted(range(m), key=lambda i: (vals[i], i))
    adjusted_sorted: list[float] = []
    running = 0.0
    for rank, idx in enumerate(order):
        candidate = (m - rank) * vals[idx]
        running = max(running, candidate)
        adjusted_sorted.append(min(1.0, running))

    adjusted = [0.0] * m
    rejected = [False] * m
    for rank, idx in enumerate(order):
        adjusted[idx] = adjusted_sorted[rank]
        rejected[idx] = adjusted_sorted[rank] <= alpha_f
    return {
        "adjusted": adjusted,
        "rejected": rejected,
        "alpha": alpha_f,
        "m": m,
        "order": order,
    }
