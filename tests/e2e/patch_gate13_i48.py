# -*- coding: utf-8 -*-
"""给 Gate13 注入 I48 反丢失断言（一次性补丁脚本）。"""
import re

path = r'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\scripts\check_poolcopy_contract.py'
t = open(path, encoding='utf-8').read()

if 'check_i48_data_safe_transition' in t:
    print('already patched')
    raise SystemExit(0)

add = '''
def check_i48_data_safe_transition(root, fails, warns):
    """[I48 2026-11-10] WB/Enabled 降档必须 fail-closed：
    RbDiscardAllCachedData() 只能出现在 dirty_after 守卫之后，且失败分支必须
    return STATUS_IO_DEVICE_ERROR。旧实现「忽略 flush 结果 + 无条件 discard
    （core 计入 lost）」曾静默丢弃已 ack 脏数据（e4/e5 探针实证 7.87MB）。"""
    p = root / 'src' / 'driver' / 'rbctl.c'
    try:
        text = p.read_text(encoding='utf-8')
    except OSError as e:
        fails.append('I48: rbctl.c unreadable: %s' % e)
        return
    idx = text.find('RbDiscardAllCachedData()')
    if idx < 0:
        fails.append('I48: RbDiscardAllCachedData call site missing')
        return
    window = text[max(0, idx - 2200):idx]
    if 'dirty_after' not in window:
        fails.append('I48: discard site without dirty_after guard (data-loss regression)')
    if 'STATUS_IO_DEVICE_ERROR' not in window:
        fails.append('I48: transition refusal return missing (fail-open regression)')
    if window.find('dirty_after != 0') < 0:
        fails.append('I48: dirty_after != 0 guard missing before discard')
    if 'no dirty' not in text:
        warns.append('I48: success log lacks no-dirty variant (cosmetic)')


'''

anchor = 'def main('
assert anchor in t
t = t.replace(anchor, add + anchor, 1)

m = re.search(r'check_[a-z0-9_]+\(root, fails, warns\)', t)
assert m, 'no check call found'
t = t.replace(m.group(0), m.group(0) + '\n    check_i48_data_safe_transition(root, fails, warns)', 1)
open(path, 'w', encoding='utf-8').write(t)
print('gate13 extended')
