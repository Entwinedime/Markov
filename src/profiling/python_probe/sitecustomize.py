"""Python probe 自动入口。

runner 将 `src/profiling/python_probe` 加入 PYTHONPATH 后，Python 会自动导入
本文件。只有 `TRACE_SIM_PYTHON_PROBE=1` 时才安装 probe。
"""

try:
    from trace_sim_probe.bootstrap import bootstrap

    bootstrap()
except Exception:
    # probe 加载失败时必须保持被测进程继续启动，不能改变 server 原始启动行为。
    pass
