// geo.m —— 检查 .m 模块能不能用上语言/库的新特性（闭包、路径、事件回调）
//   结论要能跑通：模块里可以 imp 库、建路径、返回闭包、注册事件回调。
imp ui

// 模块里建一条路径并返回它的 id（星形）
func makeStar(w, cx, cy, r) {
    local p = ui.path(w)
    ui.moveTo(p, cx, cy - r)
    ui.lineTo(p, cx + r / 4, cy - r / 4)
    ui.lineTo(p, cx + r, cy - r / 4)
    ui.lineTo(p, cx + r / 3, cy + r / 5)
    ui.lineTo(p, cx + r * 3 / 4, cy + r)
    ui.lineTo(p, cx, cy + r / 2)
    ui.lineTo(p, cx - r * 3 / 4, cy + r)
    ui.lineTo(p, cx - r / 3, cy + r / 5)
    ui.lineTo(p, cx - r, cy - r / 4)
    ui.lineTo(p, cx - r / 4, cy - r / 4)
    ui.closePath(p)
    return p
}

// 纯数据版：直接返回指令数组（模块只做数据加工）
func rectCmds(x, y, w2, h2) {
    return {1, x, y, 2, x + w2, y, 2, x + w2, y + h2, 2, x, y + h2, 6}
}

// 模块导出的闭包工厂（引用捕获）
func counter(start) {
    local n = start
    return func() { n += 1  return n }
}

// 模块里注册事件回调（回调再调外部传入的闭包）
func bindClick(target, sink) {
    ui.on(target, "click", func(e) { sink(target) })
    return true
}