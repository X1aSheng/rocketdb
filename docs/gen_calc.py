#!/usr/bin/env python3
"""Generate flash_lifespan_calc.csv — Flash lifespan calculator for Excel.

Row layout:
  1: Header
  2: Input section header
  3-15: Input parameters (PE_max .. GC_amp)
  16: blank
  17: KVDB WAF section header
  18-27: KVDB formulas (KV_w_small .. ratio_kv)
  28: blank
  29: TSDB WAF section header
  30-38: TSDB formulas (TS_w_small .. ratio_ts)
  39: blank
  40: Lifespan section header
  41-48: Lifespan formulas (T_kv_day .. T_ts_gain)
  49: blank
  50: Test data section header
  51-55: Measured test data
  56: blank
  57: Scenario section header
  58-61: Scenario projections
  62: blank
  63: Sensitivity section header
  64-68: Sensitivity analysis
"""

import csv

ROWS = []
def R(section='', param='', symbol='', value='', unit='', formula='', note=''):
    ROWS.append([section, param, symbol, str(value), unit, formula, note, '', ''])

# ── Header ──
R('Section', 'Parameter', 'Symbol', 'Value', 'Unit', 'Formula_doc', 'Note')

# ── Input Parameters (Excel rows 3-15) ──
R('', '=== INPUT PARAMETERS (edit values in column D) ===')
R('Flash',  'Max P/E cycles',          'PE_max',    100000, 'cycles',    '', 'W25QXX typical 100k')
R('Flash',  'Sector count',            'N_sec',     16,     'sectors',   '', 'Per partition config')
R('Flash',  'Sector size',             'S_sec',     4096,   'bytes',     '', 'Typical 4096')
R('Config',  'Stack buffer size',       'STACK_BUF', 64,     'bytes',     '', 'Configurable 64 128 256')
R('Config',  'TSDB record header',      'TS_REC_SZ', 20,     'bytes',     '', 'Fixed')
R('Config',  'KVDB record header',      'KV_REC_SZ', 16,     'bytes',     '', 'Fixed')
R('Load',   'Daily logical writes',    'W_day',     10000,  'writes/day','', 'Application estimate')
R('Load',   'KVDB small-record ratio', 'P_small_kv', 0.99,  '(0~1)',     '', 'Ratio of records <= STACK_BUF')
R('Load',   'TSDB small-record ratio', 'P_small_ts', 0.29,  '(0~1)',     '', 'Ratio of data <= STACK_BUF-TS_REC_SZ')
R('Load',   'KVDB avg value size',     'V_avg_kv',   11,     'bytes',     '', 'Measured or estimated')
R('Load',   'TSDB avg data size',      'D_avg_ts',   56,     'bytes',     '', 'Measured or estimated')
R('Load',   'KVDB avg key length',     'K_avg',      6,      'bytes',     '', 'Measured or estimated')
R('Load',   'GC amplification factor', 'GC_amp',     0.15,   '(0~1)',     '', 'Typical 0.05 to 0.30')

# ── KVDB WAF Calculator (Excel rows 17-27) ──
# Reference map:
#   D3=PE_max  D4=N_sec  D5=S_sec  D6=STACK_BUF  D7=TS_REC_SZ  D8=KV_REC_SZ
#   D9=W_day   D10=P_small_kv  D11=P_small_ts
#   D12=V_avg_kv  D13=D_avg_ts  D14=K_avg  D15=GC_amp
#   D18=KV_w_small  D19=header  D20=key  D21=chunks  D22=commit
#   D23=KV_w_large  D24=WAF_base  D25=WAF_kv  D26=WAF_kv_unopt  D27=ratio_kv
R()
R('', '=== KVDB WAF CALCULATOR ===')
R('KVDB', 'Small-record writes (merge+commit)', 'KV_w_small', '=1+1',              'writes/rec', '', '1 merge + 1 commit')
R('KVDB', 'Large: header write',                '',           1,                   'writes/rec', '', '')
R('KVDB', 'Large: key write',                   '',           1,                   'writes/rec', '', '')
R('KVDB', 'Large: value chunks',                'chunks',     1,                   'chunks',     '', 'CEIL(V_avg_kv/STACK_BUF)')
R('KVDB', 'Large: commit write',                '',           1,                   'writes/rec', '', '')
R('KVDB', 'Large total (sum of 4 rows above)',  'KV_w_large', '=D19+D20+D21+D22', 'writes/rec', '', '=header+key+chunks+commit')
R('KVDB', 'Base WAF (excl. GC)',                'WAF_base',   '=D10*D18+(1-D10)*D23', 'x',     '', 'P_small*2 + P_large*large_total')
R('KVDB', 'Final WAF (incl. GC)',               'WAF_kv',     '=D24*(1+D15)',      'x',          '', 'WAF_base * (1+GC_amp)')
R('KVDB', 'WAF without merge optimization',     'WAF_kv_unopt','=D10*4+(1-D10)*D23','x',         '', 'Small records: 4 writes instead of 2')
R('KVDB', 'Merge improvement ratio',            'ratio_kv',   '=D26/D25',           'x',          '', '>1 means merge optimization helps')

# ── TSDB WAF Calculator (Excel rows 29-38) ──
# Reference map:
#   D30=TS_w_small  D31=header  D32=chunks  D33=commit
#   D34=TS_w_large  D35=TS_WAF_base  D36=WAF_ts  D37=WAF_ts_unopt  D38=ratio_ts
R()
R('', '=== TSDB WAF CALCULATOR ===')
R('TSDB', 'Small-record writes (merge+commit)', 'TS_w_small', '=1+1',              'writes/rec', '', '1 merge + 1 commit')
R('TSDB', 'Large: header write',                '',           1,                   'writes/rec', '', '')
R('TSDB', 'Large: data chunks',                 'chunks',     2,                   'chunks',     '', 'CEIL(D_avg_ts/(STACK_BUF-TS_REC_SZ))')
R('TSDB', 'Large: commit write',                '',           1,                   'writes/rec', '', '')
R('TSDB', 'Large total (sum of 3 rows above)',  'TS_w_large', '=D31+D32+D33',     'writes/rec', '', '=header+chunks+commit')
R('TSDB', 'Base WAF',                           'TS_WAF_base','=D11*D30+(1-D11)*D34', 'x',     '', '')
R('TSDB', 'Final WAF',                          'WAF_ts',     '=D35',              'x',          '', 'TSDB has no GC migration')
R('TSDB', 'WAF without merge optimization',     'WAF_ts_unopt','=D11*3+(1-D11)*D34','x',         '', 'Small records: 3 writes instead of 2')
R('TSDB', 'Merge improvement ratio',            'ratio_ts',   '=D37/D36',           'x',          '', '>1 means merge optimization helps')

# ── Lifespan Results (Excel rows 40-48) ──
#   D41=T_kv_day  D42=T_kv_year  D43=T_kv_unopt  D44=T_kv_gain
#   D45=T_ts_day  D46=T_ts_year  D47=T_ts_unopt  D48=T_ts_gain
#   Uses WAF_kv(D25) for KVDB, WAF_ts(D36) for TSDB
R()
R('', '=== LIFESPAN RESULTS (watch these rows) ===')
R('Life', 'KVDB lifespan (days)',              'T_kv_day',   '=D3*D4*D5/(D9*D25*(D8+D12+D14))',          'days',  '', 'PE*Nsec*Ssec/(Wday*WAF*(HDR+KEY+VAL))')
R('Life', '>> KVDB LIFESPAN (years)',           'T_kv_year',  '=D41/365',                                 'years', '', 'FINAL KVDB RESULT')
R('Life', 'KVDB lifespan w/o merge (years)',    'T_kv_unopt', '=D3*D4*D5/(D9*D26*(D8+D12+D14))/365',      'years', '', '')
R('Life', '>> KVDB MERGE GAIN (years)',         'T_kv_gain',  '=D42-D43',                                 'years', '', 'How many years merge optimization adds')
R('Life', 'TSDB lifespan (days)',              'T_ts_day',   '=D3*D4*D5/(D9*D36*(D7+D13))',              'days',  '', 'PE*Nsec*Ssec/(Wday*WAF*(HDR+DATA))')
R('Life', '>> TSDB LIFESPAN (years)',           'T_ts_year',  '=D45/365',                                 'years', '', 'FINAL TSDB RESULT')
R('Life', 'TSDB lifespan w/o merge (years)',    'T_ts_unopt', '=D3*D4*D5/(D9*D37*(D7+D13))/365',          'years', '', '')
R('Life', '>> TSDB MERGE GAIN (years)',         'T_ts_gain',  '=D46-D47',                                 'years', '', 'How many years merge optimization adds')

# ── Measured Test Data (Excel rows 50-55) ──
R()
R('', '=== MEASURED TEST DATA (from tests/out/*.log) ===')
R('Test', 'kvdb_basic',  2648,  8126,  3.07, 'WAF', '99% merge hit avg_val=11B +30% life')
R('Test', 'kvdb_stress', 4184,  23267, 5.56, 'WAF', '65% merge hit avg_val=131B +22% life')
R('Test', 'tsdb_basic',  2402,  6780,  2.82, 'WAF', '29% merge hit avg_data=56B +17% life')
R('Test', 'tsdb_stress', 2719,  18256, 6.71, 'WAF', '21% merge hit avg_data=260B +16% life')
R('Test', 'integration', 18731, 87150, 4.65, 'WAF', 'KV89% TS21% merge hit +20% life')

# ── Scenario Projections (Excel rows 57-61) ──
R()
R('', '=== SCENARIO PROJECTIONS (pre-computed) ===')
R('Scene', 'Sensor (best case)',       10000, 3.07, 38,  'W_day=10000 WAF=3.07 R_avg=38B',   '99% merge small data high freq')
R('Scene', 'Mixed IoT (typical)',      10000, 5.56, 147, 'W_day=10000 WAF=5.56 R_avg=147B', '65% merge realistic mixed load')
R('Scene', 'Logging (large data)',     1000,  6.71, 280, 'W_day=1000 WAF=6.71 R_avg=280B',  '21% merge large data low freq')
R('Scene', 'Config store (rare writes)', 100, 3.07, 38,  'W_day=100 WAF=3.07 R_avg=38B',    '99% merge very low freq')

# ── Sensitivity Analysis (Excel rows 63-68) ──
R()
R('', '=== SENSITIVITY ANALYSIS ===')
R('Sens', 'STACK_BUF 64->128',      64,   128,  'merge-hit-rate up', 'lifespan up',     'Double buffer = more records merged')
R('Sens', 'P_small_kv 0.99->0.50',  0.99, 0.50, 'WAF up',            'lifespan -40%',   'Each -10% hit rate = -5% lifespan')
R('Sens', 'W_day x2',               10000,20000,'WAF unchanged',      'lifespan /2',     'Write freq inversely proportional')
R('Sens', 'GC_amp 0.15->0.30',      0.15, 0.30, 'WAF +15%',          'lifespan -13%',   'Each +0.1 GC amp = -10% lifespan')
R('Sens', 'PE_max 100K->50K',       100000,50000,'WAF unchanged',     'lifespan /2',     'PE cycles directly proportional')

# ── Write CSV ──
with open('flash_lifespan_calc.csv', 'w', newline='', encoding='utf-8-sig') as f:
    w = csv.writer(f, lineterminator='\n')
    for row in ROWS:
        w.writerow(row)

# ── Verify ──
print(f'Wrote {len(ROWS)} rows (1 header + {len(ROWS)-1} data)')

# Check all rows have 9 fields
bad = [(i, len(r)) for i, r in enumerate(ROWS, 1) if len(r) != 9]
if bad:
    for i, n in bad:
        print(f'  BAD row {i}: {n} fields (expected 9)')
else:
    print('All rows have 9 fields - OK')

# Print formula cells for manual review
print('\nFormula cells (column D):')
for i, r in enumerate(ROWS):
    v = str(r[3])
    if v.startswith('='):
        sym = r[2] if r[2] else '(none)'
        print(f'  Row {i+1:2d} [{sym:14s}]: {v}')

# Verify no self-references
print('\nSelf-reference check:')
errors = 0
for i, r in enumerate(ROWS):
    row_num = i + 1  # Excel row (1-indexed, row 1 = header)
    v = str(r[3])
    if v.startswith('='):
        # Extract all D<num> references
        import re
        refs = re.findall(r'D(\d+)', v)
        for ref in refs:
            if int(ref) == row_num:
                print(f'  SELF-REF ERROR: Row {row_num} [{r[2]}] references D{ref} (itself)')
                errors += 1
if errors == 0:
    print('  No self-references found - OK')
