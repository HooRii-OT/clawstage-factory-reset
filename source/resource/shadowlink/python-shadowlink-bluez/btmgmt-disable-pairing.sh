#!/bin/bash
set -euo pipefail

MAX_RETRIES="${BTMGMT_RETRIES:-20}"
RETRY_DELAY="${BTMGMT_RETRY_DELAY:-1}"
SETTLE_DELAY="${BTMGMT_SETTLE_DELAY:-1}"
HCI_INDEX="${BTMGMT_HCI_INDEX:-0}"

if [[ ! "$MAX_RETRIES" =~ ^[0-9]+$ ]] || [[ "$MAX_RETRIES" -lt 1 ]]; then
    echo "[btmgmt] invalid BTMGMT_RETRIES: $MAX_RETRIES" >&2
    exit 1
fi

if [[ ! "$RETRY_DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "[btmgmt] invalid BTMGMT_RETRY_DELAY: $RETRY_DELAY" >&2
    exit 1
fi

if [[ ! "$SETTLE_DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "[btmgmt] invalid BTMGMT_SETTLE_DELAY: $SETTLE_DELAY" >&2
    exit 1
fi

if [[ ! "$HCI_INDEX" =~ ^[0-9]+$ ]]; then
    echo "[btmgmt] invalid BTMGMT_HCI_INDEX: $HCI_INDEX" >&2
    exit 1
fi

run_btmgmt() {
    local cmd
    printf -v cmd '%q ' /usr/bin/btmgmt --index "$HCI_INDEX" "$@"
    /usr/bin/script -q -c "$cmd" /dev/null
}

controller_ready() {
    local out
    out="$(run_btmgmt info 2>/dev/null || true)"

    # 既要能拿到 hci0 信息，也要能看到 current settings
    grep -q "hci${HCI_INDEX}:" <<<"$out" && grep -q "current settings:" <<<"$out"
}

bondable_is_off() {
    local out
    out="$(run_btmgmt info 2>/dev/null || true)"

    # current settings 这一行里不包含 bondable，才算真的关掉
    local current
    current="$(grep -F "current settings:" <<<"$out" || true)"

    [[ -n "$current" ]] && [[ "$current" != *"bondable"* ]]
}

for i in $(seq 1 "$MAX_RETRIES"); do
    echo "[btmgmt] attempt $i/$MAX_RETRIES"

    # 1) 先等控制器 ready
    if ! controller_ready; then
        echo "[btmgmt] controller not ready yet"
        sleep "$RETRY_DELAY"
        continue
    fi

    # 2) 先关 bondable
    if run_btmgmt bondable off; then
        echo "[btmgmt] bondable off command sent"
    else
        echo "[btmgmt] failed to send bondable off"
        sleep "$RETRY_DELAY"
        continue
    fi

    # 3) 确认 bondable 真的关掉
    bondable_ok=0
    for _ in 1 2 3 4 5; do
        if bondable_is_off; then
            bondable_ok=1
            break
        fi
        sleep 0.5
    done

    if [[ "$bondable_ok" -ne 1 ]]; then
        echo "[btmgmt] bondable is still on, retrying..."
        sleep "$RETRY_DELAY"
        continue
    fi
    echo "[btmgmt] verified: bondable is off"

    # 4) io-cap 没法从你当前的 btmgmt info/config 直接读回校验
    #    所以采用“控制器 ready 后执行两次 + 中间短暂 settle”的方式
    if ! run_btmgmt io-cap 3; then
        echo "[btmgmt] failed to send io-cap 3 (1st)"
        sleep "$RETRY_DELAY"
        continue
    fi

    sleep "$SETTLE_DELAY"

    if ! run_btmgmt io-cap 3; then
        echo "[btmgmt] failed to send io-cap 3 (2nd)"
        sleep "$RETRY_DELAY"
        continue
    fi

    # 5) 再给一个很短的稳定窗口，避免刚返回成功就立刻启动 Python
    sleep "$SETTLE_DELAY"

    echo "[btmgmt] setup complete"
    exit 0
done

echo "[btmgmt] failed after ${MAX_RETRIES} attempts"
exit 1
