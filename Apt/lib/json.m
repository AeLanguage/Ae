// =============================================================================
//  json —— Ae 的 JSON 库（★ 用 Ae 自己写，不是 C++）
// -----------------------------------------------------------------------------
//  用法：
//      imp json
//      local cfg = json.decode(readFile("cfg.json"))
//      prln(cfg.name)
//      writeFile("out.json", json.encodePretty(cfg, 2))
//
//  导出函数（不带 _ 前缀的才会导出）：
//      decode(text)             → 值 | 抛错(E2008)
//      decodeFile(path)         → 值 | 抛错
//      encode(v)                → 紧凑 JSON 字符串
//      encodePretty(v)          → 2 空格缩进的可读 JSON
//      encodePrettyN(v, 缩进)    → 自定义缩进（Ae 没有默认参数，所以拆成两个）
//      encodeFile(path, v)      → bool | 抛错
//
//  类型映射：
//      JSON null/bool/number/string  ↔  Ae null/bool/int|float/string
//      JSON array  ↔ Ae 表【数组段】（判定：count == len，即连续无空洞）
//      JSON object ↔ Ae 表【哈希段】（键统一转成字符串）
//      数字：带小数点或 e/E 的解析为 float，否则 int；编码时 float 用最短往返表示
//
//  已知限制（写在这里而不是藏着）：
//    · Ae 的表无法区分"值为 null"与"空洞"，所以 JSON 数组里出现 null 时
//      解码会明确报错，而不是悄悄丢数据（要表达空位请用字符串 "null" 或改对象）
//    · 非连续整数键的表（如 t = {1,2}; t[5] = 3）会被编码成对象而不是数组
//    · 不产生/不接受 inf、nan（JSON 没有这两个值）
// =============================================================================
imp io

// ── 编码 ────────────────────────────────────────────────────────────────────

func encode(v) { return _enc(v, 1, "  ", false) }

func encodePretty(v) { return _enc(v, 1, "  ", true) }

func encodePrettyN(v, indent) {
    local unit = ""
    local i = 0
    while (i < indent) {
        unit += " "
        i += 1
    }
    return _enc(v, 1, unit, true)
}

func encodeFile(path, v) {
    local ok = io.writeFile(path, encode(v))
    if (not ok) { error("json.encodeFile 写文件失败: " + io.lastError()) }
    return ok
}

func _enc(v, depth, unit, pretty) {
    local tv = type(v)
    if (tv == "null")   { return "null" }
    if (tv == "bool")   { if (v) { return "true" } return "false" }
    if (tv == "int")    { return str(v) }
    if (tv == "float") {
        local s = str(v)
        if (s == "inf" or s == "-inf" or s == "nan") { error("json: 不支持 " + s) }
        return s
    }
    if (tv == "string") { return "\"" + _esc(v) + "\"" }
    if (tv != "table")  { error("json: 无法编码 " + tv + " 类型") }

    local n = count(v)
    if (n == 0) {
        if (len(v) == 0) { return "[]" }      // 空表当空数组
        return "{}"
    }
    if (count(v) == len(v)) {                 // 连续数组段 → JSON array
        local out = "["
        local i = 1
        while (i <= len(v)) {
            if (i > 1) { out += "," }
            if (pretty) { out += "\n" + _ind(depth, unit) }
            out += _enc(v[i], depth + 1, unit, pretty)
            i += 1
        }
        if (pretty) { out += "\n" + _ind(depth - 1, unit) }
        return out + "]"
    }
    local out = "{"
    local first = true
    for k, val in v {
        if (not first) { out += "," }
        first = false
        if (pretty) { out += "\n" + _ind(depth, unit) }
        out += "\"" + _esc(str(k)) + "\""
        if (pretty) { out += ": " } else { out += ":" }
        out += _enc(val, depth + 1, unit, pretty)
    }
    if (pretty) { out += "\n" + _ind(depth - 1, unit) }
    return out + "}"
}

func _ind(depth, unit) {
    local s = ""
    local i = 0
    while (i < depth) {
        s += unit
        i += 1
    }
    return s
}

func _esc(s) {
    local out = ""
    local i = 1
    local n = len(s)
    while (i <= n) {
        local c = s[i]
        if (c == "\"")      { out += "\\\"" }
        else if (c == "\\") { out += "\\\\" }
        else if (c == "\n") { out += "\\n" }
        else if (c == "\r") { out += "\\r" }
        else if (c == "\t") { out += "\\t" }
        else if (ord(c) < 32) { out += "\\u" + _hex4(ord(c)) }
        else { out += c }
        i += 1
    }
    return out
}

func _hex4(code) {
    local digits = "0123456789abcdef"
    local s = ""
    local k = 3
    while (k >= 0) {
        local d = (code / (16 ** k)) % 16
        s += digits[d + 1]
        k -= 1
    }
    return s
}

// ── 解码 ────────────────────────────────────────────────────────────────────

func decode(text) {
    if (type(text) != "string") { error("json.decode 需要字符串") }
    local p = {i = 1}
    _skipWs(text, p)
    if (p.i > len(text)) { error("json: 内容是空的") }
    local v = _value(text, p)
    _skipWs(text, p)
    if (p.i <= len(text)) {
        error("json: 第 " + str(p.i) + " 个字符处有多余内容")
    }
    return v
}

func decodeFile(path) {
    local text = io.readFile(path)
    if (text == null) { error("json.decodeFile 读文件失败: " + io.lastError()) }
    return decode(text)
}

func _fail(msg, p) { error("json 解析失败（第 " + str(p.i) + " 个字符处）：" + msg) }

func _skipWs(text, p) {
    local n = len(text)
    while (p.i <= n) {
        local c = text[p.i]
        if (c == " " or c == "\t" or c == "\n" or c == "\r") { p.i += 1 }
        else { return }
    }
}

func _value(text, p) {
    _skipWs(text, p)
    local n = len(text)
    if (p.i > n) { _fail("内容意外结束", p) }
    local c = text[p.i]
    if (c == "{")  { return _object(text, p) }
    if (c == "[")  { return _array(text, p) }
    if (c == "\"") { return _string(text, p) }
    if (c == "t")  { _expect(text, p, "true")  return true }
    if (c == "f")  { _expect(text, p, "false") return false }
    if (c == "n")  { _expect(text, p, "null")  return null }
    return _number(text, p)
}

func _expect(text, p, word) {
    local got = substr(text, p.i, len(word))
    if (got != word) { _fail("期望 '" + word + "'，实际是 '" + got + "'", p) }
    p.i += len(word)
}

func _object(text, p) {
    local t = {}
    p.i += 1                                    // '{'
    _skipWs(text, p)
    if (p.i <= len(text) and text[p.i] == "}") { p.i += 1  return t }
    while (true) {
        _skipWs(text, p)
        if (p.i > len(text) or text[p.i] != "\"") { _fail("对象的键必须是字符串", p) }
        local k = _string(text, p)
        _skipWs(text, p)
        if (p.i > len(text) or text[p.i] != ":") { _fail("键后面需要 ':'", p) }
        p.i += 1
        t[k] = _value(text, p)
        _skipWs(text, p)
        if (p.i <= len(text) and text[p.i] == ",") { p.i += 1  continue }
        if (p.i <= len(text) and text[p.i] == "}") { p.i += 1  return t }
        _fail("对象里期望 ',' 或 '}'", p)
    }
    return t
}

func _array(text, p) {
    local t = {}
    p.i += 1                                    // '['
    _skipWs(text, p)
    if (p.i <= len(text) and text[p.i] == "]") { p.i += 1  return t }
    local idx = 1
    while (true) {
        local v = _value(text, p)
        if (v == null) {
            _fail("数组里出现 null —— Ae 的表无法区分 null 与空洞，请改用字符串 \"null\" 或对象表示", p)
        }
        t[idx] = v
        idx += 1
        _skipWs(text, p)
        if (p.i <= len(text) and text[p.i] == ",") { p.i += 1  continue }
        if (p.i <= len(text) and text[p.i] == "]") { p.i += 1  return t }
        _fail("数组里期望 ',' 或 ']'", p)
    }
    return t
}

func _string(text, p) {
    p.i += 1                                    // 开头的引号
    local out = ""
    local n = len(text)
    while (p.i <= n) {
        local c = text[p.i]
        if (c == "\"") { p.i += 1  return out }
        if (c == "\\") {
            p.i += 1
            if (p.i > n) { _fail("转义符后面没有内容", p) }
            local e = text[p.i]
            if (e == "n")       { out += "\n" }
            else if (e == "t")  { out += "\t" }
            else if (e == "r")  { out += "\r" }
            else if (e == "b")  { out += "\b" }
            else if (e == "f")  { out += "\f" }
            else if (e == "/")  { out += "/" }
            else if (e == "\\") { out += "\\" }
            else if (e == "\"") { out += "\"" }
            else if (e == "u") {
                if (p.i + 4 > n) { _fail("\\u 转义不完整", p) }
                out += chr(_hexVal4(text, p.i + 1))
                p.i += 4
            }
            else { _fail("不认识的转义 \\" + e, p) }
            p.i += 1
        } else {
            out += c
            p.i += 1
        }
    }
    _fail("字符串没有闭合的引号", p)
    return out
}

func _hexVal4(text, at) {
    local v = 0
    local k = 0
    while (k < 4) {
        local c = text[at + k]
        local d = -1
        if (c >= "0" and c <= "9") { d = ord(c) - 48 }
        else if (c >= "A" and c <= "F") { d = ord(c) - 55 }
        else if (c >= "a" and c <= "f") { d = ord(c) - 87 }
        if (d < 0) { error("json: \\u 转义里有非十六进制字符 '" + c + "'") }
        v = v * 16 + d
        k += 1
    }
    return v
}

func _number(text, p) {
    local n = len(text)
    local start = p.i
    local isFloat = false
    if (p.i <= n and text[p.i] == "-") { p.i += 1 }
    while (p.i <= n) {
        local c = text[p.i]
        if (c >= "0" and c <= "9") { p.i += 1 }
        else if (c == "." or c == "e" or c == "E" or c == "+" or c == "-") {
            isFloat = true
            p.i += 1
        }
        else { break }
    }
    local tok = substr(text, start, p.i - start)
    if (len(tok) == 0) { _fail("这里应该是一个数字", p) }
    if (isFloat) { return float(tok) }
    return int(tok)
}
