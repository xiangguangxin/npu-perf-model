"""Validate exact counts, restricted timing, bounds, and experimental controls."""
# argparse：解析命令行参数；csv/json：读写结果文件；Path：拼接和访问文件路径。
import argparse
import csv
import json
from pathlib import Path
# reference 根据每条结果中的配置，独立计算预期工作量及部分调度的预期耗时。
from analytical_model import reference


# rows：从 results.json 读出的结果列表，每个元素是一个用例的结果字典。
# destination：验证明细 CSV 的输出路径；全部通过时返回检查条数，否则抛出异常。
# 输入应包含下方按 case_id 查找的固定用例；缺少这些用例时 next(...) 会报错。
def validate(rows, destination):
    # 收集所有检查明细，包括实际值、预期值和是否通过。
    checks = []

    # 内部辅助函数：登记一条检查，不在这里立即报错，便于最终统一输出报告。
    # case 是用例/检查类别，metric 是指标名，ok 是比较表达式的布尔结果。
    def check(case, metric, actual, expected, ok):
        checks.append(dict(case=case, metric=metric, actual=actual, expected=expected, passed=bool(ok)))

    # 第一部分：逐条验证仿真结果，r 表示当前用例的结果字典。
    for r in rows:
        # schema_version 是结果格式版本；此脚本期望版本 1。
        check(r['case_id'], 'schema', r['schema_version'], 1, r['schema_version'] == 1)
        # 仿真总耗时必须大于零。
        check(r['case_id'], 'positive runtime', r['sim_ticks'], '>0', r['sim_ticks'] > 0)
        # 用同一份配置计算参考值；ref.items() 依次给出“指标名、预期值”。
        ref = reference(r)
        for k, expected in ref.items():
            if k == 'pe_ticks':
                # pe_ticks 仅包含阵列计算时间，是总耗时下界，所以检查 >= 而非相等。
                check(r['case_id'], 'PE lower bound', r['sim_ticks'], expected, r['sim_ticks'] >= expected)
            else:
                # 请求数、字节数、运算量等应精确相等。
                # reference 只在支持的配置下返回 sim_ticks；若返回，也检查精确相等。
                check(r['case_id'], k, r[k], expected, r[k] == expected)
        # HBM 带宽下界（秒）= 总搬运字节数 / HBM 每秒可传输字节数。
        # 即使其他延迟都被隐藏，传输这些数据仍需要相应时间。
        bound = r['bytes'] / (r['hbm_bw_GBps'] * 1e9)
        # 比较时允许一个 tick 的误差，容纳仿真时间离散化产生的取整偏差。
        check(r['case_id'], 'HBM lower bound', r['sim_s'], bound,
              r['sim_s'] + r['time_resolution_s'] >= bound)
        # grants 记录各请求源获得的仲裁授权次数，总和应等于请求总数。
        check(r['case_id'], 'grants conserved', sum(r['grants']), r['requests'], sum(r['grants']) == r['requests'])

    # 第二部分：检查两个固定用例的手算锚点，即事先手工推导出的时间。
    # next(...) 找到第一个 case_id 匹配的结果；这里的固定期望按 1 ps/tick 计，
    # 因而 363 ns = 363000 tick，262 ns = 262000 tick。
    base = next(r for r in rows if r['case_id'] == 'v_single_serial')
    check('hand_anchor', '363 ns', base['sim_ticks'], 363000, base['sim_ticks'] == 363000)
    paired = next(r for r in rows if r['case_id'] == 'v_single_paired')
    check('hand_anchor', 'paired 262 ns', paired['sim_ticks'], 262000, paired['sim_ticks'] == 262000)

    # 第三部分：实验对照检查。E1 改调度，E2 改互连带宽，E5 改缓冲区容量。
    # 对同样的矩阵尺寸和阵列尺寸，这些变化不应改变搬运量、运算量和请求数。
    for group in ('E1', 'E2', 'E5'):
        # 列表推导式：筛选当前实验组的所有结果。
        selected = [r for r in rows if r['group'] == group]
        # 按 (M, K, N, array_n) 分组，每个 bucket 保存可相互对照的用例。
        buckets = {}
        for r in selected:
            key = (r['M'],r['K'],r['N'],r['array_n'])
            # key 尚不存在时先创建空列表，再将当前结果加入该列表。
            buckets.setdefault(key, []).append(r)
        for bucket in buckets.values():
            for r in bucket:
                # 基础指标有三项；E5 额外比较 sim_ticks，验证该实验中的容量变化
                # 不改变耗时。这里 + 拼接元组，条件不满足时拼接空元组 ()。
                for k in ('bytes', 'executed_ops', 'requests') + (('sim_ticks',) if group == 'E5' else ()):
                    # 以同一 bucket 的第一条结果为基准，逐项比较。
                    check(r['case_id'], group+' invariant '+k,r[k],bucket[0][k],r[k]==bucket[0][k])

    # 第四部分：确定性检查。同一配置重复运行，以下统计应完全一致。
    first = next(r for r in rows if r['case_id'] == 'v_multi_double')
    repeat = next(r for r in rows if r['case_id'] == 'v_repeat')
    # 除工作量和总耗时外，还比较仲裁次数、平均排队时间和停顿时间。
    for k in ('bytes','requests','executed_ops','sim_ticks','grants','avg_queue_ns','stall_ns'):
        check('determinism', k, repeat[k], first[k], repeat[k] == first[k])

    # 第五部分：写出所有检查明细，包括失败项，便于定位问题。
    # 'w' 会创建或覆盖目标文件；newline='' 让 csv 模块处理换行。
    # with 会在代码块结束时自动关闭文件；目标文件的父目录需要已存在。
    with Path(destination).open('w', newline='') as f:
        # 用第一条检查的字典键作为列名，先写表头，再写全部检查记录。
        w=csv.DictWriter(f, fieldnames=checks[0].keys()); w.writeheader(); w.writerows(checks)
    # 筛出 passed 为 False 的记录；非空列表表示至少有一项失败。
    failures = [c for c in checks if not c['passed']]
    if failures:
        # CSV 已保存，再抛出异常；indent=2 让异常中的 JSON 按两空格缩进。
        raise RuntimeError(json.dumps(failures, indent=2))
    # 只有全部通过才返回检查条数，供命令行打印。
    return len(checks)

# 脚本入口：直接运行本文件才执行；被其他 Python 文件 import 时不执行。
# 仓库根目录下的示例：
# python scripts/mvp5/validate_results.py results/mvp5/my-run
if __name__ == '__main__':
    # 创建解析器；directory 是必填的位置参数，不需要写 --directory。
    # type=Path 将目录字符串转为路径对象；parse_args() 将参数保存到 a。
    # 分号 ; 将三条语句放在一行，相当于依次分三行执行。
    p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); a=p.parse_args()
    # a.directory / 'results.json' 用 / 拼接路径，此处不是数值除法。
    # read_text() 读取文件文本，json.loads() 将 JSON 文本转为 Python 列表/字典。
    # validate(结果列表, 报告路径) 执行检查并写出 validation.csv。
    # 全部通过后打印类似“100 checks passed”；验证抛出异常时不会打印成功信息。
    print(validate(json.loads((a.directory/'results.json').read_text()),a.directory/'validation.csv'), 'checks passed')
