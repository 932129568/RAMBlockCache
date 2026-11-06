# Ground truth: exact backslash count before Registry in driver sources.
import glob, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
base = r'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\driver'
for p in sorted(glob.glob(base + r'\*.c')):
    text = open(p, 'rb').read().decode('utf-8', errors='replace')
    for i, line in enumerate(text.splitlines(), 1):
        if 'Registry' not in line:
            continue
        # count backslashes immediately before the R of Registry
        idx = 0
        while True:
            idx = line.find('Registry', idx)
            if idx < 0:
                break
            bs = 0
            j = idx - 1
            while j >= 0 and line[j] == '\\':
                bs += 1
                j -= 1
            if bs == 1 and 'L"' in line:   # exactly one backslash = broken escape
                print(f"{p.split(chr(92))[-1]}:{i}: bs={bs}  {line.strip()[:120]}")
            idx += 8
print('scan done')
