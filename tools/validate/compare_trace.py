#!/usr/bin/env python3
"""compare_trace.py — M5 validation comparison: the macOS port's exported
trace vs the published SDSS DR17 Cosmic Slime VAC, at the d8 tier.

Inputs:
  --export-dir : contains trace.bin (headerless raw f16, Z-major/X-fastest —
                 the axis order is implicit in the file, so it is pinned HERE)
                 and export_metadata.txt (grid dims).
  --reference  : mcpm_sdss_d8.npy — float32 (89, 150, 91) = (X, Y, Z),
                 block-averaged 8x from the published 712x1200x728 cube by
                 skymap's extractMcpmCube.py (downscale_local_mean).

Reports (NO pass/fail verdict — measure-first policy, M5 design §1: the
human sets the acceptance bar after the first honest measurement):
  - 3D voxelwise Pearson on log10(x+eps): masked (joint support) + unmasked
  - per-axis max-projection Pearson (project linear first, then log):
    masked + unmasked  -> eight numbers total
  - eps sensitivity (1e-4 / 1e-2) as one-line summaries
  - projections.png: ours vs reference max-projections, magma, shared scale

Conventions mirror rhizome's compareCubes.py (per-axis max-projection
Pearson as first-class metrics; magma log-scaled render; np.corrcoef equals
its manual Pearson formula). Deliberate, documented deviations (design §8):
log10(x+eps) instead of linear min-max; block-average instead of trilinear
zoom (same operator that produced the reference tiers); masked + unmasked
variants (~70% of d8 reference voxels are exact zeros).

Offline only: numpy, scikit-image, matplotlib. No network.
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from skimage.transform import downscale_local_mean

D8_SHAPE = (89, 150, 91)   # (X, Y, Z) — extractMcpmCube.py convention
# eps rationale (design §8.3): d8 nonzero minimum ~= 3.9e-4, so 1e-3 sits at
# the bottom of the signal range — no -inf, negligible compression of real
# structure, round number. New explicit choice (compareCubes.py is linear
# min-max; no local log-eps precedent). Sensitivity reported at 1e-4 / 1e-2.
DEFAULT_EPS = 1e-3
SENSITIVITY_EPS = (1e-4, 1e-2)

# Orientation pin (design §8.6): at d8 the dims (89, 150, 91) are pairwise
# distinct, so the axis TRANSPOSITION is pinned by shape alone; only the 8
# reflection combinations need the one-time --orientation-scan on the first
# real export. (False, False, False) = identity. If a flip ever wins the
# scan, that is a BUG in load_export's layout assumption — fix the transpose
# there; never ship a compensating flip here. The recorded scan table lives
# in docs/superpowers/research/m5/m5-run-log.md.
PINNED_FLIPS = (False, False, False)


def parse_dims(metadata_path):
    """(X, Y, Z) ints from export_metadata.txt's grid-resolution line."""
    text = Path(metadata_path).read_text()
    m = re.search(r'simulation grid resolution: (\d+) x (\d+) x (\d+)', text)
    if not m:
        sys.exit(f'FATAL: no grid resolution line in {metadata_path}')
    return tuple(int(g) for g in m.groups())


def load_export(export_dir):
    """trace.bin -> float32 cube indexed (X, Y, Z).

    The .bin is headerless raw f16 with index = z*W*H + y*W + x (Z-major/
    X-fastest — DirectXTex layout; export-path research §1/§6). Reshape
    (Z, Y, X) C-order, then transpose to (X, Y, Z) to match the reference's
    convention. PINNED_FLIPS is applied last (identity unless the one-time
    scan ever proves otherwise — see the constant's comment).
    """
    export_dir = Path(export_dir)
    x, y, z = parse_dims(export_dir / 'export_metadata.txt')
    raw = np.fromfile(export_dir / 'trace.bin', dtype=np.float16)
    if raw.size != x * y * z:
        sys.exit(f'FATAL: trace.bin has {raw.size} voxels, metadata says {x*y*z}')
    cube = raw.reshape(z, y, x).astype(np.float32).transpose(2, 1, 0)
    for axis, flip in enumerate(PINNED_FLIPS):
        if flip:
            cube = np.flip(cube, axis)
    return np.ascontiguousarray(cube)


def block_average_to_d8(cube):
    """Mean-preserving block average to D8_SHAPE using the SAME operator
    (skimage downscale_local_mean) that produced the reference tiers.
    Hard-fails unless the factors are exact integers (8 at native, 4 at the
    half-res fallback). float32 accumulation: f16 quantization (~1e-3
    relative) already dominates any f32 accumulation error over <=512-voxel
    blocks, so float64 buys nothing at 8x the memory (design §8.2)."""
    factors = []
    for s, t in zip(cube.shape, D8_SHAPE):
        if s % t != 0:
            sys.exit(f'FATAL: non-integer downsample factor {cube.shape} -> {D8_SHAPE}')
        factors.append(s // t)
    return downscale_local_mean(cube, tuple(factors)).astype(np.float32)


def pearson(a, b):
    """Pearson on flattened arrays; np.corrcoef == compareCubes.py's manual
    dot-product formula."""
    return float(np.corrcoef(np.ravel(a).astype(np.float64),
                             np.ravel(b).astype(np.float64))[0, 1])


def metrics(ours_lin, ref_lin, eps):
    """The eight numbers (design §8.4/§8.5), from LINEAR d8 cubes.

    Masking: primary = joint support (nonzero in BOTH cubes pre-log,
    "exclude-zeros-in-either") — including the ~70% co-located zeros adds a
    constant-value mass at log10(eps) that inflates Pearson by rewarding
    agreement on empty background. Unmasked variants are also reported (the
    PolyPhy precedent numbers are unmasked; comparability matters).
    Projections: max-project the LINEAR cube first, then log10(+eps), then
    Pearson per axis (compareCubes.py's structure-over-intensity rationale).
    """
    out = {}
    lo = np.log10(ours_lin + eps)
    lr = np.log10(ref_lin + eps)
    joint = (ours_lin > 0) & (ref_lin > 0)
    out['pearson3d_masked'] = pearson(lo[joint], lr[joint])
    out['pearson3d_unmasked'] = pearson(lo, lr)
    out['joint_support_voxels'] = int(joint.sum())
    out['total_voxels'] = int(joint.size)
    for ax, name in enumerate('xyz'):
        po = ours_lin.max(axis=ax)
        pr = ref_lin.max(axis=ax)
        plo = np.log10(po + eps)
        plr = np.log10(pr + eps)
        pj = (po > 0) & (pr > 0)
        out[f'pearson_proj_{name}_masked'] = pearson(plo[pj], plr[pj])
        out[f'pearson_proj_{name}_unmasked'] = pearson(plo, plr)
    return out


def orientation_scan(ours_d8, ref, eps):
    """Unmasked axis-projection Pearsons under all 8 flip combinations
    (design §8.6). Identity must win decisively on the first real export;
    then the finding is pinned in PINNED_FLIPS and routine runs skip this.
    A winning flip = layout bug in load_export — fix the transpose there."""
    print('orientation scan (8 flip combos, sorted by mean; identity must win):')
    print('  flips (x, y, z)             proj-x    proj-y    proj-z     mean')
    results = []
    for fx in (False, True):
        for fy in (False, True):
            for fz in (False, True):
                cube = ours_d8
                for axis, f in enumerate((fx, fy, fz)):
                    if f:
                        cube = np.flip(cube, axis)
                rs = [pearson(np.log10(cube.max(axis=ax) + eps),
                              np.log10(ref.max(axis=ax) + eps))
                      for ax in range(3)]
                results.append(((fx, fy, fz), rs, float(np.mean(rs))))
    for flips, rs, mean in sorted(results, key=lambda t: -t[2]):
        tag = '   <-- identity' if flips == (False, False, False) else ''
        print(f'  {str(flips):26s} {rs[0]:+.4f}   {rs[1]:+.4f}   {rs[2]:+.4f}   '
              f'{mean:+.4f}{tag}')
    return results


def render_projections(ours_d8, ref, eps, out_png):
    """Side-by-side max-projection PNG per axis: ours vs reference,
    log10(+eps), magma, shared color scale per axis row (compareCubes.py
    precedent: imshow(img.T, origin='lower', cmap='magma'))."""
    fig, axes = plt.subplots(3, 2, figsize=(9, 13))
    for ax_i, name in enumerate('xyz'):
        po = np.log10(ours_d8.max(axis=ax_i) + eps)
        pr = np.log10(ref.max(axis=ax_i) + eps)
        vmin = float(min(po.min(), pr.min()))
        vmax = float(max(po.max(), pr.max()))
        for col, (img, label) in enumerate(((po, 'ours'), (pr, 'reference'))):
            a = axes[ax_i][col]
            a.imshow(img.T, origin='lower', cmap='magma', vmin=vmin, vmax=vmax)
            a.set_title(f'{label} — {name} max-projection (log10+{eps:g})')
            a.axis('off')
    fig.tight_layout()
    fig.savefig(out_png, dpi=120, bbox_inches='tight')
    plt.close(fig)


def run_compare(export_dir, reference, out_dir, eps, do_scan):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ref = np.load(reference).astype(np.float32)
    if ref.shape != D8_SHAPE:
        sys.exit(f'FATAL: reference shape {ref.shape} != {D8_SHAPE}')
    ours_native = load_export(export_dir)
    native_dims = tuple(int(s) for s in ours_native.shape)
    ours_d8 = block_average_to_d8(ours_native)
    del ours_native   # free ~2.5 GB before metrics/render

    if do_scan:
        orientation_scan(ours_d8, ref, eps)

    report = {
        'export_dir': str(export_dir),
        'reference': str(reference),
        'native_dims_xyz': list(native_dims),
        'downsample_factors': [s // t for s, t in zip(native_dims, D8_SHAPE)],
        'pinned_flips': list(PINNED_FLIPS),
        'eps_primary': eps,
        'metrics_by_eps': {},
    }
    for e in sorted(set((eps,) + SENSITIVITY_EPS)):
        report['metrics_by_eps'][f'{e:g}'] = metrics(ours_d8, ref, e)

    render_projections(ours_d8, ref, eps, out_dir / 'projections.png')

    m = report['metrics_by_eps'][f'{eps:g}']
    lines = [
        f'M5 d8 comparison — ours (native {native_dims}, block-averaged '
        f'x{report["downsample_factors"]}) vs {Path(reference).name}',
        f'primary eps = {eps:g}   joint support = {m["joint_support_voxels"]}'
        f'/{m["total_voxels"]} voxels',
        '',
        f'{"metric":34s} {"masked":>10s} {"unmasked":>10s}',
        f'{"pearson 3D voxelwise":34s} {m["pearson3d_masked"]:>+10.4f} '
        f'{m["pearson3d_unmasked"]:>+10.4f}',
    ]
    for name in 'xyz':
        lines.append(f'{"pearson " + name + " max-projection":34s} '
                     f'{m["pearson_proj_" + name + "_masked"]:>+10.4f} '
                     f'{m["pearson_proj_" + name + "_unmasked"]:>+10.4f}')
    lines.append('')
    for e in SENSITIVITY_EPS:
        s = report['metrics_by_eps'][f'{e:g}']
        lines.append(f'eps {e:g}: 3D {s["pearson3d_masked"]:+.4f} masked / '
                     f'{s["pearson3d_unmasked"]:+.4f} unmasked (sensitivity)')
    lines.append('')
    lines.append('NOTE: no pass/fail verdict is printed — measure-first policy '
                 '(M5 design §1); the human sets the acceptance bar.')
    text = '\n'.join(lines) + '\n'
    (out_dir / 'report.txt').write_text(text)
    (out_dir / 'report.json').write_text(json.dumps(report, indent=2))
    print(text)
    print(f'wrote {out_dir}/report.json, report.txt, projections.png')


def self_test(reference=None):
    """Synthetic-cube verification of every pipeline stage. Seeded RNG:
    deterministic. Passes silently assert; prints a summary on success."""
    rng = np.random.default_rng(20260813)
    # (a) pearson: exact known — b = (a+n)/sqrt(2) has corr 1/sqrt(2).
    a = rng.standard_normal(D8_SHAPE)
    n = rng.standard_normal(D8_SHAPE)
    b = (a + n) / np.sqrt(2.0)
    r = pearson(a, b)
    assert abs(r - 1.0 / np.sqrt(2.0)) < 0.005, r
    # (b) metrics pipeline recovers the same correlation through log10(+eps)
    #     (10**a is always > 0, so the joint mask covers everything and the
    #     eps floor only nibbles the far-negative tail).
    m = metrics(np.power(10.0, a).astype(np.float32),
                np.power(10.0, b).astype(np.float32), DEFAULT_EPS)
    assert abs(m['pearson3d_masked'] - 1.0 / np.sqrt(2.0)) < 0.02, m
    assert m['joint_support_voxels'] == m['total_voxels']
    # (c) block averaging: a repeat-upsampled cube must come back exactly
    #     (also exercises the half-res factor path, 2 here vs 4 in prod).
    d8 = rng.random(D8_SHAPE).astype(np.float32)
    up = d8.repeat(2, axis=0).repeat(2, axis=1).repeat(2, axis=2)
    assert np.allclose(block_average_to_d8(up), d8, atol=1e-6)
    # (d) load_export layout pin: synthetic Z-major/X-fastest f16 export.
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        X, Y, Z = 3, 4, 5
        xs, ys, zs = np.meshgrid(np.arange(X), np.arange(Y), np.arange(Z),
                                 indexing='ij')
        pattern = (xs + 10 * ys + 100 * zs).astype(np.float32)  # exact in f16
        pattern.transpose(2, 1, 0).astype(np.float16).tofile(td / 'trace.bin')
        (td / 'export_metadata.txt').write_text(
            f'simulation grid resolution: {X} x {Y} x {Z} [vox]\n')
        cube = load_export(td)
        assert cube.shape == (X, Y, Z), cube.shape
        assert cube[1, 2, 3] == 1 + 20 + 300
        assert cube[2, 0, 4] == 2 + 0 + 400
    # (e) against the real reference, when provided (design §11 item 7 smoke):
    #     identity -> 1.0; shuffled -> ~0. Exercises masking on the real
    #     ~70%-zero field.
    if reference is not None:
        ref = np.load(reference).astype(np.float32)
        assert ref.shape == D8_SHAPE, ref.shape
        m_self = metrics(ref, ref, DEFAULT_EPS)
        assert m_self['pearson3d_masked'] > 0.999999, m_self
        assert m_self['pearson3d_unmasked'] > 0.999999, m_self
        shuffled = rng.permutation(ref.ravel()).reshape(ref.shape)
        m_shuf = metrics(ref, shuffled, DEFAULT_EPS)
        assert abs(m_shuf['pearson3d_unmasked']) < 0.01, m_shuf
        print('self-test vs reference: identity == 1.0, shuffled ~ 0  OK')
    print('compare_trace.py self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--export-dir')
    ap.add_argument('--reference')
    ap.add_argument('--out')
    ap.add_argument('--eps', type=float, default=DEFAULT_EPS)
    ap.add_argument('--orientation-scan', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test(args.reference)
        return 0
    if not (args.export_dir and args.reference and args.out):
        ap.error('--export-dir, --reference and --out are required unless --self-test')
    run_compare(args.export_dir, args.reference, args.out, args.eps,
                args.orientation_scan)
    return 0


if __name__ == '__main__':
    sys.exit(main())
