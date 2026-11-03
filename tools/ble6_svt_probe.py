"""Exploratory Hankel SVT gap completion; not a reproduction of Wang et al.

The paper's full method and parameters were not available. This probe keeps
observations unchanged and fills interior gaps only; it is not firmware code.
"""
import numpy as np


def complete(values, observed):
    values = np.asarray(values, dtype=complex)
    observed = np.asarray(observed, dtype=bool)
    if values.ndim != 1 or values.shape != observed.shape or observed.sum() < 4:
        raise ValueError("At least four observed tones are required")
    if not np.isfinite(values[observed]).all():
        raise ValueError("Non-finite observed tones")
    present = np.flatnonzero(observed)
    first, last = int(present[0]), int(present[-1])
    source = values[first:last + 1]
    mask = observed[first:last + 1]
    result = np.where(mask, source, 0j)
    rows = len(source) // 2
    cols = len(source) - rows + 1
    indices = np.arange(rows)[:, None] + np.arange(cols)[None, :]
    counts = np.bincount(indices.ravel(), minlength=len(source))
    for _ in range(20):
        u, s, vh = np.linalg.svd(result[indices], full_matrices=False)
        reconstructed = (u * np.maximum(s - .1*s[0], 0)) @ vh
        real = np.bincount(indices.ravel(), weights=reconstructed.real.ravel(), minlength=len(source))
        imag = np.bincount(indices.ravel(), weights=reconstructed.imag.ravel(), minlength=len(source))
        result = np.where(mask, source, (real + 1j*imag) / counts)
    output = np.zeros_like(values)
    output[first:last + 1] = result
    return output
