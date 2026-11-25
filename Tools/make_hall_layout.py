"""Bigger data hall layouts from the saved L_Test layout (2026-11-25).

L_Test is 2 pods (4 rows x 8 GB200 NVL72 = 32 racks). This writes a v2 layout file (vrg.Layout.Load) with more pods
of the same geometry, a chosen number of CDUs and in-row CRAHs, and optionally placed FWS dry coolers.

  1) in the game (any mode):   vrg.Layout.Save LTest_Base
  2) py -I Tools/make_hall_layout.py --pods 4 --cdus 4 --crah-per-row 2 --out Hall64_C4_R16
  3) in the game:              vrg.Layout.Load Hall64_C4_R16   (then vrg.Size.Rated 1 for product ratings)

Equipment of the level (placed racks, CDU, CRAHs, spines) is reused and moved; the rest is spawned as runtime
equipment. Placed CRAHs that a variant does not use go to the file's deletedCrahs list (a layout file never deletes
placed CDUs/CRAHs by omission). Gaps between units are the level's own 6 cm (1 cm rule).
"""
import argparse, copy, json, math, os, sys

APC_INROW_RC_FLOW_M3S = 6000.0 * 0.0004719474   # APC InRow RC 600 mm chilled water (ACRC60x), 6000 CFM; up to 70 kW


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default=os.path.join('Saved', 'Layouts', 'LTest_Base.json'))
    ap.add_argument('--pods', type=int, default=4, help='pods of 2 facing rows (L_Test = 2)')
    ap.add_argument('--cdus', type=int, default=4, help='CDU actors, spread over the rows')
    ap.add_argument('--crah-per-row', type=int, default=2, choices=[1, 2, 3])
    ap.add_argument('--crah-flow', type=float, default=APC_INROW_RC_FLOW_M3S, help='per unit airflow [m^3/s], 0 = share the design airflow')
    ap.add_argument('--fws-drycoolers', type=int, default=0, help='placed FWS dry coolers (0 = automatic selection rule)')
    ap.add_argument('--plant', type=int, default=-1, help='settings.plantEnabled: 1/0, -1 = keep the base value')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    base = load(a.base)
    eq = base['equipment']
    racks = [e for e in eq if 'rackId' in e]
    cdus = [e for e in eq if 'cduId' in e]
    crahs = [e for e in eq if 'crahId' in e]
    spines = [e for e in eq if 'spineId' in e]
    if not racks or not cdus:
        sys.exit('base layout has no racks or no CDU')

    row_x = sorted({round(r['x_m'], 3) for r in racks})
    if len(row_x) % 2:
        sys.exit('expected rows in facing pairs')
    pod_pitch = row_x[2] - row_x[0] if len(row_x) >= 4 else 6.13
    base_pods = len(row_x) // 2
    rows_per_pod_x = row_x  # x of the base rows
    rack_y = sorted({round(r['y_m'], 3) for r in racks})
    rack_pitch = rack_y[1] - rack_y[0]
    y_first, y_last = rack_y[0], rack_y[-1]

    out_eq, deleted_crahs = [], []

    # Racks: the base pods as they are, further pods as runtime copies shifted along x.
    for r in racks:
        out_eq.append(copy.deepcopy(r))
    for p in range(base_pods, a.pods):
        shift = (p // base_pods) * base_pods * pod_pitch
        src_pod_rows = row_x[(p % base_pods) * 2:(p % base_pods) * 2 + 2]
        for r in racks:
            if round(r['x_m'], 3) not in src_pod_rows:
                continue
            n = copy.deepcopy(r)
            n['x_m'] = r['x_m'] + shift
            row = len(row_x) * (p // base_pods) + row_x.index(round(r['x_m'], 3))
            k = rack_y.index(round(r['y_m'], 3))
            n['name'] = f'HallRack_{row}_{k}'
            n['rackId'] = f'hall/rack/{row}/{k}'
            out_eq.append(n)

    all_rows = []  # (x, yaw) of every row
    for p in range(a.pods):
        shift = (p // base_pods) * base_pods * pod_pitch
        for x in row_x[(p % base_pods) * 2:(p % base_pods) * 2 + 2]:
            yaw = next(r['yaw_deg'] for r in racks if round(r['x_m'], 3) == x)
            all_rows.append((x + shift, yaw))

    # Spines: the base ones, and a shifted copy for every further block of base pods.
    for s in spines:
        out_eq.append(copy.deepcopy(s))
    for b in range(1, math.ceil(a.pods / base_pods)):
        for i, s in enumerate(spines):
            n = copy.deepcopy(s)
            n['x_m'] = s['x_m'] + b * base_pods * pod_pitch
            n['name'] = f'HallSpine_{b}_{i}'
            n['spineId'] = f'hall/spine/{b}/{i}'
            n['runtimeSpine'] = True
            out_eq.append(n)
    spine_x = [s['x_m'] + b * base_pods * pod_pitch for s in spines for b in range(math.ceil(a.pods / base_pods))]

    # In-row CRAHs: per row, the first at the start end, the second at the far end (after a spine if the row has one),
    # the third one pitch before the first. The level's placed units are reused first.
    crah_template = copy.deepcopy(crahs[0]) if crahs else None
    placed = list(crahs)
    slots = []
    for i, (x, yaw) in enumerate(all_rows):
        far_has_spine = any(abs(sx - x) < 0.2 for sx in spine_x)
        far = y_last + rack_pitch * (2 if far_has_spine else 1)
        ends = [y_first - rack_pitch, far, y_first - 2 * rack_pitch][:a.crah_per_row]
        for j, y in enumerate(ends):
            slots.append((i, j, x, y, yaw))
    for (i, j, x, y, yaw) in slots:
        if placed:
            n = placed.pop(0)
            n = copy.deepcopy(n)
        else:
            n = copy.deepcopy(crah_template)
            n['name'] = f'HallCrah_{i}_{j}'
            n['crahId'] = f'hall/crah/{i}/{j}'
            n['runtimeCrah'] = True
            n['convertedAutomaticCrah'] = False
            for k in ('automaticCrahTransformCm', 'automaticCrahSizeM'):
                n.pop(k, None)
        n['x_m'], n['y_m'], n['yaw_deg'] = x, y, yaw
        n['crahSettings']['volumeFlowM3s'] = a.crah_flow
        out_eq.append(n)
    for n in placed:   # level units this variant does not use
        deleted_crahs.append(copy.deepcopy(n))

    # CDUs on a line in front of the row starts, at the centres of equal groups of rows (rows go to the nearest CDU).
    nrows = len(all_rows)
    cdu_y = min(y_first - (a.crah_per_row + 1) * rack_pitch, cdus[0]['y_m'])
    groups = [list(range(nrows))[g * nrows // a.cdus:(g + 1) * nrows // a.cdus] for g in range(a.cdus)]
    for g, rows in enumerate(groups):
        cx = sum(all_rows[r][0] for r in rows) / len(rows)
        if g == 0:
            n = copy.deepcopy(cdus[0])
        else:
            n = copy.deepcopy(cdus[0])
            n['name'] = f'HallCdu_{g}'
            n['cduId'] = f'hall/cdu/{g}'
            n['runtimeCdu'] = True
        n['x_m'], n['y_m'] = cx, cdu_y - 1.0
        out_eq.append(n)

    out = copy.deepcopy(base)
    out['equipment'] = out_eq
    out['deletedCrahs'] = deleted_crahs
    out['deletedCdus'] = []
    out['deletedSpines'] = []
    out['crahInventory'] = True
    out['cduInventory'] = True
    out['spineInventory'] = True
    if a.plant >= 0:
        out['settings']['plantEnabled'] = bool(a.plant)

    # Placed FWS dry coolers (others stay automatic): a grid behind the CDU line, well outside the room.
    plant = []
    if a.fws_drycoolers > 0:
        x0 = all_rows[0][0] - 4.0
        for i in range(a.fws_drycoolers):
            plant.append({'name': f'HallFwsDryCooler_{i}', 'plantId': f'hall/plant/fws_dry/{i}', 'kind': 0,
                          'x_m': x0 + (i % 12) * 3.0, 'y_m': cdu_y - 8.0 - (i // 12) * 2.0, 'z_m': 0.0,
                          'yaw_deg': 0.0, 'pitch_deg': 0.0, 'roll_deg': 0.0,
                          'size_x_m': 2.4, 'size_y_m': 1.2, 'size_z_m': 1.3})
    out['plantInventory'] = True
    out['plantEquipment'] = plant

    path = os.path.join(os.path.dirname(a.base), a.out + '.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(out, f, ensure_ascii=False)
    nr = sum(1 for e in out_eq if 'rackId' in e)
    print(f'{path}: racks {nr} in {nrows} rows, CDUs {a.cdus} (rows per CDU {[len(g) for g in groups]}), '
          f'CRAHs {len(slots)} (flow {a.crah_flow:.3f} m3/s each), deleted placed CRAHs {len(deleted_crahs)}, '
          f'FWS dry coolers placed {a.fws_drycoolers}')


if __name__ == '__main__':
    main()
