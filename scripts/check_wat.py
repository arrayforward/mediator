import re
import sys

lines = open(sys.argv[1]).read().split("\n")
depth = 0
for i, ch in enumerate(lines, 1):
    code = re.sub(r";;.*", "", ch)
    in_str = False
    j = 0
    while j < len(code):
        c = code[j]
        if c == '"':
            # 处理 \" 与 \\ 转义
            bs = 0
            k = j - 1
            while k >= 0 and code[k] == "\\":
                bs += 1
                k -= 1
            if bs % 2 == 0:
                in_str = not in_str
        if not in_str:
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
        j += 1
    if depth <= 0 or i >= 96:
        print(i, depth, ch[:60])
