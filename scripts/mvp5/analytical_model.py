"""Independent fixed-tile accounting and restricted serial/paired timing oracle."""
# Fraction 用分数精确表示时间，避免浮点误差影响 tick 的取整结果。
from fractions import Fraction


# 独立的解析参考模型：用公式计算预期统计值，供验证脚本对照仿真结果。
# 计算任务为 A[M, K] × B[K, N] = C[M, N]；c 是包含硬件和任务参数的字典。
# 所有块都按完整的 a×a 块计费，边缘不足一块也占用完整块的搬运和计算成本。
# 返回值 result 是统计字典；仅在下方支持的调度条件下包含总时间 sim_ticks。
def reference(c):
    # a：阵列边长，也是 tile（矩阵分块）的边长；d：每个元素的字节数。
    a, d = c['array_n'], c['data_bytes']
    # 分别计算 M、K、N 方向的块数；(x+a-1)//a 表示 x/a 向上取整。
    # 右侧按 M、K、N 顺序生成三个值，再分别赋给 mt、kt、nt。
    mt, kt, nt = ((c[k] + a - 1) // a for k in ('M', 'K', 'N'))
    # passes：块乘法次数，每个输出块沿 K 方向累加 kt 次。
    # outputs：输出块数；size：一个完整块的字节数。
    passes, outputs, size = mt * kt * nt, mt * nt, a * a * d
    # requests 统计搬运数据的请求次数，包括读取输入块和写回输出块。
    # 对于一个输出 C 块，需要沿 K 方向做 kt 次块乘法并累加：
    #   每次块乘法读取一个 A 块和一个 B 块，因此“2”表示两次读取请求。
    #   kt 次块乘法共需 2*kt 次读取；此模型每次都重新读取，不复用输入块。
    #   中间结果留在片上累加，全部 kt 次计算结束后才写回 C 块一次，这就是“+1”。
    # 所以一个输出块需要 2*kt+1 次请求，再乘输出块数 outputs，得到总请求数。
    # 例如 kt=3：读入 3 对 A/B 块共 6 次，写回 C 块 1 次，每个输出块共 7 次；
    # 若 outputs=8，则总请求数为 8*(2*3+1)=56 次。
    requests = outputs * (2 * kt + 1)

    # 一个 tick 是仿真的最小时间单位，例如 1e-12 秒表示 1 ps。
    # 先转字符串再构造 Fraction，按十进制数值表示时间分辨率。
    resolution = Fraction(str(c['time_resolution_s']))

    # 把秒转换为整数 tick；对非负时间四舍五入，恰好半个 tick 时向上取整。
    def ticks(seconds):
        # resolution 表示一个 tick 有多少秒。
        # tick 数 = 总时间（秒）/ 每个 tick 的时长（秒），结果可能不是整数。
        # 例如一个 tick 为 1 ns，总时间为 3.6 ns，则 q 为 3.6，即分数 18/5。
        # Fraction 用分数保存数值，后续可直接用整数运算完成取整。
        q = Fraction(seconds) / resolution
        # q.numerator 是分子 n，q.denominator 是分母 d；例如 18/5 的 n=18、d=5。
        # 下式计算 (2*n+d)//(2*d)，即对 n/d + 0.5 向下取整。
        # 对非负数，“先加 0.5，再向下取整”就是四舍五入：
        #   3.2 -> 3.7 -> 3 tick；3.5 -> 4.0 -> 4 tick；3.6 -> 4.1 -> 4 tick。
        # 例如 q=18/5 时：(2*18+5)//(2*5) = 41//10 = 4 tick。
        # 这里 // 表示向下取整的整数除法；全程无需把分数转为浮点数。
        return (2 * q.numerator + q.denominator) // (2 * q.denominator)

    # 时钟周期（秒）= 1 / 时钟频率（Hz）。
    cycle = Fraction(1, int(c['clock_hz']))
    # 片上缓冲区搬运一个块的时间（tick）：
    # buf_bw_Bpc 的单位是字节/周期，先把所需周期数向上取整，再换算时间。
    buf = ticks(((size + int(c['buf_bw_Bpc']) - 1) // int(c['buf_bw_Bpc'])) * cycle)
    # 一次访存的固定延迟（tick）= 片上网络 NoC 延迟 + 高带宽内存 HBM 延迟。
    # 两个配置延迟的单位都是周期；分别转换为 tick 后相加，以匹配仿真计时。
    lat = ticks(c['noc_latency'] * cycle) + ticks(c['hbm_lat_cyc'] * cycle)
    # HBM 传输一个块的时间（tick）= 字节数 / 带宽。
    # hbm_bw_GBps 使用十进制 GB/s，因此除以 10**9 后得到秒。
    mem = ticks(Fraction(size, 1) / Fraction(str(c['hbm_bw_GBps'])) / 10**9)
    # PE（处理单元）阵列完成一次块乘法的时间（tick）。
    # 本项目近似为填充 a 周期 + 稳态计算 a 周期 + 排空 a 周期，共 3*a 周期。
    compute = ticks(3 * a * cycle)

    # 汇总与调度方式无关的工作量：
    # requests / ic_requests：DMA 请求数 / 经过互连 IC 的请求数，两者在此相同。
    # bytes：所有输入读取和输出写回的总字节数。
    # macs：乘加次数；一次 a×a 块乘法执行 a³ 次乘加（MAC）。
    # executed_ops：按完整块执行的运算量，一次乘加算乘法、加法两个操作。
    # useful_ops：原始矩阵实际需要的运算量，不含边缘块补齐带来的额外计算。
    result = dict(requests=requests, ic_requests=requests, bytes=requests * size,
                  passes=passes, macs=passes*a**3, executed_ops=2*passes*a**3,
                  useful_ops=2*c['M']*c['K']*c['N'])
    # PE 计算时间总和；不包含搬运数据或等待数据的时间。
    result['pe_ticks'] = passes * compute

    # 以下总耗时公式只覆盖受限场景：仅一条 DMA，且互连带宽不低于 HBM 带宽。
    # 这些条件保证互连的请求发出间隔不会额外拖慢这里推导的调度。
    # 不满足条件时仍返回工作量统计，但不提供 sim_ticks。
    if c['dma'] == 1 and c['interconnect_bw_GBps'] >= c['hbm_bw_GBps']:
        if c['schedule'] == 'serial':
            # 串行：每次访存和计算都依次完成，时间直接相加。
            # 每次读取或写回都消耗固定延迟 lat、HBM 传输 mem、缓冲区访问 buf。
            result['sim_ticks'] = requests * (lat + mem + buf) + passes * compute
        elif c['schedule'] == 'paired' and c['dma_outstanding'] >= 2:
            # paired：成对提交两个输入读取请求，DMA 至少支持两个未完成请求。
            # 两个请求的固定延迟可以重叠，但 HBM 的两次数据传输仍依次进行。
            # 第一个输入就绪需 lat+mem；随后它写入缓冲区的 buf 时间与
            # 第二个输入的 HBM 传输 mem 时间重叠，所以取 max(buf, mem)。
            # 最后再花 buf 时间把第二个输入写入缓冲区。
            load = lat + mem + max(buf, mem) + buf
            # 每次块乘法先加载一对输入再计算；每个输出块最终单独写回一次。
            result['sim_ticks'] = passes * (load + compute) + outputs * (buf + lat + mem)
    # double（双缓冲）等其他调度不在上述总耗时公式的覆盖范围内。
    return result
