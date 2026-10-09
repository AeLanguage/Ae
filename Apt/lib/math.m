// math —— Ae 数学库（纯 Ae 实现，零 C++ 依赖）
//   imp math  →  math.sqrt(2.0) / math.sin(math.pi() / 2) / math.round(3.14159, 2)
// ★ 常量仍是函数（math.pi()），历史 API 保持兼容
// ★ 定义域错误返回 nan 不抛错（能顺着算式传下去），溢出返回 inf；log(0) = -inf
// ★ 内部一律先 float(x)：Ae 不允许 int/float 直接比较（E2001）
// ★ double 精度：sqrt/cbrt/expm1 与 libm 逐位一致，其余 ≤5ulp；|x|>1e15 的三角不可信

imp os

// ── 私有常量（模块顶层变量，_ 开头不导出）─────────────────────────────────
_MAX = 1.7976931348623157e308
_INF = _MAX * 10.0                    // 字面量写不出 inf，靠乘法溢出算出来
_NAN = _INF - _INF                    // inf - inf = nan
_PI = 3.14159265358979323846
_TAU = 6.28318530717958647692
_E = 2.71828182845904523536
_LN2 = 0.69314718055994530942
_LN10 = 2.30258509299404568402
_SQ2 = 1.41421356237309504880
_SQ3 = 1.73205080756887729353
_PHI = 1.61803398874989484820
_P2H = 1.5707963267948966             // pi/2 的双-双表示（归约要用）
_P2L = 6.123233995736766e-17
_L2H = 0.6931471805599453             // ln2 的双-双表示
_L2L = 2.3190468138462996e-17
_B53 = 9007199254740992.0             // 2^53：超过它本身就是整数
_ULP = 0.00000000000000011102230246251565
_EXP = 709.782712893384               // exp 溢出阈值

// ── 私有工具 ──────────────────────────────────────────────────────────────
func _n(v)  { return v != v }                       // isNaN
func _ii(v) { return v > _MAX or v < 0.0 - _MAX }   // isInf
func _ab(v) { return v < 0.0 ? 0.0 - v : v }        // fabs（只吃 float）
func _k2(k) {                                       // 2^k，k 为 int（可负）
    local r = 1.0; local b = 2.0; local n = k
    if (n < 0) { b = 0.5; n = 0 - n }
    while (n > 0) { if (n % 2 == 1) { r = r * b }; b = b * b; n = n / 2 }
    return r
}

// ── 常量 ──────────────────────────────────────────────────────────────────
func pi()      { return _PI }
func tau()     { return _TAU }
func e()       { return _E }
func ln2()     { return _LN2 }
func ln10()    { return _LN10 }
func log2e()   { return 1.44269504088896340736 }
func log10e()  { return 0.43429448190325182765 }
func sqrt2()   { return _SQ2 }
func sqrt3()   { return _SQ3 }
func phi()     { return _PHI }
func epsilon() { return 0.0000000000000002220446049250313 }
func huge()    { return _INF }
func nan()     { return _NAN }

// ── 基本运算 ──────────────────────────────────────────────────────────────
func abs(x) { return type(x) == "int" ? (x < 0 ? 0 - x : x) : _ab(float(x)) }
func sign(x) { local v = float(x); return (_n(v) or v == 0.0) ? 0 : (v > 0.0 ? 1 : -1) }

// min/max 用变参：两个起步，多了照收（2 参调用行为与旧版完全一致）
func min(a, b, r...) {
    local m = (type(a) == "int" and type(b) == "int") ? (a < b ? a : b)
                                                      : (float(a) < float(b) ? float(a) : float(b))
    local i = 1
    while (i <= len(r)) { local c = r[i]; if (float(c) < float(m)) { m = c }; i = i + 1 }
    return m
}
func max(a, b, r...) {
    local m = (type(a) == "int" and type(b) == "int") ? (a > b ? a : b)
                                                      : (float(a) > float(b) ? float(a) : float(b))
    local i = 1
    while (i <= len(r)) { local c = r[i]; if (float(c) > float(m)) { m = c }; i = i + 1 }
    return m
}
func clamp(x, lo, hi) { local v = float(x); local a = float(lo); local b = float(hi); return v < a ? a : (v > b ? b : v) }
func lerp(a, b, t) { return float(a) + (float(b) - float(a)) * float(t) }

// ── 取整 ──────────────────────────────────────────────────────────────────
func floor(x) {
    local v = float(x)
    if ((_n(v) or _ii(v) or v > _B53 or v < 0.0 - _B53)) { return v }
    local f = float(int(v))
    return f > v ? f - 1.0 : f
}
func ceil(x)  { local v = float(x); return (_n(v) or _ii(v)) ? v : 0.0 - floor(0.0 - v) }
func trunc(x) { local v = float(x); return (_n(v) or _ii(v) or v > _B53 or v < 0.0 - _B53) ? v : float(int(v)) }
func frac(x)  { local v = float(x); return (_n(v) or _ii(v)) ? v : v - trunc(v) }

// round 合并了旧 roundN：round(x) == round(x, 0)
func round(x, n = 0) {
    local v = float(x)
    if (_n(v) or _ii(v)) { return v }
    local k = int(n); local s = 1.0; local i = 0
    while (i < k) { s = s * 10.0; i = i + 1 }
    i = 0
    while (i > k) { s = s / 10.0; i = i - 1 }
    return (v >= 0.0 ? floor(v * s + 0.5) : 0.0 - floor(0.0 - v * s + 0.5)) / s
}
func roundN(x, n) { return round(x, n) }

// ── 幂 / 指数 / 对数 ──────────────────────────────────────────────────────
func sqrt(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (v < 0.0) { return _NAN }
    if (_ii(v) or v == 0.0) { return v }
    local m = v; local k = 0
    while (m >= 1.0) { m = m * 0.25; k = k + 1 }
    while (m < 0.25) { m = m * 4.0; k = k - 1 }
    local g = 0.5 + 0.5 * m; local i = 0
    while (i < 60) { local t = 0.5 * (g + m / g); if (t == g) { break }; g = t; i = i + 1 }
    // 牛顿不动点可能停在真值 ±1ulp：用精确乘法求真实残差，只在更近时才挪一格
    local hi, lo = _tp(g, g); local d = (hi - m) + lo
    if (d < 0.0) { if (0.0 - d > _ULP * (g + 0.5 * _ULP)) { g = g + _ULP } }
    else { if (d > _ULP * (g - 0.5 * _ULP)) { g = g - _ULP } }
    local r = g; local j = 0
    while (j < k) { r = r * 2.0; j = j + 1 }
    j = 0
    while (j > k) { r = r * 0.5; j = j - 1 }
    return r
}
func cbrt(x) {
    local v = float(x)
    if (_n(v) or _ii(v) or v == 0.0) { return v }
    local g = v < 0.0
    if (g) { v = 0.0 - v }
    local r = exp(log(v) / 3.0); local i = 0
    while (i < 4) { local t = r - (r * r * r - v) / (3.0 * r * r); if (t == r) { break }; r = t; i = i + 1 }
    return g ? 0.0 - r : r
}
func hypot(x, y) {
    local a = _ab(float(x)); local b = _ab(float(y))
    if (_ii(a) or _ii(b)) { return _INF }
    local m = a > b ? a : b
    if (m == 0.0) { return 0.0 }
    local p = a / m; local q = b / m
    return m * sqrt(p * p + q * q)
}
func exp(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (v > _EXP) { return _INF }
    if (v < -745.1332191019411) { return 0.0 }
    local kf = floor(v / _L2H)
    // 钳在 -1074 以上再转 int：v=-1e308 时 k≈-1.4e308，int() 会直接溢出报错
    if (kf < -1074.0) { kf = -1074.0 }
    local k = int(kf); local fk = float(k)
    local p1, e1 = _tp(fk, _L2H); local p2, e2 = _tp(fk, _L2L)
    local r = ((v - p1) - p2) - (e1 + e2)
    local t = 1.0; local s = 1.0; local n = 1; local i = 0
    while (i < 200) { t = t * r / float(n); local pv = s; s = s + t; if (s == pv) { break }; n = n + 1; i = i + 1 }
    return k == 0 ? s : s * _k2(k)
}
func expm1(x) {
    local v = float(x)
    if (v > -0.5 and v < 0.5) {
        local s = 0.0; local t = v; local n = 1; local i = 0
        while (i < 30) { s = s + t; t = t * v / float(n + 1); n = n + 1; i = i + 1 }
        return s
    }
    return exp(v) - 1.0
}
func log(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (v < 0.0) { return _NAN }
    if (v == 0.0) { return 0.0 - _INF }
    if (_ii(v)) { return v }
    local k = 0; local m = v
    while (m >= 1.4142135623730951) { m = m * 0.5; k = k + 1 }
    while (m < 0.7071067811865476) { m = m * 2.0; k = k - 1 }
    local t = (m - 1.0) / (m + 1.0); local t2 = t * t; local tm = t; local s = 0.0; local n = 1; local i = 0
    while (i < 300) { local d = tm / float(n); local pv = s; s = s + d; if (s == pv) { break }; tm = tm * t2; n = n + 2; i = i + 1 }
    return 2.0 * s + float(k) * _LN2
}
func log2(x)    { return log(x) / _LN2 }
func log10(x)   { return log(x) / _LN10 }
func logN(x, b) { return log(x) / log(b) }
func pow(x, y) {
    local b = float(x)
    if (_n(b) or _n(float(y))) { return _NAN }
    if (type(y) == "int") {
        local n = y
        if (n == 0) { return 1.0 }
        if (n < -4611686018427387904) { return exp(float(n) * log(b)) }
        local g = false
        if (n < 0) { g = true; n = 0 - n }
        local r = 1.0; local s = b
        while (n > 0) { if (n % 2 == 1) { r = r * s }; s = s * s; n = n / 2 }
        return g ? (r == 0.0 ? _INF : 1.0 / r) : r
    }
    local p = float(y)
    if (p == 0.0 or b == 1.0) { return 1.0 }
    if (b == 0.0) { return p > 0.0 ? 0.0 : _INF }
    if (b < 0.0) { return _NAN }
    return exp(p * log(b))
}

// ── 三角 ──────────────────────────────────────────────────────────────────
func _sk(r) {   // |r| <= pi/4 的正弦级数
    local r2 = r * r; local t = r; local s = r; local n = 1; local i = 0
    while (i < 80) { t = 0.0 - t * r2 / float((2 * n) * (2 * n + 1)); local pv = s; s = s + t; if (s == pv) { break }; n = n + 1; i = i + 1 }
    return s
}
func _ck(r) {   // 余弦级数
    local r2 = r * r; local t = 1.0; local s = 1.0; local n = 1; local i = 0
    while (i < 80) { t = 0.0 - t * r2 / float((2 * n - 1) * (2 * n)); local pv = s; s = s + t; if (s == pv) { break }; n = n + 1; i = i + 1 }
    return s
}
func _sp(a) { local c = 134217729.0 * a; local h = c - (c - a); return h, a - h }   // Dekker 拆分
func _tp(a, b) {                                                                     // 精确乘积 p+err
    local ah, al = _sp(a); local bh, bl = _sp(b); local p = a * b
    return p, ((ah * bh - p) + ah * bl + al * bh) + al * bl
}
func _qd(v) {   // 归约到 [-pi/4, pi/4]，返回 (象限 0..3, 余项)；双-双避免大参数灾难性抵消
    local n = int(floor(v / _P2H + 0.5)); local fn = float(n)
    local p1, e1 = _tp(fn, _P2H); local p2, e2 = _tp(fn, _P2L)
    local r = ((v - p1) - p2) - (e1 + e2)
    local q = n % 4
    if (q < 0) { q = q + 4 }
    return q, r
}
func sin(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v) or v > 1.0e15 or v < -1.0e15) { return _NAN }
    local q, r = _qd(v)
    if (q == 0) { return _sk(r) }
    if (q == 1) { return _ck(r) }
    if (q == 2) { return 0.0 - _sk(r) }
    return 0.0 - _ck(r)
}
func cos(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v) or v > 1.0e15 or v < -1.0e15) { return _NAN }
    local q, r = _qd(v)
    if (q == 0) { return _ck(r) }
    if (q == 1) { return 0.0 - _sk(r) }
    if (q == 2) { return 0.0 - _ck(r) }
    return _sk(r)
}
func tan(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v)) { return _NAN }
    local c = cos(v)
    return c == 0.0 ? _INF : sin(v) / c
}
func _ak(r) {   // atan 级数，|r| <= tan(pi/12)
    local r2 = r * r; local t = r; local s = r; local n = 1; local i = 0
    while (i < 200) { t = 0.0 - t * r2; local d = t / float(2 * n + 1); local pv = s; s = s + d; if (s == pv) { break }; n = n + 1; i = i + 1 }
    return s
}
func _ac(v) {   // 0 <= v <= 1：半角归约到 <= tan(pi/12) 再展开（避免 v*v 溢出）
    local k = 0; local i = 0
    while (v > 0.2679491924311227 and i < 60) { v = v / (1.0 + sqrt(1.0 + v * v)); k = k + 1; i = i + 1 }
    local r = _ak(v); local j = 0
    while (j < k) { r = r * 2.0; j = j + 1 }
    return r
}
func atan(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v)) { return v > 0.0 ? _PI * 0.5 : 0.0 - _PI * 0.5 }
    local g = v < 0.0
    if (g) { v = 0.0 - v }
    local r = v > 1.0 ? _PI * 0.5 - _ac(1.0 / v) : _ac(v)
    return g ? 0.0 - r : r
}
func atan2(y, x) {
    local a = float(y); local b = float(x)
    if (_n(a) or _n(b)) { return _NAN }
    if (_ii(a) and _ii(b)) { return a > 0.0 ? _PI * 0.25 : 0.0 - _PI * 0.75 }
    if (_ii(a)) { return a > 0.0 ? _PI * 0.5 : 0.0 - _PI * 0.5 }
    if (_ii(b)) { return b > 0.0 ? 0.0 : _PI }
    if (b > 0.0) { return atan(a / b) }
    if (b < 0.0) { return a >= 0.0 ? atan(a / b) + _PI : atan(a / b) - _PI }
    if (a > 0.0) { return _PI * 0.5 }
    if (a < 0.0) { return 0.0 - _PI * 0.5 }
    return 0.0
}
func asin(x) {
    local v = float(x)
    if (_n(v) or _ii(v) or v > 1.0 or v < -1.0) { return _NAN }
    if (v == 1.0) { return _PI * 0.5 }
    if (v == -1.0) { return 0.0 - _PI * 0.5 }
    return atan(v / sqrt((1.0 - v) * (1.0 + v)))
}
// acos 不走 pi/2 - asin(v)：x→±1 时两个接近的数相减，实测 x=0.999999 误差 3.6e-14
func acos(x) {
    local v = float(x)
    if (_n(v) or _ii(v) or v > 1.0 or v < -1.0) { return _NAN }
    if (v == 1.0) { return 0.0 }
    if (v == -1.0) { return _PI }
    return v >= 0.0 ? 2.0 * asin(sqrt((1.0 - v) * 0.5)) : _PI - 2.0 * asin(sqrt((1.0 + v) * 0.5))
}
func rad(x) { return float(x) * _PI / 180.0 }
func deg(x) { return float(x) * 180.0 / _PI }

// ── 双曲（一律走 expm1，避免 |x| 很小时 e^x-e^-x 的灾难性抵消）────────────
func sinh(x) {
    local v = float(x)
    if (_n(v) or _ii(v)) { return v }
    if (v > _EXP) { return _INF }
    if (v < 0.0 - _EXP) { return 0.0 - _INF }
    local a = _ab(v); local s = 0.0
    if (a >= 1.0) { local t = exp(a); s = (t - 1.0 / t) * 0.5 }
    else { local m = expm1(a); s = m * (2.0 + m) / (2.0 * (1.0 + m)) }
    return v < 0.0 ? 0.0 - s : s
}
func cosh(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v) or v > _EXP or v < 0.0 - _EXP) { return _INF }
    local a = _ab(v)
    if (a >= 1.0) { local t = exp(a); return (t + 1.0 / t) * 0.5 }
    local m = expm1(a)
    return 1.0 + m * m / (2.0 * (1.0 + m))
}
func tanh(x) {
    local v = float(x)
    if (_n(v)) { return v }
    if (_ii(v)) { return v > 0.0 ? 1.0 : -1.0 }
    if (v > 20.0) { return 1.0 }
    if (v < -20.0) { return -1.0 }
    local b = expm1(2.0 * _ab(v)); local t = b / (b + 2.0)
    return v < 0.0 ? 0.0 - t : t
}
func asinh(x) {
    local v = float(x)
    if (_n(v) or _ii(v) or v == 0.0) { return v }
    local g = v < 0.0
    if (g) { v = 0.0 - v }
    local r = log(v + sqrt(v * v + 1.0))
    return g ? 0.0 - r : r
}
func acosh(x) {
    local v = float(x)
    if (_n(v) or _ii(v)) { return v }
    if (v < 1.0) { return _NAN }
    if (v == 1.0) { return 0.0 }
    return log(v + sqrt((v - 1.0) * (v + 1.0)))
}
func atanh(x) {
    local v = float(x)
    if (_n(v) or v > 1.0 or v < -1.0) { return _NAN }
    if (v == 1.0) { return _INF }
    if (v == -1.0) { return 0.0 - _INF }
    return 0.5 * log((1.0 + v) / (1.0 - v))
}

// ── 取余 / 整数 ───────────────────────────────────────────────────────────
func fmod(x, y) { local a = float(x); local b = float(y); return (b == 0.0 or _ii(a) or _n(a) or _n(b)) ? _NAN : a % b }
func mod(x, y) {
    local b = float(y)
    if (b == 0.0) { return _NAN }
    local r = float(x) % b
    if (r != 0.0) { if (r < 0.0 and b > 0.0) { r = r + b }; if (r > 0.0 and b < 0.0) { r = r + b } }
    return r
}
func gcd(a, b) {
    local x = int(a); local y = int(b)
    if (x < 0) { x = 0 - x }
    if (y < 0) { y = 0 - y }
    while (y != 0) { local t = x % y; x = y; y = t }
    return x
}
func lcm(a, b) { local x = int(a); local y = int(b); return (x == 0 or y == 0) ? 0 : (x / gcd(x, y)) * y }
func factorial(n) {
    local k = int(n)
    if (k < 0) { return _NAN }
    if (k > 170) { return _INF }
    local r = 1.0; local i = 2
    while (i <= k) { r = r * float(i); i = i + 1 }
    return r
}
func comb(n, k) {
    local N = int(n); local K = int(k)
    if (N < 0 or K < 0 or K > N) { return 0.0 }
    if (K > N - K) { K = N - K }
    local r = 1.0; local i = 1
    while (i <= K) { r = r * float(N - K + i) / float(i); i = i + 1 }
    return r
}
func perm(n, k) {
    local N = int(n); local K = int(k)
    if (N < 0 or K < 0 or K > N) { return 0.0 }
    local r = 1.0; local i = 0
    while (i < K) { r = r * float(N - i); i = i + 1 }
    return r
}

// ── 判定 ──────────────────────────────────────────────────────────────────
func isNaN(x) { return _n(float(x)) }
func isInf(x) { return _ii(float(x)) }
func isFinite(x) { local v = float(x); return not (_n(v) or _ii(v)) }
func isClose(a, b, eps = 0.000000001) {
    local x = float(a); local y = float(b)
    if (_n(x) or _n(y)) { return false }
    if (_ii(x) or _ii(y)) { return x == y }
    return _ab(x - y) <= float(eps)
}
func isCloseE(a, b, eps) { return isClose(a, b, eps) }

// ── 表 / 随机 ─────────────────────────────────────────────────────────────
func sum(t)   { local s = 0; local n = len(t); local i = 1; while (i <= n) { s = s + t[i]; i = i + 1 }; return s }
func mean(t)  { local n = len(t); return n == 0 ? _NAN : float(sum(t)) / float(n) }
func minOf(t) { local n = len(t); if (n == 0) { return null }; local m = t[1]; local i = 2; while (i <= n) { if (t[i] < m) { m = t[i] }; i = i + 1 }; return m }
func maxOf(t) { local n = len(t); if (n == 0) { return null }; local m = t[1]; local i = 2; while (i <= n) { if (t[i] > m) { m = t[i] }; i = i + 1 }; return m }
func rand()            { return os.randomFloat() }
func randInt(lo, hi)   { return lo + os.random(hi - lo + 1) }
func randFloat(lo, hi) { return float(lo) + os.randomFloat() * (float(hi) - float(lo)) }
