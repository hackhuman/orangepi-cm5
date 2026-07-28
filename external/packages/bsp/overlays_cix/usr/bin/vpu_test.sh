#!/bin/bash

if [ "$(id -u)" -ne 0 ]; then
    # 尝试使用 sudo 重新执行当前脚本，并传递所有参数
    sudo "$0" "$@"

    # 退出当前非 root 的进程
    exit $?
fi

echo 3 | tee /sys/kernel/debug/amvx/log/group/perf/enable > /dev/null

echo "开始监控VPU使用率（只显示大于0的情况）..."
echo "按 Ctrl+C 停止监控"

while true
do
    # 读取VPU使用率
    utilization=$(cat /sys/kernel/debug/amvx/log/group/perf/utilization | awk '{print $3}' | tr -d '%')

    # 只有当使用率大于0时才输出
    if (( $(echo "$utilization > 0" | bc -l) )); then
        # 获取当前时间
        current_time=$(date '+%H:%M:%S')

        # 读取实时FPS信息
        fps_info=$(cat /sys/kernel/debug/amvx/log/group/perf/realtime_fps)

        # 输出信息
        echo "VPU Utilization: ${utilization}%"
        if [ -n "$fps_info" ] && [ "$fps_info" != "null" ]; then
            echo "$current_time $fps_info"
        fi
    fi
done
