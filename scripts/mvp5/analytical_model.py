"""Independent fixed-tile accounting and restricted serial/paired timing oracle."""
from fractions import Fraction


def reference(c):
    a, d = c['array_n'], c['data_bytes']
    mt, kt, nt = ((c[k] + a - 1) // a for k in ('M', 'K', 'N'))
    passes, outputs, size = mt * kt * nt, mt * nt, a * a * d
    requests = outputs * (2 * kt + 1)
    resolution = Fraction(str(c['time_resolution_s']))
    def ticks(seconds):
        q = Fraction(seconds) / resolution
        return (2 * q.numerator + q.denominator) // (2 * q.denominator)
    cycle = Fraction(1, int(c['clock_hz']))
    buf = ticks(((size + int(c['buf_bw_Bpc']) - 1) // int(c['buf_bw_Bpc'])) * cycle)
    lat = ticks(c['noc_latency'] * cycle) + ticks(c['hbm_lat_cyc'] * cycle)
    mem = ticks(Fraction(size, 1) / Fraction(str(c['hbm_bw_GBps'])) / 10**9)
    compute = ticks(3 * a * cycle)
    result = dict(requests=requests, ic_requests=requests, bytes=requests * size,
                  passes=passes, macs=passes*a**3, executed_ops=2*passes*a**3,
                  useful_ops=2*c['M']*c['K']*c['N'])
    result['pe_ticks'] = passes * compute
    # Conditions ensure the IC issue interval does not delay the reference schedule.
    if c['dma'] == 1 and c['interconnect_bw_GBps'] >= c['hbm_bw_GBps']:
        if c['schedule'] == 'serial':
            result['sim_ticks'] = requests * (lat + mem + buf) + passes * compute
        elif c['schedule'] == 'paired' and c['dma_outstanding'] >= 2:
            load = lat + mem + max(buf, mem) + buf
            result['sim_ticks'] = passes * (load + compute) + outputs * (buf + lat + mem)
    return result
