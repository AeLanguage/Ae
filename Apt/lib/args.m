// =============================================================================
//  args —— Ae 的命令行参数解析库（★ 用 Ae 写）
// -----------------------------------------------------------------------------
//  和 os 的分工：
//      os.args()   → 【原始】参数表（系统给了什么，字符串数组）
//      args（本库）→ 【解析】规格化的选项（短/长选项、取值、--、帮助、错误）
//
//  用法：
//      imp args
//
//      local spec = {
//          {name = "verbose", short = "v", kind = "flag",   help = "输出详细信息"},
//          {name = "out",     short = "o", kind = "option", help = "输出文件", default = "out.txt"},
//          {name = "count",   short = "c", kind = "option", help = "重复次数"},
//          {name = "input",   short = "i", kind = "option", help = "输入目录", multiple = true},
//      }
//
//      local a = args.parseOrExit(spec)     // 出错/请求帮助会自动打印并退出
//      prln(a.verbose, a.out, a.count, a.input, a.rest)
//
//  支持的写法：
//      -v            短选项（flag/count）
//      -vo out.txt   短选项可以连写；遇到需要值的选项，剩下的字符当它的值
//      -o out.txt    值取下一个参数
//      --out=x       长选项用 = 接值
//      --out x       长选项的值取下一个参数
//      --            之后的全部算位置参数
//      -             单独一个横线算位置参数（约定表示标准输入）
//      以 - 开头但后面是数字（如 -5）算位置参数
//
//  -h / --help 是【短路】的：一旦出现就立刻返回（help = true、error = null），
//  后面的参数不再解析，required 也不再校验 —— 这是"请求帮助"该有的行为。
//
//  导出函数：
//      parse(spec)                 用 os.args()（含程序名，会自动跳过 argv[0]）
//      parseArgv(spec, argv)       用给定的参数数组（★ 测试/复用用这个，可复现）
//      parseOrExit(spec)           出错就直接打印用法并 exit(3)（Ae 的"用法错误"码），
//                                  -h/--help 时打印帮助并 exit(0)
//      usage(spec)                帮助文本（程序名自动推导）
//      usageNamed(spec, prog)     帮助文本（指定程序名）
//      intOr(s, def) / floatOr(s, def)   选项值都是字符串，转换失败给默认值（不抛错）
//  帮助文本按【显示宽度】对齐（中文/全角/表情算 2 列）。
//
//  spec 每项支持的字段：
//      name      长选项名（必填）
//      short     单字符短选项（可选）
//      kind      "flag"（默认）| "option"（取值）| "count"（可重复，计数）
//      help      帮助文本
//      default   option 的默认值
//      required  必填（缺了报错）
//      multiple  option 可重复，值收进一个数组
//
//  返回表字段：
//      每个选项名 → 值（flag 是 bool，count 是 int，option 是 string/数组/null）
//      rest       位置参数数组
//      help       是否请求了 -h/--help
//      error      解析错误信息（null 表示没错误）★ 预期失败用返回值，不抛异常
//      prog       推导出的程序名（取自 os.scriptPath()）
//
//  解析错误不抛异常（这是"预期失败"），要省事就用 parseOrExit()。
// =============================================================================
imp os
imp path

// ── 对外入口 ────────────────────────────────────────────────────────────────

func parse(spec) { return parseArgv(spec, os.args()) }

func parseOrExit(spec) {
    local r = parse(spec)
    if (r.error != null) {
        prln("[-] 参数错误: " + r.error)
        prln("")
        prln(usageNamed(spec, r.prog))
        exit(3)                                  // 退出码 3 = 用法错误（与工具链一致）
    }
    if (r.help) {
        prln(usageNamed(spec, r.prog))
        exit(0)
    }
    return r
}

func usage(spec) { return usageNamed(spec, _progName()) }

func usageNamed(spec, prog) {
    local out = "用法: " + prog + " [选项] [参数...]\n\n选项:\n"
    local w = 0
    local rows = {}
    local i = 1
    while (i <= len(spec)) {
        local d = spec[i]
        local left = "  "
        if (d.short != null) { left += "-" + d.short + ", " } else { left += "    " }
        left += "--" + d.name
        local kind = d.kind
        if (kind == null) { kind = "flag" }
        if (kind == "option") { left += " <值>" }
        local right = ""
        if (d.help != null) { right = d.help }
        if (d.default != null) { right += " (默认: " + str(d.default) + ")" }
        if (d.multiple != null) { if (d.multiple) { right += " (可重复)" } }
        if (d.required != null) { if (d.required) { right += " [必填]" } }
        if (_width(left) > w) { w = _width(left) }
        rows[i] = {left = left, right = right}
        i += 1
    }
    i = 1
    while (i <= len(rows)) {
        out += _padTo(rows[i].left, w + 2) + rows[i].right + "\n"
        i += 1
    }
    out += _padTo("  -h, --help", w + 2) + "显示本帮助\n"
    return out
}

// 便捷转换（选项值都是字符串，转换失败给默认值而不是抛错）
func intOr(s, def) {
    if (s == null) { return def }
    try { return int(s) } catch (e) { return def }
}

func floatOr(s, def) {
    if (s == null) { return def }
    try { return float(s) } catch (e) { return def }
}

// ── 解析实现 ────────────────────────────────────────────────────────────────

func parseArgv(spec, argv) {
    local r = _newResult(spec)
    if (r.error != null) { return r }
    local i = 1
    local onlyRest = false
    while (i <= len(argv)) {
        local tok = argv[i]
        if (onlyRest) {
            append(r.rest, tok)
            i += 1
            continue
        }
        if (tok == "--") {
            onlyRest = true
            i += 1
            continue
        }
        if (_startsWith(tok, "--")) {
            // ── 长选项：--name / --name=value ──
            local body = substr(tok, 3, len(tok) - 2)
            local eq = find(body, "=")
            local nm = body
            local inlineVal = null
            if (eq != null) {
                nm = substr(body, 1, eq - 1)
                inlineVal = substr(body, eq + 1, len(body) - eq)
            }
            if (nm == "help") { r.help = true  return r }   // ★ -h/--help 短路：不再校验必填
            local d = _findLong(spec, nm)
            if (d == null) { return _err(r, "未知选项 --" + nm) }
            if (_kindOf(d) == "option") {
                if (inlineVal != null) {
                    r = _apply(r, d, inlineVal)
                    i += 1
                } else {
                    if (i + 1 > len(argv)) { return _err(r, "选项 --" + nm + " 需要一个值") }
                    local nxt = argv[i + 1]
                    if (_looksLikeFlag(nxt)) {
                        return _err(r, "选项 --" + nm + " 需要一个值（值本身以 - 开头时请写成 --" + nm + "=值）")
                    }
                    r = _apply(r, d, nxt)
                    i += 2
                }
            } else {
                if (inlineVal != null) { return _err(r, "选项 --" + nm + " 不接受值") }
                r = _apply(r, d, null)
                i += 1
            }
            if (r.error != null) { return r }
            continue
        }
        if (len(tok) >= 2 and substr(tok, 1, 1) == "-" and not _looksLikeNegative(tok)) {
            // ── 短选项簇：-v / -vo out.txt / -o out.txt ──
            local k = 2
            local consumedNext = false
            while (k <= len(tok)) {
                local ch = tok[k]
                if (ch == "h") { r.help = true  return r }      // ★ 短路：不再校验必填
                local d = _findShort(spec, ch)
                if (d == null) { return _err(r, "未知选项 -" + ch) }
                if (_kindOf(d) == "option") {
                    local rest = substr(tok, k + 1, len(tok) - k)
                    if (len(rest) > 0) {
                        r = _apply(r, d, rest)          // -ofile.txt
                    } else {
                        if (i + 1 > len(argv)) { return _err(r, "选项 -" + ch + " 需要一个值") }
                        local nxt = argv[i + 1]
                        if (_looksLikeFlag(nxt)) {
                            return _err(r, "选项 -" + ch + " 需要一个值（值本身以 - 开头时请写成 -" + ch + " 值 或用长选项 --" + d.name + "=值）")
                        }
                        r = _apply(r, d, nxt)
                        consumedNext = true
                    }
                    if (r.error != null) { return r }
                    k = len(tok) + 1                    // 该选项吃掉了本簇剩余部分
                    continue
                }
                r = _apply(r, d, null)                  // flag / count
                if (r.error != null) { return r }
                k += 1
            }
            if (consumedNext) { i += 2 } else { i += 1 }
            continue
        }
        append(r.rest, tok)
        i += 1
    }
    // required 检查
    i = 1
    while (i <= len(spec)) {
        local d = spec[i]
        if (d.required != null) {
            if (d.required) {
                local v = r[d.name]
                if (v == null or v == false) {
                    return _err(r, "缺少必填选项 --" + d.name)
                }
            }
        }
        i += 1
    }
    return r
}

func _newResult(spec) {
    local r = {rest = {}, help = false, error = null, prog = _progName()}
    local i = 1
    while (i <= len(spec)) {
        local d = spec[i]
        if (d.name == null) { return _err(r, "spec 第 " + str(i) + " 项缺少 name") }
        local k = _kindOf(d)
        if (k == "flag") { r[d.name] = false }
        else if (k == "count") { r[d.name] = 0 }
        else {
            if (d.multiple != null) {
                if (d.multiple) { r[d.name] = {} } else { r[d.name] = null }
            } else { r[d.name] = null }
            if (d.default != null) {
                if (d.multiple != null) {
                    if (not d.multiple) { r[d.name] = d.default }
                } else { r[d.name] = d.default }
            }
        }
        i += 1
    }
    return r
}

func _apply(r, d, val) {
    local k = _kindOf(d)
    if (k == "flag")   { r[d.name] = true  return r }
    if (k == "count")  { r[d.name] = r[d.name] + 1  return r }
    if (d.multiple != null) {
        if (d.multiple) {
            append(r[d.name], val)
            return r
        }
    }
    r[d.name] = val
    return r
}

func _err(r, msg) {
    if (r.error == null) { r.error = msg }
    return r
}

func _kindOf(d) {
    if (d.kind == null) { return "flag" }
    local k = d.kind
    if (k == "option" or k == "flag" or k == "count") { return k }
    error("args: 不认识的 kind '" + k + "'（只支持 flag / option / count）")
    return "flag"
}

func _findLong(spec, nm) {
    local i = 1
    while (i <= len(spec)) {
        if (spec[i].name == nm) { return spec[i] }
        i += 1
    }
    return null
}

func _findShort(spec, ch) {
    local i = 1
    while (i <= len(spec)) {
        local s = spec[i].short
        if (s != null) {
            if (s == ch) { return spec[i] }
        }
        i += 1
    }
    return null
}

// 下一个参数"看起来像选项"吗（用来防止 --out --verbose 静默吃掉 --verbose）
func _looksLikeFlag(s) {
    if (len(s) < 2) { return false }
    if (substr(s, 1, 1) != "-") { return false }
    return not _looksLikeNegative(s)
}

func _looksLikeNegative(s) {
    if (len(s) < 2) { return false }
    if (substr(s, 1, 1) != "-") { return false }
    local c = substr(s, 2, 1)
    return c >= "0" and c <= "9"
}

func _startsWith(s, pre) {
    if (len(s) < len(pre)) { return false }
    return substr(s, 1, len(pre)) == pre
}

// 显示宽度：CJK / 全角 / 表情占 2 列，其余占 1 列。
// 帮助文本对齐必须按显示宽度算，按 len()（码点数）算会让中文行错位。
func _width(s) {
    local n = 0
    local i = 1
    while (i <= len(s)) {
        local c = ord(substr(s, i, 1))
        local wide = false
        if (c >= 0x1100 and c <= 0x115F) { wide = true }
        if (c >= 0x2E80 and c <= 0xA4CF) { wide = true }
        if (c >= 0xAC00 and c <= 0xD7A3) { wide = true }
        if (c >= 0xF900 and c <= 0xFAFF) { wide = true }
        if (c >= 0xFE30 and c <= 0xFE6F) { wide = true }
        if (c >= 0xFF00 and c <= 0xFF60) { wide = true }
        if (c >= 0xFFE0 and c <= 0xFFE6) { wide = true }
        if (c >= 0x1F300 and c <= 0x1FAFF) { wide = true }
        if (c >= 0x20000 and c <= 0x3FFFD) { wide = true }
        if (wide) { n += 2 } else { n += 1 }
        i += 1
    }
    return n
}

func _padTo(s, n) {
    local out = s
    while (_width(out) < n) { out += " " }
    return out
}

func _progName() {
    local sp = os.scriptPath()
    if (sp == null) { return "程序" }
    if (len(sp) == 0) { return "程序" }
    local st = path.stem(sp)
    if (st == null) { return sp }
    if (len(st) == 0) { return sp }
    return st
}
