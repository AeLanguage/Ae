// =============================================================================
//  ui —— Ae 的窗口 + 绘制 + 文字库（★ 原生 C++，Win32 + GDI+）
// -----------------------------------------------------------------------------
//  窗口：创建 / 标题 / 大小 / 位置 / 缩放 / 最大化 / 消息循环
//  绘制/文字：点线面和文字都是【对象】，建出来拿 id，之后随时改属性（立刻重画），也能读属性
//
//  用法：
//      imp ui
//      local w = ui.window("Ae 演示", 400, 300)
//      ui.show(w)
//      local box = ui.rect(w, 20, 20, 120, 80)
//      ui.setAll(box, {fill = true, fillColor = "#3B82F6", stroke = 2, rtl = 16, rbr = 16})
//      ui.run(0)                 // 阻塞，直到所有窗口被关闭（点 X 即可）
//
//  函数表（36 个）
//    窗口        window(title,w,h) → id|null   show(id)   hide(id)   close(id)
//                title(id,s)  titleOf(id)  size(id,w,h)  pos(id,x,y)
//                resizable(id,bool)  maximizable(id,bool)  maximize(id)  restore(id)
//                visible(id)  count()  info(id)  pump()  run(id)  lastError()
//    建图形      point(win,x,y)               line(win,x1,y1,x2,y2)
//                poly(win,{x1,y1,x2,y2,...})  rect(win,x,y,w,h)
//                roundRect(win,x,y,w,h,r)     circle(win,cx,cy,r)
//                ellipse(win,cx,cy,rx,ry)     text(win,x,y,"内容") → id
//    改/读属性   set(id,name,value) → id      setAll(id,{...}) → id
//                get(id,name) → value         props(id) → 表
//    其它        remove(id)  clear(win)  shapes(win)  redraw(win)
//                pixel(win,x,y) → 0xRRGGBB|null
//                textSize(id[,内容]) → {w,h}（文字框尺寸；给了第 2 个参数就量那段内容）
//
//  绘制模型
//    · 画出来的东西都是对象：建的时候拿 id，之后 set / setAll 改（立刻重画）、
//      get / props 读。没有"重画回调"，改完属性等下一次 pump() 就会重画。
//    · 窗口和图形【共用一套 id 空间】：把窗口 id 传给图形函数（或反过来）会立刻报错，
//      不会"恰好命中另一个对象"。
//    · 坐标：窗口客户区左上角是 (0,0)，x 向右、y 向下。
//    · 层级 z：小的先画（在下层）；z 相同按创建顺序，后建的在上面。
//    · 抗锯齿：每个图形独立开关 aa（默认 true）。
//    · 描边以路径为中心线（和 SVG 一致）：1px 描边正好压住边界那一圈像素。
//    · 空心/实心 = fill；不描边 = stroke 设 0；全部隐藏 = visible 设 false。
//
//  属性表（set / setAll / get / props 通用）
//    几何  x y w h r rx ry rtl rtr rbr rbl x2 y2 points closed
//          · x/y：point/circle/ellipse 是位置(圆心)、rect 是左上角、poly 是整体偏移
//          · r：point 半径 / circle 半径 / rect 的统一圆角
//          · rtl rtr rbr rbl：四角【各自】的圆角（为 0 就用 r；都为 0 就是直角）
//          · points：poly 的点，扁平数组 {x1,y1,x2,y2,...}（至少 2 个点）
//          · closed：poly 闭合(默认 true)是面；false 是折线（不填充）
//    外观  fill fillColor stroke strokeColor visible aa
//    层级  z
//    只读  kind（"rect"/"text"/"window"…）  win（所属窗口 id）  id
//
//  文字对象（ui.text）自己的属性
//    text    内容（\n 换行；不自动折行）
//    font    字体族名（UTF-8），比如 "Consolas"、"微软雅黑"
//    size    字号，单位是【像素】(em 尺寸)，不是磅
//    bold italic
//    color   文字颜色
//    align   left|center|right   ：x 落在文字框的哪条边（默认 left）
//    valign  top|middle|bottom   ：y 落在文字框的哪条边（默认 top）
//    另有通用的 x y z visible aa（文字【没有】fill/stroke/w/h/points 这些，
//    设了会明确报错而不是静默忽略）
//    props(text) 里额外有：fontUsed（真正用上的族名，字体名不存在时会兜底）、
//                          tw/th（文字框宽高，和 textSize 一致）
//    文字框 = 按内容量出来的尺寸；想让一个点正好是文字中心，就
//    setAll(t, {x=cx, y=cy, align="center", valign="middle"})
//
//  颜色："#RRGGBB" / "#AARRGGBB"（8 位时 alpha 在前）/ "#RGB"，也接受整数
//        （≤0xFFFFFF 当 RGB，否则当 AARRGGBB）。get 一律返回字符串。
//
//  info(id) 返回：id alive visible x y w h winW winH resizable maximizable
//                 minimizable maximized title   （x/y/winW/winH 含边框，w/h 是客户区）
//
//  设计约定
//    · 窗口 id / 图形 id：单调递增、不回收（旧 id 永远无效，不会指到新对象）
//    · 大小 = 客户区尺寸（不含边框/标题栏）；用 AdjustWindowRectEx 换算
//    · 位置 = 窗口左上角（含边框），屏幕坐标
//    · ★ 本库让进程变成 DPI 感知：坐标/尺寸都是【真实像素】。在 125% 缩放的屏幕上，
//      400 像素看起来比"不感知"时小 20%，但不会糊。未感知的进程（比如 PowerShell）
//      去查询本窗口会拿到被系统虚拟化过的坐标，数值对不上是正常的。
//    · show() 用 SW_SHOW，会激活窗口（抢焦点）；测试里建完立刻关，只闪一下。
//    · 预期失败（建不出窗口、尺寸非法）→ 返回 null/false + ui.lastError()
//      程序性错误（id 非法、参数类型错）→ 直接抛运行错误
//    · 渲染用 GDI+，逐图形设抗锯齿。★ 第四批起：脏矩形（只重画变动的区域）+
//      每窗口一张离屏缓冲（DIB），改一个属性只重画那一小块，而且不闪。
//    · pixel() 读的就是那张离屏缓冲（先把待重画的画完再读一个字节），
//      所以它既和屏幕上一致，又便宜（几百次取色是毫秒级）——但别在动画里每帧读整屏
//    · 文字用【灰度抗锯齿】(AntiAliasGridFit)：清晰、不打彩边，而且离屏和屏上完全
//      一致（ui.pixel 的断言才有意义）。要 ClearType 那种彩边锐利感是以后的可选项。
//    · 字体按 (族名,字号,粗,斜) 缓存，不在每帧新建；字体名不存在时兜底到默认族，
//      并可在 props().fontUsed 里查到实际用的是什么。
//    · 还没有的东西：输入框之类的现成控件（自己做：组 + 事件 + 文字就是全部零件）。
//    · 编译本库（★ 链接要多带 -lwinmm，定时器精度要用）：
//        clang++ -std=c++17 -O2 -Wall -shared ui.cpp -o ui.dll \
//                -luser32 -lgdi32 -lgdiplus -lwinmm
//    · ★ 一个反直觉的实测结论（700x470，纯软件 GDI+，本机量的）：
//        ★ 画进【离屏缓冲的 DC】比画进【窗口 DC】快一个数量级 ——
//          同样 40 个渐变圆角矩形 + 40 段文字：直接画到窗口 DC ≈ 54ms，
//          画进 DIB 再 BitBlt ≈ 3ms。早期"GDI+ 很慢"的结论其实是被窗口 DC 拖的。
//        现在的成本（每帧）：
//          整面重画（空窗口，只有底色+贴缓冲） ~0ms
//          整面重画（含贴一张整窗画布）        ~1ms   （以前 13ms）
//          300 个静态图形 + 每帧动 1 个        ~0ms   （只重画 6 个，跳过 297 个）
//          300 个图形每帧全省重画              ~3ms
//          5600 次 ui.pixel                    ~5ms   （以前每次都要重画整个场景）
//    · 消息循环由脚本驱动：pump() 处理一轮不阻塞，run(id) 阻塞到关闭。
//      ★ 第四批起 pump/run 里会把待重画的脏区画进缓冲（不用等 WM_PAINT），
//        所以"改属性 → pump 一下"就够了，WM_PAINT 只负责把缓冲贴到屏幕上。
// =============================================================================
// =============================================================================
//  ★ 第一批新增（路径 / 裁剪 / 光标 / 事件增强）
// -----------------------------------------------------------------------------
//  路径：任意几何（曲线、图标、挖洞）也是对象
//      local p = ui.path(w)                  // 空路径
//      local p = ui.path(w, {1,10,10, 2,90,10, 6})   // 也可以直接给指令数组
//      ui.moveTo(p, x, y)   ui.lineTo(p, x, y)
//      ui.quadTo(p, cx, cy, x, y)                     // 二次贝塞尔
//      ui.curveTo(p, c1x,c1y, c2x,c2y, x, y)          // 三次贝塞尔
//      ui.arc(p, x, y, w, h, start, sweep)            // 弧（外接矩形 + 起止角，单位度）
//      ui.closePath(p)
//      ui.setAll(p, {fill = true, fillColor = "#3B82F6", stroke = 2})   // 和别的图形一样
//    指令流存在 cmds 属性里，可以 get / set / props / 序列化：
//      1 x y                  移动（开新子路径）
//      2 x y                  直线
//      3 cx cy x y            二次贝塞尔
//      4 c1x c1y c2x c2y x y  三次贝塞尔
//      5 x y w h start sweep  弧（度；0° 是三点钟方向、顺时针）
//      6                      闭合
//    整条路径的 x/y 是统一偏移（和 poly 一样，方便整体移动）。
//
//  裁剪：每个图形自带一个裁剪矩形（宽或高 <= 0 = 不裁剪）
//      ui.setAll(item, {clipX = 0, clipY = 0, clipW = 200, clipH = 120})
//    绘制和命中测试都遵守它 —— 于是"滚动视图 / 进度条填充 / 文字溢出省略 /
//    揭幕动画"都能在 Ae 层自己做出来（滚动视图就是"挪 clipX/clipY"）。
//
//  鼠标形状：cursor 属性（图形上设、窗口上设默认都行）
//      ui.set(btn, "cursor", "hand")
//      ui.set(w,   "cursor", "cross")     // 窗口默认
//    可用：arrow hand ibeam wait cross sizeall sizewe sizens sizenwse sizenesw no help up
//
//  事件（ui.on / ui.off / ui.offAll / ui.handlers / ui.send）
//    种类：click dblclick mousedown mouseup mousemove mouseenter mouseleave
//          wheel keydown keyup char resize close
//    回调收到一个表 e：
//      kind win target x y button wheel key keyName shift ctrl alt time w h
//      · key 是虚拟键码，keyName 是可读名（"Enter"/"Left"/"A"…）
//      · shift/ctrl/alt 修饰键；time 是毫秒时间戳（长按/双击间隔/拖动速度自己算）
//    命中规则（重要）：
//      · 事件沿【命中链】从最上层往下走：第一个注册了这种事件的对象【接管】，
//        没注册的对象（装饰、文字标签）自动穿透给下面的 —— 所以"按钮上盖着标签"
//        也能点到按钮
//      · 形状之间互斥；之后仍然冒泡到窗口（"点别处关掉菜单"靠它）
//      · handler 返回 true = 吃掉：连窗口也不再收到
//      · 坐标被裁剪矩形约束：被裁掉的部分点不到
//      · hits = false 的对象完全不参与命中（纯装饰件）
//    click 是【合成】的：同一个键按下→抬起且位移 ≤ 4px 才算一次点击
//    （Windows 只给 down/up，不给 click）；按下时会 SetCapture，拖到窗口外也收得到抬起。
//    ui.send(win, kind, {...}) 走的是和真实消息【完全一样】的派发路径，
//    所以既能做测试，也能拿来做快捷键/演示。
//
//  几何只有一份实现：绘制（FillPath/DrawPath）和命中测试（IsVisible + Widen）
//  共用 buildPath() —— 不会再出现"画出来"和"点得到"不一致。
// =============================================================================
// =============================================================================
//  ★ 第二批新增（画布 / 图片 / 变换 / 文字折行 / 无边框窗口）
// -----------------------------------------------------------------------------
//  画布：离屏渲染表面 —— "虚拟窗口"，同一套 API
//      local cv = ui.canvas(200, 120)
//      local r  = ui.rect(cv, 0, 0, 60, 40)        // 图形建在【画布】上，不是窗口上
//      ui.setAll(r, {fill = true, fillColor = "#FF0000"})
//      ui.drawImage(win, cv, 100, 100)             // 贴到窗口（也可以贴到另一个画布）
//      ui.pixel(cv, 10, 10)                        // 画布内容可以直接取色（测试好写）
//    语义：
//      · 画布默认【全透明】，适合叠加
//      · 它是【缓存】：脏了才重画；改画布里的图形，贴它的窗口会自动跟着重画
//      · 可以套娃（画布 A 贴画布 B，B 再贴窗口）；自环有保护
//      · props(画布) = {kind="canvas", w, h, shapes, dirty}
//      · 用途：复杂图形只画一次（每帧只贴一张图）、双缓冲、把一层 UI 整体缓存
//
//  图片：
//      local ico = ui.image("icon.png")            // 打不开 → null + ui.lastError()
//      ui.drawImage(win, ico, 10, 10)              // 原尺寸
//      ui.drawImage(win, ico, 10, 10, 64, 64)      // 缩放（双三次插值）
//      ui.pixel(ico, 0, 0)                         // 也能直接取图片像素
//      props(图片) = {kind="image", path, w, h}
//
//  变换（每个图形独立）：
//      ui.set(bar, "angle", 90)                    // 旋转（度，顺时针）
//      ui.set(bar, "scale", 2)                     // 缩放
//      ui.set(bar, "anchorX", 20)  ui.set(bar, "anchorY", 20)   // 旋转/缩放中心
//      ui.set(bar, "anchorAuto", true)             // 恢复"自动取包围盒中心"（默认）
//    命中测试按【逆变换】算，所以旋转/缩放之后照样点得准。
//
//  文字（折行是自己算的，不是交给 GDI+ —— 这样"量出来的"和"画出来的"严格一致，
//        文本输入框的光标定位才有依据）：
//      ui.setAll(t, {wrap = 120})                  // 折行宽度（0 = 不折行）
//      ui.setAll(t, {lineGap = 4})                 // 行距
//      ui.setAll(t, {trim = "ellipsis"})           // 放不下就截断加 …
//      local m = ui.measure("一段文字", {size = 16, wrap = 120})
//        → { w, h, lines, lineH, lineW, charW, lineStart }
//          · lineW[i]     第 i 行宽度
//          · charW[i]     第 i 个字符在【自己行内】的 x 偏移  ← 光标位置靠它
//          · lineStart[i] 第 i 行首字符在整串里的下标
//    （不建对象也能量文字；折行优先在空格处断，中文那种没空格的硬断。）
//
//  无边框窗口（自绘标题栏 / 自绘关闭按钮 / 弹出菜单）：
//      ui.set(win, "frameless", true)              // 去掉标题栏
//      ui.on(titleBar, "mousedown", func(e) { ui.beginDrag(win) })   // 拖自绘标题栏
//      ui.minimize(win)   ui.maximize(win)   ui.restore(win)
//    ★ frameless + resizable 时，库会在 WM_NCHITTEST 里自己认边缘（6px），
//      所以无边框窗口照样能拖边改大小。
//    ★ ui.beginDrag 是【库自己实现】的拖动（抓住鼠标跟光标走）：不会进系统的模态
//      消息循环，所以拖动期间动画照跑（以前拖一下就冻住，松手才恢复）。
//      代价：没有系统的 Aero 贴边 / 拖动预览（见 beginWindowDrag 上面的说明）。
// =============================================================================
// =============================================================================
//  ★ 第三批新增（组 / 定时器 / 渐变 / 虚线）
// -----------------------------------------------------------------------------
//  组（容器）：把一堆图形当一个整体来摆 —— 自绘控件的骨架
//      local g   = ui.group(w)                 // 建在窗口上（也可以建在画布或另一个组上）
//      local box = ui.rect(g, 0, 0, 80, 40)    // ★ 子图形的坐标是【相对组】的
//      local lab = ui.text(g, 40, 20, "确定")
//      ui.setAll(g, {x = 100, y = 60})         // 整组平移：子图形跟着走
//      ui.set(g, "angle", 15)                  // 整组旋转（绕组的包围盒中心）
//      ui.set(g, "scale", 1.5)                 // 整组缩放
//      ui.set(g, "visible", false)             // 整组隐藏
//    语义：
//      · 内联（不是新表面）：子图形【直接画进父表面】，所以子图形仍然能单独
//        set/get/命中 —— "按钮 = 组，里面一个矩形 + 一个文字标签"就是这么来的
//      · x/y/angle/scale/clip 是【层层叠加】的：外组的变换作用在内组上，
//        再作用到内组的子图形（嵌套组 = 平移 + 旋转 + 缩放的自然组合）
//      · z：组按自己的 z 整体排序，组内的子图形在组【内部】再按 z 排
//        —— 组和外面图形之间不会互相穿插
//      · 组没有自己的尺寸（w/h 由子图形算出来的包围盒决定，只读）；所以
//        ui.pixel(组, …) 会明确报错，要对窗口/画布取色
//      · ui.remove(组) 连子图形一起删（ui.clear 同理）
//      · props(组) = {id, kind="group", win, x, y, angle, scale, visible, z,
//                     clipX, clipY, clipW, clipH, shapes, hasBounds}
//        （组的 w/h 是子图形的包围盒，不算出来就不给 —— 要尺寸就自己看子图形）
//    事件：点组里的图形，先给那个图形，再【冒泡】给它所在的组（由内向外一层层），
//    最后到窗口。任何一层 return true 就到此为止。组本身没有形状，
//    所以它"被点到"指的就是"它的某个子图形被点到了"。
//
//  定时器（脚本驱动，没有额外线程：pump/run 里到点就调）
//      local t = ui.timer(50, func(e) { ... })        // 每 50ms 一次
//      local t = ui.timerOnce(200, func(e) { ... })   // 200ms 后一次
//      local t = ui.tick(func(e) { ... })             // 每次 pump/run 都调（每帧/动画用）
//      ui.cancelTimer(t)                              // 取消（已经触发过的也安全，返回 bool）
//      prln(ui.timers())                              // 还活着的：id → {ms, once, count, frame}
//      prln(e.kind, e.id, e.count, e.late)            // 回调 e：谁的、第几次、迟到了几毫秒
//    语义：
//      · ui.run(id) 在【有定时器时】改成"轮询消息 + 睡 1ms"，否则死等消息会让定时器永不触发；
//        有定时器活着时库还会把系统计时器精度提到 1ms（timeBeginPeriod）——
//        否则 Sleep(1) 实际睡 15.6ms，动画只能跑到 30fps；定时器全结束就还回去
//        纯 ui.pump() 的循环要自己 os.sleep（否则会把 CPU 跑满）
//      · 时间基准是 GetTickCount64，不累计误差：每次从"上次计划时刻 + 周期"往后找第一个
//        未来时刻 —— 脚本忙了一阵之后不会疯狂补发，只会赶上最近的一次
//      · count 是【这次触发之前】的次数，所以第一次触发时 count = 0
//      · 回调里可以 cancelTimer 自己；一次性定时器触发时自动注销（不用手动取消）
//      · 回调抛错 = 程序中止（和事件回调一致，见 aec 的错误模型）
//
//  渐变（fillColor → fillColor2）
//      ui.setAll(panel, {fill = true, fillColor = "#3B82F6", fillColor2 = "#8B5CF6",
//                        gradient = "vertical"})    // 等价于 linear + gradientAngle = 90
//      ui.setAll(dot,   {fill = true, fillColor = "#FFFFFF", fillColor2 = "#3B82F6",
//                        gradient = "radial"})      // 径向（从中心往外）
//      ui.set(t, "gradientAngle", 30)               // 线性渐变给任意角度（度，顺时针）
//    gradient 取值：""（关闭）/ linear / radial / horizontal / vertical / diagonal；
//    给方向别名时库会折算成 linear + 对应的 gradientAngle（get 回来是规范形）。
//    语义：只有 fill = true 时才有意义；没设 fillColor2 时看不出渐变（两端同色）。
//
//  虚线 / 端点 / 拐角（描边风格，SVG 那套）
//      ui.setAll(grid, {stroke = 1, strokeColor = "#333333", dash = "6 3"})
//      ui.setAll(free, {stroke = 3, cap = "round", join = "round"})
//    dash：一串空格分开的数，奇偶交替 = 画多长 / 空多长（"6 3" 或 "6 3 2 3"）
//    cap ：flat（默认，平头）| round（圆头）| square（方头，比平头多出一半线宽）
//    join：round（默认，圆角）| miter（尖角）| bevel（切角）
//    ★ 这几项只影响【绘制】：命中测试仍按完整笔画算 —— 虚线的空隙也点得到，
//      否则"虚线按钮"会变得几乎点不中。
//    ★ 值域很小的字符串属性（gradient / cap / join / trim / cursor）写错了会
//      【明确报错】并列出可用值，不会静默当成默认值。
// =============================================================================
// =============================================================================
//  ★ 第四批新增（脏矩形 / 离屏缓冲 / 不再闪）
// -----------------------------------------------------------------------------
//  以前：改一个属性 → 整个窗口重画，而且是【直接画在屏幕 DC 上】。于是
//    · 图形一多、窗口一大就慢（40 个渐变圆角矩形 + 40 段文字 ≈ 54ms/帧）
//    · 每次改动整窗重画一遍 → 画面闪，看起来像"窗口重启了一次"
//  现在分两层：
//
//  ① 每个表面（窗口 / 画布）有一张【离屏缓冲】+ 一张【待重画矩形】清单
//      · 改图形 → 算它"旧位置 ∪ 新位置"的矩形（含自己的旋转缩放、祖先组的变换、
//        自己与祖先的裁剪，再往外放 2px 吃抗锯齿的边）→ 加进清单
//      · 重画时只画这些矩形：矩形里先刷背景，再按 z 序重画【与矩形相交】的图形，
//        不相交的子树整棵跳过 —— 矩形之外的像素一个都不碰
//      · 矩形内"整层重画"保证了正确性：被上层图形盖住的像素也能恢复
//      · 窗口画进 DIB 缓冲再 BitBlt 到屏幕 → 不闪（画布本来就是离屏位图）
//      · 什么时候退回整面重画：新建缓冲 / 尺寸变了 / 清单超过 12 块 /
//        面积超过 60% / 清空 / 显式 ui.redraw —— 宁可多画，绝不画错
//
//  ② 画布内容变了 → 这块矩形按贴图缩放【映射】到所有贴它的图形上，一层层传到
//     窗口（画布套画布也算）。所以"只动画布里一个小圆"在窗口上只重画 13x13。
//
//  什么时候真的重画：pump() / run() / ui.pixel() / ui.redraw() / 系统发 WM_PAINT。
//  ui.pixel 现在读的就是那张缓冲（先把待重画的画完再读一个字节），所以
//  "取色"从"每次重画整个场景"变成"读一个字节" —— 大量取色的测试快了几十倍。
//
//  调试接口 ui.damage(窗口|画布)：
//      paints  累计重画几次        rects  上次重画几个矩形
//      drawn   上次真正画了几个图形  skipped 上次跳过了几个（脏区外面，一帧只算一次）
//      full    上次是不是整面重画    x y w h  上次重画区域的包围盒
//      pending / pendingFull / px py pw ph   还没画的部分
//  典型用法：改一个图形之后 ui.damage(w).drawn 应该只有个位数、full = false。
//
//  ★ 顺带修掉一个藏了很久的 bug：drawShape 里"绕锚点旋转/缩放"以前【没有还原
//    变换】—— 一个转过的图形会把旋转缩放留给它后面画的所有图形。以前每帧都新建
//    Graphics、而且转过的图形往往恰好排在最后，所以看不出来；局部重画把绘制顺序
//    打乱之后它立刻现形（表现为"某个角落里多了块颜色"）。
//
//  另外这批修掉的两个"手感"问题（都是用户报的）：
//  ① 鼠标在窗口里移动时动画掉到 30fps
//     原因：每来一条 WM_MOUSEMOVE，命中测试都会把【每一个图形】真的建一次路径 +
//     Widen 判一遍 —— 800 个图形上一条 mousemove 要 31ms。
//     修法：先用【缓存的包围盒】粗筛（collectHits 里用 itemParentBounds，
//     坐标是"父坐标系"，和递归下去的那个点同一个坐标系），沾不到点的直接跳过；
//     另外命中测试共用一个 1x1 的 Graphics，不再每次新建 Bitmap。
//     实测：800 图形下一条 mousemove 31ms → 0ms，动画稳定在 600fps 上下。
//  ② 拖窗口时动画全停，松手才恢复
//     原因：拖标题栏时 DefWindowProc 会跑一个自己的模态消息循环，把脚本的 pump/run
//     卡在里面；实测那段时间【什么都不派发】—— WM_TIMER 消息、SetTimer 回调、
//     PostMessage 的自定义消息统统不来（只有鼠标消息能进去）。
//     修法：标题栏拖动由库自己做（WM_NCLBUTTONDOWN + HTCAPTION → beginWindowDrag，
//     跟光标 SetWindowPos，松手结束），根本不进模态循环；
//     另外给"拖边框改大小"这条仍然走系统模态循环的路径留了两道保险
//     （WM_MOVING/WM_SIZING 里派发 + 辅助线程每 10ms 发一条 WM_MOUSEMOVE 当心跳）。
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef UNICODE
  #define UNICODE          // 让 IDC_ARROW 之类的宏走 W 版
#endif
#ifndef _UNICODE
  #define _UNICODE
#endif

#include "ae_libutil.h"

#include <windows.h>
#include <mmsystem.h>        // timeBeginPeriod（定时器精度；链接要带 -lwinmm）
#include <objidl.h>          // gdiplus.h 需要 IStream（WIN32_LEAN_AND_MEAN 把它挡掉了）
#include <propidl.h>         // 以及 PROPID
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// =============================================================================
//  对象表：窗口和图形【共用一套 id 空间】
//    · id = 下标 + 1，单调递增、不回收 → 旧 id 永远无效，不会静默指到新对象
//    · 共用一个空间的好处：把窗口 id 传给图形函数（或反过来）能立刻报错，
//      而不是"恰好命中另一个对象"这种最难查的错
// =============================================================================
// ★ 第二批：画布（离屏渲染表面）和图片也是对象
//    OBJ_WINDOW  真窗口（hwnd）
//    OBJ_CANVAS  离屏画布（内存位图）：图形可以建在它上面，行为跟窗口一样，
//                再用 ui.drawImage(win, 画布, x, y) 贴到别处 —— 复杂图形只画一次
//    OBJ_IMAGE   一张图片（从文件加载）
//    OBJ_SHAPE   图形
//    OBJ_GROUP   ★ 第三批：分组/容器（内联）—— 子图形直接画进父表面，
//                但组的 x/y/angle/scale/clip/visible/z 作用于整组；可嵌套；
//                子图形仍然能单独命中、单独改属性（自制控件的基础）
enum ObjKind { OBJ_WINDOW = 0, OBJ_SHAPE = 1, OBJ_CANVAS = 2, OBJ_IMAGE = 3, OBJ_GROUP = 4 };
enum ShapeKind { SK_POINT = 0, SK_LINE, SK_POLY, SK_RECT, SK_CIRCLE, SK_ELLIPSE, SK_TEXT, SK_PATH, SK_IMAGE, SK_GROUP };

typedef uint32_t Argb;

struct Shape {
    ShapeKind kind = SK_RECT;
    int64_t   winId = 0;      // 属于哪个表面（窗口 or 画布）
    int64_t   srcId = 0;      // ★ SK_IMAGE：贴的是哪个对象（画布或图片）
    int64_t   seq   = 0;      // 创建序号：z 相同时按它排（后建的在上）
    // ── 几何 ──
    double x = 0, y = 0;      // point/circle/ellipse 位置（圆心）；rect 左上角；poly 整体偏移
    double w = 0, h = 0;      // rect 尺寸
    double r = 0;             // point 半径 / circle 半径 / rect 统一圆角
    double rx = 0, ry = 0;    // ellipse 半轴
    double rtl = 0, rtr = 0, rbr = 0, rbl = 0;   // rect 四角各自的圆角（独立设置）
    double x2 = 0, y2 = 0;    // line 终点
    std::vector<double> pts;  // poly：扁平 {x1,y1,x2,y2,...}
    // path：扁平指令流（见文件头"路径指令"说明）
    std::vector<double> cmds;
    bool   closed = true;     // poly 闭合 = 面；不闭合 = 折线
    // ── 外观 ──
    bool   fill    = false;   // 实心 / 空心
    bool   visible = true;
    bool   hits    = true;    // ★ 是否参与命中测试（false = 鼠标穿透，纯装饰用）
    bool   aa      = true;    // 抗锯齿
    double stroke  = 1;       // 线宽，0 = 不描边
    Argb   fillColor   = 0xFF000000;
    Argb   strokeColor = 0xFF000000;
    double z = 0;             // 层级：小的先画（在下层）
    // ── 渐变 / 虚线 / 线帽线接（第三批）──
    Argb   fillColor2 = 0xFF000000;              // 渐变另一端颜色
    std::string gradient;                        // "" / "linear" / "radial"
    double gradientAngle = 0;                    // 线性渐变方向（度）
    std::string dash;                            // 虚线样式，如 "6 3"（空 = 实线）
    std::string cap  = "flat";                   // 线帽：flat / round / square
    std::string join = "round";                  // 线接：miter / round / bevel
    // ── 变换（第二批）：绕锚点旋转 + 缩放 ──
    double angle = 0;         // 旋转角度（度，顺时针，和 GDI+ 一致）
    double scale = 1;         // 缩放倍数（对路径/多边形尤其有用）
    double anchorX = 0, anchorY = 0;
    bool   hasAnchor = false; // false = 自动用图形包围盒中心

    // ── 裁剪：宽或高 <= 0 表示不裁剪（默认）──
    double clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    // ── 鼠标形状（空串 = 沿用窗口默认；见 cursor 属性）──
    std::string cursor;
    // ── 文字（SK_TEXT）──
    std::string text;         // 内容，用 \n 换行
    std::string font;         // 字体族名（UTF-8），不存在时按默认字体画
    double size  = 16;        // 字号，单位是【像素】(em 尺寸)
    double wrap  = 0;         // ★ 折行宽度（0 = 不折行；>0 时按它折行）
    double lineGap = 0;       // ★ 行距（额外像素）
    std::string trim = "none";// ★ 放不下怎么办：none / ellipsis（截断加 …）
    bool   bold   = false;
    bool   italic = false;
    Argb   color  = 0xFF000000;
    int    align  = 0;        // 0 左 1 居中 2 右：x 落在文字框的哪条边上
    int    valign = 0;        // 0 上 1 中 2 下：y 落在文字框的哪条边上
};

// ★ 事件：一个回调登记项。handle 是宿主给的闭包句柄（func_retain），
//   移除时必须 func_release，否则闭包永远不会被回收。
struct Handler {
    std::string kind;
    int         handle = 0;
};

struct Obj {
    ObjKind kind  = OBJ_WINDOW;
    bool    alive = true;
    HWND    hwnd  = nullptr;             // 窗口
    std::vector<int64_t> shapes;         // 窗口/画布/组：拥有的图形 id（创建顺序）
    int64_t parent = 0;                  // ★ 组属于哪个表面（窗口/画布/另一个组）
    Shape   sh;                          // 图形
    std::vector<Handler> handlers;       // ★ 事件回调（按注册顺序）
    int64_t hover = 0;                   // ★ 鼠标当前悬停在哪个对象上（0 = 没有）
    // ── 画布（OBJ_CANVAS）──
    int cw = 0, ch = 0;                              // 画布尺寸
    Gdiplus::Bitmap* bmp = nullptr;                   // 渲染目标
    bool dirty = true;                                // 需要重画
    bool rendering = false;                           // 防自环（画布里贴自己的图）
    std::vector<int64_t> users;                       // 谁把它贴出去了（它脏了要连累这些窗口重画）
    // ── 图片（OBJ_IMAGE）──
    Gdiplus::Bitmap* img = nullptr;
    std::string imgPath;
    // ★ click 合成用：哪个键在某处按下（1 左 2 右 3 中）
    bool downActive[4] = { false, false, false, false };
    int  downX[4] = { 0, 0, 0, 0 };
    int  downY[4] = { 0, 0, 0, 0 };
    bool    pendingClose = false;        // ★ 收到 WM_CLOSE 且有待派发的 close 回调
    bool frameless = false;              // ★ 无边框窗口（自绘标题栏用）
    bool resizable = true;               // 缓存一下，WM_NCHITTEST 要用
    std::string cursorName;              // ★ 窗口默认鼠标形状（空 = 系统箭头）
    std::string curCursor;               // ★ 当前已应用的形状（WM_SETCURSOR 用）

    // ── ★ 第四批：脏矩形（局部重画）+ 离屏缓冲 ────────────────────────────
    //   表面（窗口/画布）用 damage 记"待重画的矩形"；图形/组用 wb 记"上次画出来
    //   占的地方"，两者合起来就知道"改这一个东西要重画哪块"。
    std::vector<Gdiplus::RectF> damage;   // 待重画的矩形（表面坐标）
    bool  fullDamage = true;              // 整面重画（新建/改尺寸/块数太多/清空）
    Gdiplus::RectF wb;                    // 上次画出来占的地方（所在表面坐标）
    Gdiplus::RectF pb;                    // 同上但只到【父坐标系】（命中粗筛要用）
    bool  wbOk = false;                   // wb 是否还有效（无效 = 要重算）
    double wbX = 0, wbY = 0, wbA = 0, wbS = 1;   // 存 wb 时自己的变换（判断"只是平移"）
    int64_t dmgStamp = 0;                 // 统计"跳过"用：上次算进哪一次重画
    // 窗口的离屏缓冲（DIB）：先画进缓冲再 BitBlt → 改一个属性也不会闪
    HDC     memDC   = nullptr;
    HBITMAP dib     = nullptr;
    HGDIOBJ dibOld  = nullptr;
    void*   dibBits = nullptr;            // 缓冲像素（自上而下：内存第一行 = 屏幕第一行）
    int     bufW = 0, bufH = 0;
    // ui.damage() 的统计（测试的抓手：证明"只重画了一小块"）
    int64_t paints = 0;                   // 累计重画次数
    int64_t lastDrawn = 0;                // 上次真正画了几个图形
    int64_t lastSkipped = 0;              // 上次跳过了几个图形
    int64_t lastRects = 0;                // 上次重画了几个矩形
    bool    lastFull = true;              // 上次是不是整面重画
    Gdiplus::RectF lastRegion;            // 上次重画区域的包围盒
};

// ★ 事件队列：WndProc 里只【入队】，不打回调。真正的回调在 ui.pump() 里派发 ——
//   这样用户代码不会跑在窗口过程里（在 WndProc 里 DestroyWindow / 改控件都容易出事）。
struct Ev {
    std::string kind;
    int64_t winId = 0;
    int  x = 0, y = 0;        // 客户区坐标
    int  button = 0;          // 1 左 2 右 3 中
    int  delta  = 0;          // 滚轮
    int  key    = 0;          // 虚拟键码
    int  w = 0, h = 0;        // resize
    bool hasPos = false;      // 是否带坐标（鼠标类事件）
    // ★ 修饰键与时间戳（多选/框选/快捷键组合、长按、拖动速度都要用）
    bool shift = false, ctrl = false, alt = false;
    int64_t time = 0;         // 事件发生时刻（毫秒，程序启动起算）
};
ULONGLONG g_startTick = GetTickCount64();      // 时间戳基准
std::vector<Ev> g_events;

// ── ★ 第三批：定时器（脚本驱动）──
//   库里自己记时间，在 pump() / run() 里把"到点了"的回调派发出去 ——
//   和事件走同一套派发路径，不用线程、不打扰消息循环。
//   ms = 0 表示"每帧"（ui.tick）。落后了就跳到下一个未来时刻，不会补发一大堆。
struct Timer {
    int64_t id = 0;
    int64_t ms = 0;          // 周期（0 = 每帧）
    int64_t nextAt = 0;      // 下次到点时刻（毫秒，自加载起算）
    bool    once = false;
    bool    alive = true;
    int     handle = 0;      // 闭包句柄
    int64_t count = 0;       // 已经触发几次
};
std::vector<Timer> g_timers;
int64_t g_timerSeq = 0;
AeVM*           g_vm = nullptr;      // 最近一次原生调用的 VM（派发回调要用）

std::vector<Obj> g_objs;
int64_t          g_seq       = 0;
bool             g_classReady = false;
bool             g_dpiDone    = false;
bool             g_gdiReady   = false;

const wchar_t* kClass = L"AeUiWindow";


Obj* objAt(int64_t id) {
    if (id < 1 || (size_t)id > g_objs.size()) return nullptr;
    Obj& o = g_objs[(size_t)id - 1];
    return o.alive ? &o : nullptr;
}
inline int64_t idOfObj(const Obj* o) { return (int64_t)(o - g_objs.data()) + 1; }
Obj* objByHwnd(HWND h) {
    for (auto& o : g_objs) if (o.alive && o.kind == OBJ_WINDOW && o.hwnd == h) return &o;
    return nullptr;
}

inline int aliveCount() {
    int n = 0;
    for (auto& o : g_objs) if (o.alive && o.kind == OBJ_WINDOW) n++;
    return n;
}

// ── 取对象：不存在/已删 → 抛；类型不对 → 抛（并说清是哪种 id）───────────────
//   ★ 窗口的报错文本被回归用例锁定，不要改措辞
Obj* objOf(AeVM* vm, int64_t id, const char* fn) {
    Obj* o = objAt(id);
    if (!o)
        aelib::raise(vm, std::string(fn) + "：id 非法（" + std::to_string(id) +
                             "）—— 可能已经关闭/删除，或从来不存在");
    return o;
}
// ★ 表面 = 窗口 或 画布。图形可以建在两者任何一个上，用同一套 API。
Obj* surfOf(AeVM* vm, int64_t id, const char* fn) {
    Obj* o = objAt(id);
    if (!o) {
        aelib::raise(vm, std::string(fn) + "：id 非法（" + std::to_string(id) +
                             "）—— 可能已经关闭/删除，或从来不存在");
        return nullptr;
    }
    if (o->kind != OBJ_WINDOW && o->kind != OBJ_CANVAS && o->kind != OBJ_GROUP) {
        aelib::raise(vm, std::string(fn) + "：需要窗口、画布或组 id（当前是 " +
                             (o->kind == OBJ_SHAPE ? "图形" : "图片") + "）");
        return nullptr;
    }
    return o;
}
// 一直往上找，直到窗口/画布（组要连累的是它所在的表面）
Obj* rootSurface(Obj* o) {
    int guard = 0;
    while (o && guard++ < 64) {
        if (o->kind == OBJ_WINDOW || o->kind == OBJ_CANVAS) return o;
        if (o->kind == OBJ_GROUP) { o = objAt(o->parent); continue; }
        o = objAt(o->sh.winId);
    }
    return nullptr;
}
void surfSize(Obj& o, int* w, int* h) {
    if (o.kind == OBJ_CANVAS) { *w = o.cw; *h = o.ch; return; }
    RECT cr;
    if (o.hwnd) { GetClientRect(o.hwnd, &cr); *w = (int)(cr.right - cr.left); *h = (int)(cr.bottom - cr.top); }
    else { *w = 0; *h = 0; }
}

Obj* winOf(AeVM* vm, int64_t id, const char* fn) {
    Obj* o = objAt(id);
    if (!o) {
        aelib::raise(vm, std::string(fn) + "：窗口 id 非法（" + std::to_string(id) +
                             "）—— 可能已经关闭，或从来不存在");
        return nullptr;
    }
    if (o->kind != OBJ_WINDOW) {
        aelib::raise(vm, std::string(fn) + "：窗口 id 非法（" + std::to_string(id) +
                             "）—— 这是个图形 id，窗口函数要传窗口 id");
        return nullptr;
    }
    return o;
}
Obj* shapeOf(AeVM* vm, int64_t id, const char* fn) {
    Obj* o = objAt(id);
    if (!o) {
        aelib::raise(vm, std::string(fn) + "：图形 id 非法（" + std::to_string(id) +
                             "）—— 可能已经删除，或从来不存在");
        return nullptr;
    }
    if (o->kind != OBJ_SHAPE) {
        aelib::raise(vm, std::string(fn) + "：图形 id 非法（" + std::to_string(id) +
                             "）—— 这是个窗口 id，图形函数要传绘制出来的那个 id");
        return nullptr;
    }
    return o;
}

// =============================================================================
//  ★ 第四批：脏矩形（局部重画）+ 每窗口一张离屏缓冲
// -----------------------------------------------------------------------------
//  以前"改一个属性 → 整个窗口重画"，图形一多、窗口一大就又慢又闪。现在的模型：
//    · 每个表面（窗口 / 画布）各有一张【待重画矩形】清单 damage
//    · 改图形 → 算它"旧位置 ∪ 新位置"（含自己旋转缩放 + 祖先组的变换 + 祖先裁剪，
//      再往外放 2px 吃抗锯齿的边）→ 加进所在表面的清单
//    · 重画时只画这些矩形：矩形里先刷背景，再按 z 序重画【与矩形相交】的图形
//      （不相交的子树整棵跳过）—— 矩形之外的屏幕像素原封不动
//      矩形内"整层重画"保证了正确性：被上层图形盖住的像素也能恢复
//    · 窗口有一张 DIB 缓冲：画进缓冲再 BitBlt → 不闪；ui.pixel 也直接读缓冲，
//      不用每次重画整个场景（几百个取色从"每次几毫秒"变成"读一个字节"）
//    · 画布同理，而且画布内容变了要把这块矩形【映射】到所有贴它的图形上，
//      一层层传到窗口（画布套画布也算）
//  什么时候退回整面重画：新建缓冲 / 尺寸变了 / 清单块数太多 / 面积超过一半 /
//  清空 / 显式 ui.redraw —— 反正都比"每帧整窗重画"强，而且绝不会漏画（宁可多画）
// =============================================================================
struct PaintStats { int64_t drawn = 0, skipped = 0, gen = 0; };
Argb fromColorRef(COLORREF c);              // 定义在后面（刷窗口底色要用）

// 整面重画（窗口或画布；传图形/组也行，会自动找它所在的表面）
void damageAll(Obj* surfOrItem);
// 按"旧位置 ∪ 新位置"精确重画某个图形/组（组平移时子孙缓存能精确跟着挪）
void damageItem(Obj* item);
// 把"某个对象自己坐标系里的矩形"映射到它所在表面，算进重画清单
void damageLocalRect(Obj* item, const Gdiplus::RectF& local);
// 直接给一个表面加伤害区（表面坐标）
void damageSurfaceRect(Obj* surf, const Gdiplus::RectF& r);
// 表面尺寸（窗口 = 客户区，画布 = 画布尺寸）
void surfSize(Obj& o, int* w, int* h);
// 窗口离屏缓冲（DIB）：没有/尺寸不对就建一张；失败返回 false
bool ensureWindowBuffer(Obj& win);
void releaseWindowBuffer(Obj& win);
// 把待重画的画进缓冲（窗口 → DIB；画布 → 位图）。返回是否有东西要画
bool flushSurface(Obj& surf);
// 把窗口缓冲 BitBlt 到屏幕（区域用这次的更新区，包围盒即可）
void blitWindowBuffer(Obj& win, HDC hdc);
// 从缓冲里读一个像素（先把待重画的画完，保证读到的是最新画面）
bool readSurfacePixel(Obj& surf, int x, int y, Argb* out);

// 让一个表面【整面】重画（以前的做法，留给"变化范围说不清"的场合）
inline void invalidate(Obj* win) {
    if (win && win->hwnd) InvalidateRect(win->hwnd, nullptr, FALSE);
}
// 画布变脏 → 所有贴了它的窗口也要重画
void invalidateSurface(Obj* s) {
    damageAll(s);
}
inline void invalidateOfShape(Obj* sh) {
    if (sh) damageItem(sh);
}

// 渲染/事件（定义在后面，这里先声明给 wndProc 用）
void paintWindow(Obj& win, HDC hdc);
void flushEvents(AeVM* vm);
void ensureCanvasRendered(Obj& cv);      // ★ 画布按需渲染（drawShape 贴图时要用）
void flushTimers(AeVM* vm);              // ★ 定时器派发
Timer* findTimer(int64_t id);
void killObj(AeVM* vm, Obj& o);          // ★ 递归清理对象（组带走子图形）

// ★ 拖动/改大小期间的心跳线程（定义在后面；WM_ENTERSIZEMOVE / WM_DESTROY 要用）
void startModalBeat(HWND h);
void stopModalBeat();

// ★ 自己实现的"拖动窗口"（不经过系统的模态循环，动画才不会停）
void beginWindowDrag(HWND h);
void beginWindowDragAt(HWND h, int sx, int sy);
void updateWindowDragAt(HWND h, int cx, int cy);
void endWindowDrag();
bool windowDragActive();

// ★ 把待重画的脏区画进缓冲（定义在绘制那段；pump/run/拖动期间的 WM_TIMER 都要用）
void flushAllPending() {
    for (Obj& o : g_objs) {                                   // 先画布（窗口贴图要用）
        if (o.alive && o.kind == OBJ_CANVAS && (o.fullDamage || !o.damage.empty()))
            flushSurface(o);
    }
    for (Obj& o : g_objs) {
        if (!o.alive || o.kind != OBJ_WINDOW) continue;
        if (o.fullDamage || !o.damage.empty()) flushSurface(o);
    }
}

bool hasHandler(Obj& o, const char* kind) {
    for (const Handler& h : o.handlers) if (h.kind == kind) return true;
    return false;
}
// 修饰键 + 时间戳（真实消息用 GetKeyState 现场取）
void fillMods(Ev& e) {
    e.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    e.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    e.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    e.time  = (int64_t)(GetTickCount64() - g_startTick);
}
void queueMouse(HWND h, const char* kind, int x, int y, int button) {
    Obj* o = objByHwnd(h);
    if (!o) return;
    Ev e;
    e.kind = kind; e.winId = idOfObj(o);
    e.x = x; e.y = y; e.button = button; e.hasPos = true;
    fillMods(e);
    g_events.push_back(e);
}
void queueKey(HWND h, const char* kind, int key) {
    Obj* o = objByHwnd(h);
    if (!o) return;
    Ev e; e.kind = kind; e.winId = idOfObj(o); e.key = key;
    fillMods(e);
    g_events.push_back(e);
}
// ★ 字符串属性的值域都很小，写错了【明确报错】而不是静默当成默认值
//   （cursor 的名字和 loadCursorByName 里的 IDC_* 是同一份清单，改一处要改两处）
const char* kCursorNames = "arrow hand ibeam wait cross sizeall sizewe sizens sizenwse sizenesw no help up";
bool validCursorName(const std::string& n) {
    static const char* kNames[] = { "arrow", "hand", "ibeam", "wait", "cross", "sizeall",
                                    "sizewe", "sizens", "sizenwse", "sizenesw", "no", "help", "up" };
    if (n.empty()) return true;                     // 空 = 沿用默认（窗口默认 / 系统箭头）
    for (const char* k : kNames) if (n == k) return true;
    return false;
}
HCURSOR loadCursorByName(const std::string& n) {
    LPCWSTR id = nullptr;
    if      (n == "arrow")  id = IDC_ARROW;
    else if (n == "hand")   id = IDC_HAND;
    else if (n == "ibeam")  id = IDC_IBEAM;
    else if (n == "wait")   id = IDC_WAIT;
    else if (n == "cross")  id = IDC_CROSS;
    else if (n == "sizeall")id = IDC_SIZEALL;
    else if (n == "sizewe") id = IDC_SIZEWE;
    else if (n == "sizens") id = IDC_SIZENS;
    else if (n == "sizenwse") id = IDC_SIZENWSE;
    else if (n == "sizenesw") id = IDC_SIZENESW;
    else if (n == "no")     id = IDC_NO;
    else if (n == "help")   id = IDC_HELP;
    else if (n == "up")     id = IDC_UPARROW;
    if (!id) return nullptr;
    return LoadCursorW(nullptr, id);
}
void applyCursor(Obj& win) {
    HCURSOR hc = loadCursorByName(win.curCursor);
    if (hc) SetCursor(hc);
}
// ★ 拖动/改大小期间的"心跳"（见 WM_ENTERSIZEMOVE 的说明）
//
//   实测（本机 Windows，拖标题栏 2 秒）模态循环里【只有鼠标消息会到我们手里】：
//     · WM_TIMER 消息 ✗   · SetTimer 的计时器回调 ✗   · PostMessage 的自定义消息 ✗
//     · WM_MOVING / WM_SIZING ✓（每次鼠标移动一条，但鼠标不动就没有）
//   也就是说脚本的 pump/run 循环被卡住期间，库自己【没有任何时钟】。
//   解法：拖动一开始就开一个辅助线程，每 10ms 往窗口 PostMessage 一条
//   WM_MOUSEMOVE（坐标取【当前光标位置】，所以窗口不会跳、跟手感和系统一样），
//   模态循环一定把它捞出来派发 → 主线程在 WndProc 里就能继续
//   派发定时器 + 重画 → 动画不停。松手（WM_EXITSIZEMOVE）就停掉这个线程。
//   ★ 用户回调始终跑在【主线程】上（VM 不是线程安全的），辅助线程只发消息。
HANDLE  g_beatThread = nullptr;
volatile LONG g_beatStop = 0;
HWND    g_beatHwnd = nullptr;
bool    g_inModalDrag = false;           // 正在拖动/改大小的模态循环里

DWORD WINAPI modalBeatProc(LPVOID) {
    while (!g_beatStop) {
        HWND h = (HWND)g_beatHwnd;
        if (!h) break;
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(h, &pt))
            PostMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM((short)pt.x, (short)pt.y));
        Sleep(10);
    }
    return 0;
}
void startModalBeat(HWND h) {
    stopModalBeat();
    g_beatHwnd = h;
    g_beatStop = 0;
    g_beatThread = CreateThread(nullptr, 0, modalBeatProc, nullptr, 0, nullptr);
}
void stopModalBeat() {
    if (g_beatThread) {
        g_beatStop = 1;
        WaitForSingleObject(g_beatThread, 200);
        CloseHandle(g_beatThread);
        g_beatThread = nullptr;
    }
    g_beatHwnd = nullptr;
}

// =============================================================================
//  ★ 拖动窗口 —— 自己实现，不走系统的模态循环
// -----------------------------------------------------------------------------
//  问题：拖标题栏（或 DefWindowProc 处理 SC_MOVE）时，Windows 会跑一个【自己的
//  模态消息循环】，直到松手才回来。这期间脚本的 pump/run 被卡在里面，定时器没人
//  派发、脏矩形没人画 —— 动画就"冻住"了，松手才恢复（实测：整段拖动期间脚本
//  一帧都跑不了；连 WM_TIMER 消息、SetTimer 回调、PostMessage 的自定义消息
//  统统不会派发，只有鼠标消息能进去）。
//  办法：这个拖动我们自己来做 —— 抓住鼠标 + 跟着光标 SetWindowPos + 松手结束。
//  于是消息循环一直是我们的，动画照跑。
//  ★ 代价（要说清楚）：没有系统的 Aero 贴边（拖到屏幕边缘平铺/最大化）和拖动时的
//    半透明预览；窗口移动由我们的消息循环驱动，脚本要是一直不 pump 就会跟不上。
//    双击标题栏最大化仍然有效（那是 WM_NCLBUTTONDBLCLK，我们没拦）。
// =============================================================================
struct DragWinState {
    bool    active = false;
    HWND    hwnd   = nullptr;
    int     dx = 0, dy = 0;         // 光标 到 窗口左上角 的偏移
    int64_t winId  = 0;
};
DragWinState g_dragWin;

bool windowDragActive() { return g_dragWin.active; }

// sx/sy = 屏幕坐标（非客户区消息的 lParam 就是屏幕坐标；自己调就从 WM_NCHITTEST 那套拿）
void beginWindowDragAt(HWND h, int sx, int sy) {
    if (!h) return;
    Obj* o = objByHwnd(h);
    RECT wr;
    if (!GetWindowRect(h, &wr)) return;
    // ★ 我们没把非客户区点击交给 DefWindowProc，所以"点标题栏会激活窗口"这件事
    //   得自己做一下（不然点不活动的窗口不会到前台）
    if (GetForegroundWindow() != h) SetForegroundWindow(h);
    // 最大化时先还原：还原后光标按【原来的横向比例】落在标题栏上（和系统行为接近）
    if (IsZoomed(h)) {
        double frac = wr.right > wr.left ? (double)(sx - wr.left) / (double)(wr.right - wr.left) : 0.5;
        ShowWindow(h, SW_RESTORE);
        if (!GetWindowRect(h, &wr)) return;
        int nx = sx - (int)(frac * (double)(wr.right - wr.left));
        SetWindowPos(h, nullptr, nx, sy - 12, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (o) { releaseWindowBuffer(*o); o->fullDamage = true; o->damage.clear(); }
        if (!GetWindowRect(h, &wr)) return;
    }
    g_dragWin.active = true;
    g_dragWin.hwnd = h;
    g_dragWin.winId = o ? idOfObj(o) : 0;
    g_dragWin.dx = sx - wr.left;
    g_dragWin.dy = sy - wr.top;
    SetCapture(h);
}
// 从脚本里调（mousedown 回调）：光标按【当前真实位置】算偏移
void beginWindowDrag(HWND h) {
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    beginWindowDragAt(h, pt.x, pt.y);
}
// cx/cy = 客户区坐标（WM_MOUSEMOVE 的 lParam）
void updateWindowDragAt(HWND h, int cx, int cy) {
    if (!g_dragWin.active || g_dragWin.hwnd != h) return;
    POINT pt;
    pt.x = cx;
    pt.y = cy;
    if (!ClientToScreen(h, &pt)) return;
    SetWindowPos(h, nullptr, pt.x - g_dragWin.dx, pt.y - g_dragWin.dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void endWindowDrag() {
    if (!g_dragWin.active) return;
    HWND h = g_dragWin.hwnd;
    g_dragWin.active = false;
    g_dragWin.hwnd = nullptr;
    if (h && GetCapture() == h) ReleaseCapture();
    // 位置变了 → 整面重画一次（缓冲没法平移，重画最省心）
    Obj* o = g_dragWin.winId ? objAt(g_dragWin.winId) : nullptr;
    if (o && o->alive) { o->fullDamage = true; o->damage.clear(); }
    g_dragWin.winId = 0;
}
void releaseHandlers(AeVM* vm, Obj& o) {
    for (Handler& h : o.handlers) ae_release_func(vm, h.handle);
    o.handlers.clear();
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: {                    // 点 X / Alt+F4
        Obj* o = objByHwnd(h);
        if (o && hasHandler(*o, "close")) {
            // ★ 有脚本回调：先入队 close 事件，等 pump 派发完再决定是否真的关
            Ev e; e.kind = "close"; e.winId = idOfObj(o);
            g_events.push_back(e);
            o->pendingClose = true;
            return 0;
        }
        DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY: {                  // 窗口没了 → 它和它的图形一起失效
        g_inModalDrag = false;          // 拖动中途被关掉也要把心跳线程收掉
        stopModalBeat();
        Obj* o = objByHwnd(h);
        if (o) {
            if (g_vm) {
                for (int64_t sid : o->shapes) {         // ★ 连组内的子图形一起清
                    Obj* s = objAt(sid);
                    if (s) killObj(g_vm, *s);
                }
                releaseHandlers(g_vm, *o);
            }
            o->alive = false;
            for (int64_t sid : o->shapes) {
                Obj* s = objAt(sid);
                if (s) s->alive = false;
            }
            o->shapes.clear();
            releaseWindowBuffer(*o);            // ★ 窗口没了 → 缓冲也还掉
        }
        return 0;
    }
    case WM_NCHITTEST: {
        // ★ 无边框窗口：没有标题栏和边框，Windows 就不知道哪是"边缘"，
        //   于是拖边改大小会失效。这里按到客户区边缘的距离自己报 HTLEFT/HTTOP…
        LRESULT hit = DefWindowProcW(h, msg, wp, lp);
        Obj* o = objByHwnd(h);
        if (!o || !o->frameless || !o->resizable) return hit;
        POINT pt; pt.x = (int)(short)LOWORD(lp); pt.y = (int)(short)HIWORD(lp);
        RECT wr; GetWindowRect(h, &wr);
        int m = 6;                                  // 边缘判定宽度
        bool l = pt.x < wr.left + m,  r = pt.x >= wr.right - m;
        bool t2 = pt.y < wr.top + m,  b = pt.y >= wr.bottom - m;
        if (t2 && l) return HTTOPLEFT;
        if (t2 && r) return HTTOPRIGHT;
        if (b && l)  return HTBOTTOMLEFT;
        if (b && r)  return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t2) return HTTOP;
        if (b) return HTBOTTOM;
        return hit;
    }
    case WM_NCLBUTTONDOWN: {
        // ★ 点标题栏 → 我们自己拖（不交给 DefWindowProc，否则它会跑模态循环把动画冻住）
        if (wp == HTCAPTION) {                       // lParam = 屏幕坐标
            beginWindowDragAt(h, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            return 0;
        }
        break;                                      // 其它非客户区（边框、按钮）交给系统
    }
    case WM_CAPTURECHANGED: {
        endWindowDrag();                            // 捕获没了 → 拖动结束
        break;
    }
    case WM_NCLBUTTONDBLCLK:
        break;                                      // 双击标题栏最大化：交给 DefWindowProc
    case WM_SETCURSOR: {                // ★ 光标形状：Windows 每条鼠标消息都会问一次
        Obj* o = objByHwnd(h);
        if (o && !o->curCursor.empty() && loadCursorByName(o->curCursor)) {
            applyCursor(*o);
            return TRUE;                // 我设了，别用系统默认
        }
        break;                          // 没设 → 交给系统（箭头）
    }
    case WM_ERASEBKGND:
        return 1;                       // 背景自己在 WM_PAINT 里刷，避免闪烁
    // ─── ★ 拖动 / 改大小时的"模态循环"─────────────────────────────────────
    //   拖标题栏（或拖窗口边缘改大小）时，Windows 会在 DefWindowProc 里跑一个
    //   自己的消息循环，直到松手才回来 —— 这期间我们的 pump/run 循环压根没在跑，
    //   定时器自然没人派发，动画就"冻住"了（松手才恢复）。
    //   详见文件里 startModalBeat() 上面的说明：模态循环只捞鼠标消息，
    //   所以拖动期间由辅助线程每 10ms 发一条 WM_MOUSEMOVE 当心跳。
    case WM_MOVING:
    case WM_SIZING: {
        // ★ 这里跑用户回调是【有意为之】—— 不跑就等于拖动期间动画停了。
        //   注意别改 lParam 里的 RECT（那是系统的拖动矩形），交给 DefWindowProc。
        if (g_vm) { flushTimers(g_vm); flushAllPending(); }
        break;
    }
    case WM_ENTERSIZEMOVE: {
        g_inModalDrag = true;
        if (g_vm) startModalBeat(h);
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        g_inModalDrag = false;
        stopModalBeat();
        Obj* o = objByHwnd(h);
        if (o) {
            releaseWindowBuffer(*o);           // 拖完尺寸可能变了 → 缓冲按新尺寸重建
            o->fullDamage = true;
            o->damage.clear();
        }
        return 0;
    }
    // ─── ★ 事件：只入队，不打回调（回调统一在 pump 里派发）───
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        int btn = (msg == WM_LBUTTONDOWN) ? 1 : (msg == WM_RBUTTONDOWN ? 2 : 3);
        queueMouse(h, "mousedown", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), btn);
        SetCapture(h);              // ★ 拖到窗口外面也能收到 up（click 判定需要）
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int btn = (msg == WM_LBUTTONUP) ? 1 : (msg == WM_RBUTTONUP ? 2 : 3);
        if (btn == 1) endWindowDrag();          // 松手 → 自己实现的窗口拖动结束
        ReleaseCapture();
        queueMouse(h, "mouseup", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), btn);
        return 0;
    }
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
        queueMouse(h, "dblclick", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                   msg == WM_LBUTTONDBLCLK ? 1 : 2);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        TrackMouseEvent(&tme);          // 离开窗口时给一条 WM_MOUSELEAVE
        queueMouse(h, "mousemove", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), 0);
        // ★ 自己实现的窗口拖动：跟着光标挪窗口
        if (windowDragActive() && g_dragWin.hwnd == h)
            updateWindowDragAt(h, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        // ★ 拖动/改大小的模态循环期间：系统只派发鼠标消息，所以我们的人工心跳
        //   （startModalBeat 发的 WM_MOUSEMOVE）就是唯一的时钟 —— 借着它把
        //   到期定时器跑掉 + 脏区画进缓冲，动画才不会在拖动时冻住。
        if (g_inModalDrag && g_vm) { flushTimers(g_vm); flushAllPending(); }
        return 0;
    }
    case WM_MOUSELEAVE: {               // 鼠标离开窗口 → 让"悬停对象"归零（派发 mouseleave）
        Obj* o = objByHwnd(h);
        if (o) { Ev e; e.kind = "mouseleave"; e.winId = idOfObj(o); g_events.push_back(e); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt; pt.x = (int)(short)LOWORD(lp); pt.y = (int)(short)HIWORD(lp);
        ScreenToClient(h, &pt);
        queueMouse(h, "wheel", pt.x, pt.y, 0);
        if (!g_events.empty()) g_events.back().delta = (int)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        queueKey(h, "keydown", (int)wp);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        queueKey(h, "keyup", (int)wp);
        return 0;
    case WM_SIZE: {
        Obj* o = objByHwnd(h);
        if (o && wp != SIZE_MINIMIZED) {
            // ★ 尺寸变了 → 缓冲要按新尺寸重建，并且整面重画一次
            releaseWindowBuffer(*o);
            o->fullDamage = true;
            o->damage.clear();
            Ev e; e.kind = "resize"; e.winId = idOfObj(o);
            e.w = (int)LOWORD(lp); e.h = (int)HIWORD(lp);
            g_events.push_back(e);
        }
        return 0;
    }
    case WM_PAINT: {
        Obj* o = objByHwnd(h);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        if (o) paintWindow(*o, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool ensureClass() {
    if (g_classReady) return true;
    // DPI 感知：越早越好（必须在建窗口之前）
    if (!g_dpiDone) { SetProcessDPIAware(); g_dpiDone = true; }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;   // ★ 双击事件需要它
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) {
        aelib::fail("注册窗口类失败: " + aelib::sysErr());
        return false;
    }
    g_classReady = true;
    return true;
}

// GDI+ 只在第一次真正要画的时候启动（纯建窗口不开销）
bool ensureGdiplus() {
    if (g_gdiReady) return true;
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) {
        aelib::fail("GDI+ 初始化失败（画不了图形）");
        return false;
    }
    g_gdiReady = true;
    return true;
}

// ── 参数取窗口句柄：id 非法一律抛（程序性错误）────────────────────────────────
HWND mustWin(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    Obj* o = winOf(vm, aelib::argInt(vm, argv, argc, i, fn), fn);
    return o ? o->hwnd : nullptr;
}

inline void redrawFrame(HWND h) {
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// 按当前样式把"客户区 w×h"换算成整窗尺寸
inline void clientToWindowSize(HWND h, int cw, int ch, int* ow, int* oh) {
    RECT r;
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
    *ow = (int)(r.right - r.left);
    *oh = (int)(r.bottom - r.top);
}

// ── 对内小工具：返回 true/false ──────────────────────────────────────────────
int retBool(AeVM* vm, bool b) { ae_push_bool(vm, b ? 1 : 0); return 1; }

// 取一个 bool 开关（int 也接受，Ae 里 0/1 用得多）
bool argFlag(AeVM* vm, AeValue* argv, int argc, int i, const char* fn) {
    if (i >= argc || !(ae_is_bool(argv[i]) || ae_is_int(argv[i]))) {
        aelib::raise(vm, std::string(fn) + " 的第 " + std::to_string(i + 1) +
                             " 个参数需要 bool（true/false）");
        return false;
    }
    return argv[i].i != 0;
}

}  // namespace

// =============================================================================
//  创建 / 显示 / 关闭
// =============================================================================

// window(title, w, h) → id | null      （创建后是隐藏的，需要 show）
static int nat_window(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;                       // ★ 事件回调需要 VM：任何入口都记一下
    aelib::clearError();
    const std::string& title = aelib::argStr(vm, argv, argc, 0, "ui.window");
    int64_t w = aelib::argInt(vm, argv, argc, 1, "ui.window");
    int64_t h = aelib::argInt(vm, argv, argc, 2, "ui.window");
    if (w <= 0 || h <= 0 || w > 32767 || h > 32767) {
        aelib::fail("ui.window：窗口大小必须在 1..32767 之间（客户区像素），得到 " +
                    std::to_string(w) + "×" + std::to_string(h));
        ae_push_null(vm);
        return 1;
    }
    if (!ensureClass()) { ae_push_null(vm); return 1; }

    std::wstring wt = aelib::utf8ToUtf16(title);
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r;
    r.left = 0; r.top = 0; r.right = (LONG)w; r.bottom = (LONG)h;
    AdjustWindowRectEx(&r, style, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                (int)(r.right - r.left), (int)(r.bottom - r.top),
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        aelib::fail("创建窗口失败: " + aelib::sysErr());
        ae_push_null(vm);
        return 1;
    }
    g_objs.push_back(Obj());                  // id = 下标 + 1，永不回收
    Obj& o = g_objs.back();
    o.kind = OBJ_WINDOW;
    o.hwnd = hwnd;
    ae_push_int(vm, idOfObj(&o));
    return 1;
}

static int nat_show(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.show");
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    return retBool(vm, true);
}

static int nat_hide(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.hide");
    ShowWindow(h, SW_HIDE);
    return retBool(vm, true);
}

static int nat_close(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.close");
    DestroyWindow(h);                          // 触发 WM_DESTROY → id 失效
    return retBool(vm, true);
}

// =============================================================================
//  标题 / 大小 / 位置 / 样式
// =============================================================================

static int nat_title(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.title");
    const std::string& t = aelib::argStr(vm, argv, argc, 1, "ui.title");
    SetWindowTextW(h, aelib::utf8ToUtf16(t).c_str());
    return retBool(vm, true);
}

static int nat_titleOf(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.titleOf");
    int n = GetWindowTextLengthW(h);
    std::wstring buf((size_t)n + 1, L'\0');
    int got = GetWindowTextW(h, &buf[0], n + 1);
    buf.resize((size_t)(got < 0 ? 0 : got));
    ae_push_str(vm, aelib::utf16ToUtf8(buf));
    return 1;
}

static int nat_size(AeVM* vm, int argc, AeValue* argv) {           // 客户区尺寸
    HWND h = mustWin(vm, argv, argc, 0, "ui.size");
    aelib::clearError();
    int64_t w = aelib::argInt(vm, argv, argc, 1, "ui.size");
    int64_t d = aelib::argInt(vm, argv, argc, 2, "ui.size");
    if (w <= 0 || d <= 0 || w > 32767 || d > 32767) {
        aelib::fail("ui.size：尺寸必须在 1..32767 之间（客户区像素），得到 " +
                    std::to_string(w) + "×" + std::to_string(d));
        return retBool(vm, false);
    }
    int ow = 0, oh = 0;
    clientToWindowSize(h, (int)w, (int)d, &ow, &oh);
    if (!SetWindowPos(h, nullptr, 0, 0, ow, oh,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        aelib::fail("ui.size 失败: " + aelib::sysErr());
        return retBool(vm, false);
    }
    return retBool(vm, true);
}

static int nat_pos(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.pos");
    aelib::clearError();
    int64_t x = aelib::argInt(vm, argv, argc, 1, "ui.pos");
    int64_t y = aelib::argInt(vm, argv, argc, 2, "ui.pos");
    if (!SetWindowPos(h, nullptr, (int)x, (int)y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        aelib::fail("ui.pos 失败: " + aelib::sysErr());
        return retBool(vm, false);
    }
    return retBool(vm, true);
}

// resizable(id, flag)：WS_THICKFRAME —— 能不能拖边框改大小
static int nat_resizable(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.resizable");
    bool on = argFlag(vm, argv, argc, 1, "ui.resizable");
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if (on) st |= WS_THICKFRAME; else st &= ~(LONG_PTR)WS_THICKFRAME;
    SetWindowLongPtrW(h, GWL_STYLE, st);
    if (Obj* oo = objByHwnd(h)) oo->resizable = on;      // 无边框时认边缘要用
    redrawFrame(h);
    return retBool(vm, true);
}

// maximizable(id, flag)：WS_MAXIMIZEBOX —— 最大化按钮能不能用
static int nat_maximizable(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.maximizable");
    bool on = argFlag(vm, argv, argc, 1, "ui.maximizable");
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if (on) st |= WS_MAXIMIZEBOX; else st &= ~(LONG_PTR)WS_MAXIMIZEBOX;
    SetWindowLongPtrW(h, GWL_STYLE, st);
    redrawFrame(h);
    return retBool(vm, true);
}

static int nat_maximize(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.maximize");
    ShowWindow(h, SW_MAXIMIZE);
    return retBool(vm, true);
}

// beginDrag(id)：开始"拖动窗口"（在自己画的标题栏的 mousedown 回调里调它）
//   ★ 第四批起：这个拖动是我们自己做的（抓住鼠标跟光标走），【不会】进系统的
//     模态消息循环 —— 所以拖动期间动画照跑（以前拖一下就冻住，松手才恢复）。
//     代价：没有 Aero 贴边/拖动预览（见 beginWindowDrag 上面的说明）。
static int nat_beginDrag(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.beginDrag");
    beginWindowDrag(h);
    return retBool(vm, true);
}

static int nat_minimize(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.minimize");
    ShowWindow(h, SW_MINIMIZE);
    return retBool(vm, true);
}

static int nat_restore(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.restore");
    ShowWindow(h, SW_RESTORE);
    return retBool(vm, true);
}

// =============================================================================
//  查询 / 消息循环
// =============================================================================

static int nat_visible(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.visible");
    return retBool(vm, IsWindowVisible(h) != 0);
}

static int nat_count(AeVM* vm, int, AeValue*) {
    ae_push_int(vm, aliveCount());
    return 1;
}

// info(id) → 表：一次拿全，脚本里方便断言/调试
static int nat_info(AeVM* vm, int argc, AeValue* argv) {
    HWND h = mustWin(vm, argv, argc, 0, "ui.info");
    AeValue t = ae_new_table(vm);

    RECT wr; GetWindowRect(h, &wr);
    RECT cr; GetClientRect(h, &cr);
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);

    auto set = [&](const char* k, AeValue v) { ae_table_set(vm, t, ae_make_str(vm, k), v); };
    Obj* wo = objByHwnd(h);
    set("id",           ae_int_value(wo ? idOfObj(wo) : 0));
    set("alive",        ae_bool_value(IsWindow(h) != 0));
    set("visible",      ae_bool_value(IsWindowVisible(h) != 0));
    set("x",            ae_int_value(wr.left));
    set("y",            ae_int_value(wr.top));
    set("w",            ae_int_value(cr.right - cr.left));
    set("h",            ae_int_value(cr.bottom - cr.top));
    set("winW",         ae_int_value(wr.right - wr.left));
    set("winH",         ae_int_value(wr.bottom - wr.top));
    set("resizable",    ae_bool_value((st & WS_THICKFRAME) != 0));
    set("maximizable",  ae_bool_value((st & WS_MAXIMIZEBOX) != 0));
    set("minimizable",  ae_bool_value((st & WS_MINIMIZEBOX) != 0));
    set("maximized",    ae_bool_value(IsZoomed(h) != 0));

    int n = GetWindowTextLengthW(h);
    std::wstring buf((size_t)n + 1, L'\0');
    int got = GetWindowTextW(h, &buf[0], n + 1);
    buf.resize((size_t)(got < 0 ? 0 : got));
    set("title", ae_make_str(vm, aelib::utf16ToUtf8(buf)));

    ae_push_table(vm, t);
    return 1;
}

// pump() → 还活着的窗口数；只处理"当前已排队"的消息，不阻塞
//   ★ 第四批：pump 也会把【待重画的脏区】画进离屏缓冲（不用等系统发 WM_PAINT）——
//     于是"改属性 → pump 一下就生效"这条链子在隐藏窗口上也成立（测试好写），
//     而 WM_PAINT 只负责把缓冲贴到屏幕上。
static int nat_pump(AeVM* vm, int, AeValue*) {
    g_vm = vm;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);      // WndProc 只入队事件，不打回调
    }
    flushEvents(vm);                 // ★ 用户回调在这里跑（出错当场中断，带调用栈）
    flushTimers(vm);                 // ★ 定时器
    flushAllPending();               // ★ 把待重画的画进缓冲（隐藏窗口也一样）
    ae_push_int(vm, aliveCount());
    return 1;
}

// run(id) → 阻塞直到窗口关闭：id = 0 表示"等到所有窗口都关掉"
static int nat_run(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.run");
    HWND target = nullptr;
    if (id != 0) target = mustWin(vm, argv, argc, 0, "ui.run");
    if (id == 0 && aliveCount() == 0) { ae_push_bool(vm, 1); return 1; }

    MSG msg;
    for (;;) {
        // ★ 有定时器时不能死等消息（否则定时器永远不触发）：改成轮询 + 小睡
        if (!g_timers.empty()) {
            MSG m2;
            while (PeekMessageW(&m2, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m2);
                DispatchMessageW(&m2);
            }
            flushEvents(vm);
            flushTimers(vm);
            flushAllPending();                     // ★ 待重画的画进缓冲
            Sleep(1);
            if (target) { if (!IsWindow(target)) break; }
            else if (aliveCount() == 0) break;
            continue;
        }
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) break;         // WM_QUIT / 出错
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        flushEvents(vm);                          // ★ 每处理完一条消息就派发回调
        if (target) { if (!IsWindow(target)) break; }
        else if (aliveCount() == 0) break;
    }
    ae_push_bool(vm, 1);
    return 1;
}

static int nat_lastError(AeVM* vm, int, AeValue*) {
    const std::string& e = aelib::errorSlot();
    if (e.empty()) { ae_push_null(vm); return 1; }
    ae_push_str(vm, e);
    return 1;
}

namespace {

// =============================================================================
//  绘制层
// -----------------------------------------------------------------------------
//  模型：所有画出来的东西都是【图形对象】——创建时返回一个 id，之后随时
//        ui.set / ui.setAll 改属性（立刻重画），ui.get / ui.props 读属性。
//        z 决定层级（小的先画，在下面）；z 相同时按创建顺序，后建的在上面。
//  坐标：窗口客户区左上角为 (0,0)，x 向右、y 向下（和 Windows 一致）。
//  颜色："#RRGGBB" 或 "#AARRGGBB"（8 位时 alpha 在前），也接受整数
//        （≤0xFFFFFF 当 RGB，否则当 AARRGGBB）。
//  抗锯齿：每个图形独立的 aa 开关；描边以路径为中心线（和 SVG 一致），
//        所以 1px 描边的矩形会正好压住边界像素。
// =============================================================================

// ── 颜色 ────────────────────────────────────────────────────────────────────
bool parseColor(const std::string& s, Argb* out) {
    if (s.size() < 2 || s[0] != '#') return false;
    std::string hex = s.substr(1);
    for (char c : hex)
        if (!isxdigit((unsigned char)c)) return false;
    Argb v = 0;
    if (hex.size() == 6) {                       // #RRGGBB
        v = (Argb)strtoul(hex.c_str(), nullptr, 16) | 0xFF000000u;
    } else if (hex.size() == 8) {                // #AARRGGBB（alpha 在前）
        v = (Argb)strtoul(hex.c_str(), nullptr, 16);
    } else if (hex.size() == 3) {                // #RGB 简写：#f00 = #ff0000
        for (int i = 0; i < 3; i++) {
            char c = hex[(size_t)i];
            int d = (c >= '0' && c <= '9') ? c - '0' : (tolower((unsigned char)c) - 'a' + 10);
            v = (v << 4) | (Argb)d;
            v = (v << 4) | (Argb)d;
        }
        v |= 0xFF000000u;
    } else {
        return false;
    }
    *out = v;
    return true;
}

std::string colorToString(Argb c) {
    char buf[16];
    if ((c >> 24) == 0xFF)
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    else
        snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", (c >> 24) & 0xFF, (c >> 16) & 0xFF,
                 (c >> 8) & 0xFF, c & 0xFF);
    return std::string(buf);
}

// 整数就压 int：Ae 是严格类型，2.0 和 2 比较会报错，能整就别给 float
void pushNum(AeVM* vm, double d) {
    if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) ae_push_int(vm, (int64_t)d);
    else                                                   ae_push_float(vm, d);
}

std::wstring winTitle(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring buf((size_t)n + 1, L'\0');
    int got = GetWindowTextW(h, &buf[0], n + 1);
    buf.resize((size_t)(got < 0 ? 0 : got));
    return buf;
}

// 参数取颜色：字符串 "#..." 或整数
bool argColor(AeVM* vm, AeValue& v, Argb* out, const char* fn, int idx) {
    if (ae_is_str(v)) {
        size_t n = 0;
        const char* p = ae_to_str(v, &n);
        if (parseColor(std::string(p, n), out)) return true;
        aelib::raise(vm, std::string(fn) + " 的第 " + std::to_string(idx) + " 个参数颜色写法不对：" +
                             std::string(p, n) + "（要 \"#RRGGBB\" 或 \"#AARRGGBB\"）");
        return false;
    }
    if (ae_is_int(v)) {
        uint64_t x = (uint64_t)v.i;
        *out = (x <= 0xFFFFFFu) ? (Argb)(x | 0xFF000000u) : (Argb)(x & 0xFFFFFFFFu);
        return true;
    }
    aelib::raise(vm, std::string(fn) + " 的第 " + std::to_string(idx) + " 个参数需要颜色字符串或整数");
    return false;
}

// ── 属性表：名字 → 字段。set/get/props/setAll 都靠它，所以加属性只改这一处 ──
enum PropType { PT_NUM, PT_BOOL, PT_COLOR, PT_STR };

struct PropDef {
    const char* name;
    PropType    type;
    double Shape::*num;
    bool   Shape::*bl;
    Argb   Shape::*col;
    std::string Shape::*sval = nullptr;   // ★ 字符串属性（cursor 等）
};
const PropDef kProps[] = {
    { "x",           PT_NUM,   &Shape::x,           nullptr, nullptr },
    { "y",           PT_NUM,   &Shape::y,           nullptr, nullptr },
    { "w",           PT_NUM,   &Shape::w,           nullptr, nullptr },
    { "h",           PT_NUM,   &Shape::h,           nullptr, nullptr },
    { "r",           PT_NUM,   &Shape::r,           nullptr, nullptr },
    { "rx",          PT_NUM,   &Shape::rx,          nullptr, nullptr },
    { "ry",          PT_NUM,   &Shape::ry,          nullptr, nullptr },
    { "rtl",         PT_NUM,   &Shape::rtl,         nullptr, nullptr },
    { "rtr",         PT_NUM,   &Shape::rtr,         nullptr, nullptr },
    { "rbr",         PT_NUM,   &Shape::rbr,         nullptr, nullptr },
    { "rbl",         PT_NUM,   &Shape::rbl,         nullptr, nullptr },
    { "x2",          PT_NUM,   &Shape::x2,          nullptr, nullptr },
    { "y2",          PT_NUM,   &Shape::y2,          nullptr, nullptr },
    { "stroke",      PT_NUM,   &Shape::stroke,      nullptr, nullptr },
    { "z",           PT_NUM,   &Shape::z,           nullptr, nullptr },
    { "angle",       PT_NUM,   &Shape::angle,       nullptr, nullptr },
    { "scale",       PT_NUM,   &Shape::scale,       nullptr, nullptr },
    { "anchorX",     PT_NUM,   &Shape::anchorX,     nullptr, nullptr },
    { "anchorY",     PT_NUM,   &Shape::anchorY,     nullptr, nullptr },

    { "clipX",       PT_NUM,   &Shape::clipX,       nullptr, nullptr },
    { "clipY",       PT_NUM,   &Shape::clipY,       nullptr, nullptr },
    { "clipW",       PT_NUM,   &Shape::clipW,       nullptr, nullptr },
    { "clipH",       PT_NUM,   &Shape::clipH,       nullptr, nullptr },
    { "strokeColor", PT_COLOR, nullptr, nullptr,    &Shape::strokeColor },
    { "fillColor2",  PT_COLOR, nullptr, nullptr,    &Shape::fillColor2 },
    { "gradient",    PT_STR,   nullptr, nullptr, nullptr, &Shape::gradient },
    { "gradientAngle", PT_NUM, &Shape::gradientAngle, nullptr, nullptr },
    { "dash",        PT_STR,   nullptr, nullptr, nullptr, &Shape::dash },
    { "cap",         PT_STR,   nullptr, nullptr, nullptr, &Shape::cap },
    { "join",        PT_STR,   nullptr, nullptr, nullptr, &Shape::join },
    { "fillColor",   PT_COLOR, nullptr, nullptr,    &Shape::fillColor },
    { "fill",        PT_BOOL,  nullptr, &Shape::fill,    nullptr },
    { "closed",      PT_BOOL,  nullptr, &Shape::closed,  nullptr },
    { "visible",     PT_BOOL,  nullptr, &Shape::visible, nullptr },
    { "hits",        PT_BOOL,  nullptr, &Shape::hits,    nullptr },
    { "cursor",      PT_STR,   nullptr, nullptr, nullptr, &Shape::cursor },   // 鼠标形状：arrow/hand/ibeam/sizewe/sizens/sizeall/cross/wait/no
    { "aa",          PT_BOOL,  nullptr, &Shape::aa,      nullptr },
};
const int kPropCount = (int)(sizeof(kProps) / sizeof(kProps[0]));

const char* kindName(ShapeKind k) {
    switch (k) {
        case SK_POINT:   return "point";
        case SK_LINE:    return "line";
        case SK_POLY:    return "poly";
        case SK_RECT:    return "rect";
        case SK_CIRCLE:  return "circle";
        case SK_ELLIPSE: return "ellipse";
        case SK_TEXT:    return "text";
        case SK_PATH:    return "path";
        case SK_IMAGE:   return "image";
        case SK_GROUP:   return "group";
    }
    return "?";
}
// props() 里额外能看到的只读项
const char* kPropNamesAll =
    "x y w h r rx ry rtl rtr rbr rbl x2 y2 stroke z strokeColor fillColor fill closed visible aa"
    " fillColor2 hits cursor angle scale anchorX anchorY anchorAuto clipX clipY clipW clipH"
    " gradient gradientAngle dash cap join points cmds kind win（后两个只读，kind 是图形种类名）";

const PropDef* findProp(const std::string& name) {
    for (int i = 0; i < kPropCount; i++)
        if (name == kProps[i].name) return &kProps[i];
    return nullptr;
}

// 文字相关helper 里大量用 Gdiplus 的短名（REAL / Font / RectF），这里统一打开
using namespace Gdiplus;

// ── 文字专用属性 ───────────────────────────────────────────────────────────
enum TextPropType { TP_STR, TP_NUM, TP_BOOL, TP_COLOR, TP_HALIGN, TP_VALIGN };

struct TextPropDef {
    const char*   name;
    TextPropType  type;
    std::string Shape::*sval;
    double      Shape::*num;
    bool        Shape::*bl;
    Argb        Shape::*col;
    int         Shape::*al;
    const char*   help;
};
const TextPropDef kTextProps[] = {
    { "text",   TP_STR,    &Shape::text,   nullptr, nullptr, nullptr, nullptr, "文字内容（\\n 换行）" },
    { "font",   TP_STR,    &Shape::font,   nullptr, nullptr, nullptr, nullptr, "字体族名，例如 \"Consolas\"" },
    { "size",   TP_NUM,    nullptr, &Shape::size,   nullptr, nullptr, nullptr, "字号（像素）" },
    { "wrap",   TP_NUM,    nullptr, &Shape::wrap,   nullptr, nullptr, nullptr, "折行宽度（0 = 不折行）" },
    { "lineGap",TP_NUM,    nullptr, &Shape::lineGap,nullptr, nullptr, nullptr, "行距（额外像素）" },
    { "trim",   TP_STR,    &Shape::trim,   nullptr, nullptr, nullptr, nullptr, "放不下时：none / ellipsis" },
    { "bold",   TP_BOOL,   nullptr, nullptr, &Shape::bold,   nullptr, nullptr, "粗体" },
    { "italic", TP_BOOL,   nullptr, nullptr, &Shape::italic, nullptr, nullptr, "斜体" },
    { "color",  TP_COLOR,  nullptr, nullptr, nullptr, &Shape::color,  nullptr, "文字颜色" },
    { "align",  TP_HALIGN, nullptr, nullptr, nullptr, nullptr, &Shape::align,  "left|center|right：x 落在文字框哪条边" },
    { "valign", TP_VALIGN, nullptr, nullptr, nullptr, nullptr, &Shape::valign, "top|middle|bottom：y 落在文字框哪条边" },
};
const int kTextPropCount = (int)(sizeof(kTextProps) / sizeof(kTextProps[0]));

const TextPropDef* findTextProp(const std::string& name) {
    for (int i = 0; i < kTextPropCount; i++)
        if (name == kTextProps[i].name) return &kTextProps[i];
    return nullptr;
}

// 文字也能用的通用属性（几何/外观里对文字有意义的那些）
bool textAllowsCommon(const std::string& n) {
    return n == "x" || n == "y" || n == "z" || n == "visible" || n == "aa" || n == "hits" ||
           n == "cursor" || n == "clipX" || n == "clipY" || n == "clipW" || n == "clipH";
}

// ★ 修：这张表原来漏了 wrap / lineGap / trim 和几个通用项，用户会以为设不了
const char* kTextPropNames =
    "text font size wrap lineGap trim bold italic color align valign"
    "（外加通用的 x y z visible aa hits cursor clipX clipY clipW clipH）";

// 属性校验：把"在错的种类上设属性"变成明确报错，而不是静默忽略
std::string propCheck(ShapeKind k, const std::string& name) {
    bool isText = (k == SK_TEXT);
    const PropDef*     sp = findProp(name);
    const TextPropDef* tp = findTextProp(name);
    if (isText) {
        if (tp) return std::string();
        if (sp && textAllowsCommon(name)) return std::string();
        if (sp) return "文字没有 '" + name + "' 属性（文字的大小由字体和内容决定，用 textSize 查）";
        return std::string();              // 交给下面报"不认识属性"
    }
    if (tp) return "'" + name + "' 只有文字有（文字用 ui.text 创建）";
    return std::string();
}

// ── 字体：按 (族名,字号,粗,斜) 缓存，别每帧新建 ─────────────────────────────
const char* kDefaultFont = "Microsoft YaHei UI";     // 中文友好；找不到会自动兜底

struct FontEntry {
    Gdiplus::Font* font = nullptr;
    std::string    used;                              // 真正用上的族名（兜底后可能不同）
};
std::map<std::string, FontEntry> g_fonts;

FontEntry* getFont(const Shape& s) {
    char key[512];
    snprintf(key, sizeof(key), "%s|%.2f|%d%d", s.font.c_str(), s.size, s.bold ? 1 : 0, s.italic ? 1 : 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return &it->second;

    int style = Gdiplus::FontStyleRegular;
    if (s.bold && s.italic) style = Gdiplus::FontStyleBoldItalic;
    else if (s.bold)        style = Gdiplus::FontStyleBold;
    else if (s.italic)      style = Gdiplus::FontStyleItalic;
    REAL px = (REAL)(s.size > 0.1 ? s.size : 1.0);

    FontEntry e;
    std::wstring wfam = aelib::utf8ToUtf16(s.font);
    if (!wfam.empty()) {
        Gdiplus::FontFamily fam(wfam.c_str());
        if (fam.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Font* f = new Gdiplus::Font(&fam, px, style, Gdiplus::UnitPixel);
            if (f->GetLastStatus() == Gdiplus::Ok) { e.font = f; e.used = s.font; }
            else delete f;
        }
    }
    if (!e.font) {                                    // 族名不存在 → 兜底默认族
        Gdiplus::FontFamily def(aelib::utf8ToUtf16(kDefaultFont).c_str());
        if (def.GetLastStatus() == Gdiplus::Ok) {
            e.font = new Gdiplus::Font(&def, px, style, Gdiplus::UnitPixel);
            e.used = kDefaultFont;
        } else {
            e.font = new Gdiplus::Font(Gdiplus::FontFamily::GenericSansSerif(), px, style, Gdiplus::UnitPixel);
            e.used = "sans-serif";
        }
    }
    auto res = g_fonts.emplace(key, e);
    return &res.first->second;
}

// 量文字：Typographic 版式（去掉 GDI+ 默认那圈多余留白），不自动折行
Gdiplus::RectF measureText(Gdiplus::Graphics& g, Gdiplus::Font* f, const std::wstring& wt) {
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap |
                      Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF box(0, 0, 1.0e6f, 1.0e6f);
    Gdiplus::RectF out(0, 0, 0, 0);
    if (!wt.empty()) g.MeasureString(wt.c_str(), (INT)wt.size(), f, box, &sf, &out);
    return out;
}

// =============================================================================
//  ★ 文字排版：折行自己算（不是交给 GDI+），这样"量出来的"和"画出来的"严格一致 ——
//    文本输入框的光标定位、省略号截断都靠这个前提。
// =============================================================================
struct TextLayout {
    std::vector<std::wstring> lines;   // 每行内容
    std::vector<double>  widths;       // 每行宽度
    std::vector<int>     starts;       // 每行第一个字符在整串里的下标
    std::vector<double>  charX;        // 每个字符在【自己行内】的 x 偏移（光标定位用）
    double w = 0, h = 0, lineH = 0;
};

// ★ 排版缓存：逐字符宽度要对每个前缀量一次（80 字 = 80 次 MeasureString，约十几毫秒），
//   而每帧重画都要排版一次 —— 所以按"影响排版的属性"做 key 缓存。
//   命中就零成本；key 变了自然失效（不用手工维护 dirty 标志）。
std::map<std::string, TextLayout> g_textLayouts;

bool layoutText(Gdiplus::Graphics& g, Shape& s, TextLayout* out) {
    FontEntry* fe = getFont(s);
    if (!fe || !fe->font) return false;
    char kb[64];
    snprintf(kb, sizeof(kb), "|%.2f|%d%d|%.2f|%.2f", s.size, s.bold ? 1 : 0, s.italic ? 1 : 0,
             s.wrap, s.lineGap);
    std::string key = s.text + "\x01" + s.font + kb;
    auto ci = g_textLayouts.find(key);
    if (ci != g_textLayouts.end()) { *out = ci->second; return true; }
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap |
                      Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    auto wOf = [&](const std::wstring& w) -> double {
        if (w.empty()) return 0.0;
        Gdiplus::RectF box(0, 0, 1.0e6f, 1.0e6f), o;
        g.MeasureString(w.c_str(), (INT)w.size(), fe->font, box, &sf, &o);
        return (double)o.Width;
    };
    out->lineH = (double)fe->font->GetHeight(&g);
    double gap = s.lineGap > 0 ? s.lineGap : 0;
    double wrapW = s.wrap > 0 ? s.wrap : 0;

    std::wstring all = aelib::utf8ToUtf16(s.text);
    std::vector<std::pair<size_t, size_t>> paras;      // [起点, 长度]（\n 分段）
    size_t p0 = 0;
    for (size_t i = 0; i <= all.size(); i++)
        if (i == all.size() || all[i] == L'\n') { paras.push_back({p0, i - p0}); p0 = i + 1; }

    for (auto& pr : paras) {
        const std::wstring& para = all;              // 只是为了让下面的 substr 读起来短一点
        if (wrapW <= 0) {                            // 不折行：一段一行
            std::wstring line = para.substr(pr.first, pr.second);
            out->lines.push_back(line);
            out->widths.push_back(wOf(line));
            out->starts.push_back((int)pr.first);
            for (size_t k = 0; k < line.size(); k++)
                out->charX.push_back(wOf(line.substr(0, k)));
            continue;
        }
        size_t i = pr.first, endP = pr.first + pr.second;
        bool first = true;
        while (first || i < endP) {
            first = false;
            size_t e = i, lastSpace = (size_t)-1;
            while (e < endP) {
                double wt = wOf(para.substr(i, e - i + 1));
                if (wt > wrapW && e > i) break;
                if (para[e] == L' ') lastSpace = e;
                e++;
            }
            // 能在空格处断就断（不然中文那种没有空格的就得硬断）
            if (e < endP && lastSpace != (size_t)-1 && lastSpace > i) e = lastSpace;
            if (e == i) e = i + 1;                   // 一个字符都放不下 → 硬放一个
            std::wstring line = para.substr(i, e - i);
            out->lines.push_back(line);
            out->widths.push_back(wOf(line));
            out->starts.push_back((int)i);
            double cw = 0;
            for (size_t k = i; k < e; k++) {
                out->charX.push_back(cw);
                cw = wOf(para.substr(i, k - i + 1));
            }
            if (e < endP && para[e] == L' ') {       // 断行处的空格挂在本行末尾（不影响观感）
                out->charX.push_back(wOf(line));
                i = e + 1;
            } else {
                i = e;
            }
        }
        if (pr.second == 0) {                        // 空行
            out->lines.push_back(L"");
            out->widths.push_back(0);
            out->starts.push_back((int)pr.first);
        }
    }
    out->w = 0;
    for (double w : out->widths) out->w = (std::max)(out->w, w);
    if (wrapW > 0) out->w = wrapW;                   // 折行时框宽就是指定宽度（对齐才有意义）
    out->h = (double)out->lines.size() * (out->lineH + gap);
    if (!out->lines.empty()) out->h -= gap;
    if (g_textLayouts.size() > 256) g_textLayouts.clear();   // 兜底：别无限长（动画改 wrap 会造很多 key）
    g_textLayouts[key] = *out;
    return true;
}

// 文字框（左上角）＝按对齐方式把 (x,y) 当锚点摊开
void textBox(Gdiplus::Graphics& g, Shape& s, Gdiplus::RectF* box, Gdiplus::Font** font,
             TextLayout* lay = nullptr) {
    FontEntry* fe = getFont(s);
    *font = fe->font;
    double w = 0, h = 0;
    if (s.wrap > 0 || s.lineGap > 0 || lay) {
        TextLayout L;
        if (layoutText(g, s, &L)) {
            if (lay) *lay = L;
            w = L.w; h = L.h;
        }
    } else {
        std::wstring wt = aelib::utf8ToUtf16(s.text);
        Gdiplus::RectF m = measureText(g, fe->font, wt);
        w = (double)m.Width; h = (double)m.Height;
    }
    double x0 = s.align == 1 ? s.x - w / 2 : (s.align == 2 ? s.x - w : s.x);
    double y0 = s.valign == 1 ? s.y - h / 2 : (s.valign == 2 ? s.y - h : s.y);
    box->X = (REAL)x0; box->Y = (REAL)y0;
    box->Width = (REAL)w; box->Height = (REAL)h;
}

bool parseHAlign(const std::string& v, int* out) {
    if (v == "left" || v == "左")   { *out = 0; return true; }
    if (v == "center" || v == "中") { *out = 1; return true; }
    if (v == "right" || v == "右")  { *out = 2; return true; }
    return false;
}
bool parseVAlign(const std::string& v, int* out) {
    if (v == "top" || v == "上")     { *out = 0; return true; }
    if (v == "middle" || v == "中")  { *out = 1; return true; }
    if (v == "bottom" || v == "下")  { *out = 2; return true; }
    return false;
}
const char* hAlignName(int a) { return a == 1 ? "center" : (a == 2 ? "right" : "left"); }
const char* vAlignName(int a) { return a == 1 ? "middle" : (a == 2 ? "bottom" : "top"); }


// 参数取数值（int/float/bool 都行）
double argNum(AeVM* vm, AeValue& v, const char* fn, const char* prop, int idx) {
    if (ae_is_int(v))   return (double)v.i;
    if (ae_is_float(v)) return v.f;
    if (ae_is_bool(v))  return v.i ? 1.0 : 0.0;
    aelib::raise(vm, std::string(fn) + " 设置 " + prop + " 需要数值（第 " + std::to_string(idx) + " 个参数）");
    return 0;
}

// 点数组：{x1,y1,x2,y2,...}（至少 2 个点）
bool readPoints(AeVM* vm, AeValue t, std::vector<double>* out, const char* fn) {
    int n = ae_table_len(vm, t);
    if (n < 4 || (n % 2) != 0) {
        aelib::raise(vm, std::string(fn) + " 的点数组要写成 {x1,y1,x2,y2,...}："
                             "至少 2 个点、个数为偶数，当前 " + std::to_string(n) + " 个数字");
        return false;
    }
    out->clear();
    out->reserve((size_t)n);
    for (int i = 1; i <= n; i++) {
        AeValue v;
        if (!ae_table_get(vm, t, ae_int_value(i), &v) || !ae_is_num(v)) {
            aelib::raise(vm, std::string(fn) + " 的点数组第 " + std::to_string(i) + " 项不是数字");
            return false;
        }
        out->push_back(ae_is_int(v) ? (double)v.i : v.f);
    }
    return true;
}

// 数值 → AeValue：能整就压 int（Ae 严格类型，2.0 和 2 比较会报错）
AeValue ae_num_value(double d) {
    if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) return ae_int_value((int64_t)d);
    AeValue v = ae_int_value(0);
    v.kind = AE_FLOAT;
    v.f = d;
    return v;
}

// ★ 路径指令：读 + 校验。指令流是扁平数字数组：
//     1 x y                 移动（开新子路径）
//     2 x y                 直线
//     3 cx cy x y           二次贝塞尔（控制点 + 终点）
//     4 c1x c1y c2x c2y x y 三次贝塞尔
//     5 x y w h start sweep 弧（外接矩形 + 起始角 + 扫过角，单位【度】，0° 是三点钟方向、顺时针）
//     6                     闭合当前子路径
bool readCmds(AeVM* vm, AeValue t, std::vector<double>* out, const char* fn) {
    int n = ae_table_len(vm, t);
    if (n < 1) {
        aelib::raise(vm, std::string(fn) + " 的路径指令数组不能为空（1 移动 2 直线 3 二次 4 三次 5 弧 6 闭合）");
        return false;
    }
    out->clear();
    out->reserve((size_t)n);
    for (int i = 1; i <= n; i++) {
        AeValue v;
        if (!ae_table_get(vm, t, ae_int_value(i), &v) || !ae_is_num(v)) {
            aelib::raise(vm, std::string(fn) + " 的路径指令第 " + std::to_string(i) + " 项不是数字");
            return false;
        }
        out->push_back(ae_is_int(v) ? (double)v.i : v.f);
    }
    size_t k = 0;
    while (k < out->size()) {
        int op = (int)(*out)[k];
        size_t need = 0;
        if (op == 1 || op == 2)      need = 3;
        else if (op == 3)            need = 5;
        else if (op == 4 || op == 5) need = 7;
        else if (op == 6)            need = 1;
        else {
            aelib::raise(vm, std::string(fn) + " 的路径指令第 " + std::to_string(k + 1) +
                             " 项不是合法操作码（1 移动 2 直线 3 二次 4 三次 5 弧 6 闭合）");
            return false;
        }
        if (k + need > out->size()) {
            aelib::raise(vm, std::string(fn) + " 的路径指令第 " + std::to_string(k + 1) +
                             " 条参数不够（" + std::to_string(need - 1) + " 个数字），指令流在中间截断了");
            return false;
        }
        k += need;
    }
    return true;
}

// 设一个属性（图形）
bool setShapeProp(AeVM* vm, Obj* o, const std::string& name, AeValue v, const char* fn, int idx) {
    Shape& s = o->sh;
    if (name == "anchorAuto") {          // anchorAuto = true → 旋转中心自动取包围盒中心
        if (!(ae_is_bool(v) || ae_is_int(v))) {
            aelib::raise(vm, std::string(fn) + " 设置 anchorAuto 需要 bool");
            return false;
        }
        s.hasAnchor = (v.i == 0);
        return true;
    }
    if (name == "cmds") {
        if (s.kind != SK_PATH) {
            aelib::raise(vm, std::string(fn) + "：只有 path 有 cmds 属性（当前是 " + kindName(s.kind) + "）");
            return false;
        }
        if (!ae_is_table(v)) {
            aelib::raise(vm, std::string(fn) + " 设置 cmds 需要指令数组 {1,x,y, 2,x,y, ...}");
            return false;
        }
        return readCmds(vm, v, &s.cmds, fn);
    }
    if (name == "points") {
        if (s.kind != SK_POLY) {
            aelib::raise(vm, std::string(fn) + "：只有 poly 有 points 属性（当前是 " + kindName(s.kind) + "）");
            return false;
        }
        if (!ae_is_table(v)) { aelib::raise(vm, std::string(fn) + " 设置 points 需要表 {x1,y1,...}"); return false; }
        return readPoints(vm, v, &s.pts, fn);
    }
    // 属性与种类是否匹配（静默忽略比报错更难查，所以这里一律拒绝）
    std::string why = propCheck(s.kind, name);
    if (!why.empty()) {
        aelib::raise(vm, std::string(fn) + "：" + why + "；" +
                             (s.kind == SK_TEXT ? std::string("文字可用：") + kTextPropNames : std::string("可用：") + kPropNamesAll));
        return false;
    }
    // 文字专用属性
    if (const TextPropDef* td = findTextProp(name)) {
        switch (td->type) {
            case TP_STR: {
                if (!ae_is_str(v)) {
                    aelib::raise(vm, std::string(fn) + " 设置 " + td->name + " 需要字符串");
                    return false;
                }
                size_t n = 0;
                const char* p = ae_to_str(v, &n);
                std::string val(p, n);
                if (std::string(td->name) == "trim") {          // 只有两种取值，写错就报错
                    if (val.empty()) val = "none";
                    else if (val != "none" && val != "ellipsis") {
                        aelib::raise(vm, std::string(fn) + " 不认识的 trim '" + val +
                                         "'；可用：none（直接截掉，默认）ellipsis（截断处加 …）");
                        return false;
                    }
                }
                s.*(td->sval) = val;
                break;
            }
            case TP_NUM:   s.*(td->num) = argNum(vm, v, fn, td->name, idx); break;
            case TP_BOOL:
                if (!(ae_is_bool(v) || ae_is_int(v))) {
                    aelib::raise(vm, std::string(fn) + " 设置 " + td->name + " 需要 bool");
                    return false;
                }
                s.*(td->bl) = (v.i != 0);
                break;
            case TP_COLOR: {
                Argb c = 0;
                if (!argColor(vm, v, &c, fn, idx)) return false;
                s.*(td->col) = c;
                break;
            }
            case TP_HALIGN:
            case TP_VALIGN: {
                if (!ae_is_str(v)) {
                    aelib::raise(vm, std::string(fn) + " 的 " + td->name + " 需要字符串 " +
                                         (td->type == TP_HALIGN ? "left/center/right" : "top/middle/bottom"));
                    return false;
                }
                size_t n = 0;
                const char* p = ae_to_str(v, &n);
                std::string sv(p, n);
                int out = 0;
                bool ok = (td->type == TP_HALIGN) ? parseHAlign(sv, &out) : parseVAlign(sv, &out);
                if (!ok) {
                    aelib::raise(vm, std::string(fn) + " 的 " + td->name + " 只认 " +
                                         (td->type == TP_HALIGN ? "left/center/right" : "top/middle/bottom") +
                                         "，得到 '" + sv + "'");
                    return false;
                }
                s.*(td->al) = out;
                break;
            }
        }
        return true;
    }
    const PropDef* d = findProp(name);
    if (!d) {
        aelib::raise(vm, std::string(fn) + " 不认识属性 '" + name + "'；可用：" +
                             (s.kind == SK_TEXT ? kTextPropNames : kPropNamesAll));
        return false;
    }
    switch (d->type) {
        case PT_NUM:   s.*(d->num) = argNum(vm, v, fn, d->name, idx);
                       if (std::string(d->name) == "anchorX" || std::string(d->name) == "anchorY")
                           s.hasAnchor = true;      // 显式给了锚点就不再自动
                       break;
        case PT_BOOL: {
            if (!(ae_is_bool(v) || ae_is_int(v))) {
                aelib::raise(vm, std::string(fn) + " 设置 " + d->name + " 需要 bool");
                return false;
            }
            s.*(d->bl) = (v.i != 0);
            break;
        }
        case PT_COLOR: {
            Argb c = 0;
            if (!argColor(vm, v, &c, fn, idx)) return false;
            s.*(d->col) = c;
            break;
        }
        case PT_STR: {
            if (!ae_is_str(v)) {
                aelib::raise(vm, std::string(fn) + " 设置 " + d->name + " 需要字符串");
                return false;
            }
            size_t n = 0;
            const char* p = ae_to_str(v, &n);
            std::string val(p, n);
            const std::string nm(d->name);
            // ★ 这几项的值域很小：不认识就报错，别静默退回默认值
            if (nm == "gradient") {
                // 线性渐变的方向其实就是一个角度（GDI+ 约定：0° 从左到右、90° 从上到下），
                // 但让人记角度不友好 —— 所以也给 horizontal/vertical/diagonal 这种说法，
                // 设的时候折算成 linear + gradientAngle，get 回来是规范形（所见即所得）
                if (val == "horizontal" || val == "h")      { val = "linear"; s.gradientAngle = 0; }
                else if (val == "vertical" || val == "v")   { val = "linear"; s.gradientAngle = 90; }
                else if (val == "diagonal" || val == "d")   { val = "linear"; s.gradientAngle = 45; }
                else if (val == "none" || val == "off")     { val.clear(); }
                else if (!(val.empty() || val == "linear" || val == "radial")) {
                    aelib::raise(vm, std::string(fn) +
                        " 不认识的渐变 '" + val +
                        "'；可用：linear（线性，方向看 gradientAngle）radial（径向）"
                        "horizontal vertical diagonal，空串 = 关闭渐变");
                    return false;
                }
            } else if (nm == "cap") {
                if (val.empty()) val = "flat";
                else if (val != "flat" && val != "round" && val != "square") {
                    aelib::raise(vm, std::string(fn) + " 不认识的线帽 '" + val +
                                     "'；可用：flat（平头，默认）round（圆头）square（方头）");
                    return false;
                }
            } else if (nm == "join") {
                if (val.empty()) val = "round";
                else if (val != "miter" && val != "round" && val != "bevel") {
                    aelib::raise(vm, std::string(fn) + " 不认识的线接 '" + val +
                                     "'；可用：round（圆角，默认）miter（尖角）bevel（切角）");
                    return false;
                }
            } else if (nm == "cursor") {
                if (!validCursorName(val)) {
                    aelib::raise(vm, std::string(fn) + " 不认识的鼠标形状 '" + val +
                                     "'；可用：" + kCursorNames + "（空串 = 沿用窗口默认）");
                    return false;
                }
            }
            s.*(d->sval) = val;
            break;
        }
    }
    return true;
}

// ── 几何：按图形种类生成 GDI+ 路径 ───────────────────────────────────────────
void addRoundRect(Gdiplus::GraphicsPath& p, double x, double y, double w, double h,
                  double rtl, double rtr, double rbr, double rbl) {
    using namespace Gdiplus;
    double maxr = (std::min)(std::fabs(w), std::fabs(h)) / 2.0;
    auto cl = [&](double r) { return r < 0.0 ? 0.0 : (r > maxr ? maxr : r); };
    rtl = cl(rtl); rtr = cl(rtr); rbr = cl(rbr); rbl = cl(rbl);
    // 相邻两个圆角加起来不能超过边长，超了就按比例一起缩
    auto shrink = [](double& a, double& b, double len) {
        if (a + b > len && a + b > 0) { double k = len / (a + b); a *= k; b *= k; }
    };
    shrink(rtl, rtr, w); shrink(rbl, rbr, w);
    shrink(rtl, rbl, h); shrink(rtr, rbr, h);

    double x1 = x + w, y1 = y + h;
    p.StartFigure();
    if (rtl > 0) p.AddArc((REAL)x, (REAL)y, (REAL)(2 * rtl), (REAL)(2 * rtl), 180.0f, 90.0f);
    p.AddLine((REAL)(x + rtl), (REAL)y, (REAL)(x1 - rtr), (REAL)y);
    if (rtr > 0) p.AddArc((REAL)(x1 - 2 * rtr), (REAL)y, (REAL)(2 * rtr), (REAL)(2 * rtr), 270.0f, 90.0f);
    p.AddLine((REAL)x1, (REAL)(y + rtr), (REAL)x1, (REAL)(y1 - rbr));
    if (rbr > 0) p.AddArc((REAL)(x1 - 2 * rbr), (REAL)(y1 - 2 * rbr), (REAL)(2 * rbr), (REAL)(2 * rbr), 0.0f, 90.0f);
    p.AddLine((REAL)(x1 - rbr), (REAL)y1, (REAL)(x + rbl), (REAL)y1);
    if (rbl > 0) p.AddArc((REAL)x, (REAL)(y1 - 2 * rbl), (REAL)(2 * rbl), (REAL)(2 * rbl), 90.0f, 90.0f);
    p.AddLine((REAL)x, (REAL)(y1 - rbl), (REAL)x, (REAL)(y + rtl));
    p.CloseFigure();
}

// ── 裁剪 ────────────────────────────────────────────────────────────────────
//   每个图形自带的裁剪矩形（宽或高 <= 0 = 不裁剪）。绘制和命中测试都遵守它，
//   于是"滚动视图 / 进度条 / 文字省略 / 揭幕动画"这些都能在 Ae 层自己做出来。
bool hasClip(const Shape& s) { return s.clipW > 0 && s.clipH > 0; }
bool insideClip(const Shape& s, double x, double y) {
    return !hasClip(s) || (x >= s.clipX && y >= s.clipY &&
                           x <= s.clipX + s.clipW && y <= s.clipY + s.clipH);
}

// =============================================================================
//  ★ 统一几何：一个图形的路径（客户区坐标）
// -----------------------------------------------------------------------------
//  绘制（FillPath / DrawPath）和命中测试（IsVisible / Widen）都只用这一份实现 ——
//  以前"画出来的形状"和"点得到的形状"是两段独立代码，靠人工对齐，迟早漂移。
// =============================================================================
bool buildPath(Gdiplus::GraphicsPath& p, Shape& s) {
    using namespace Gdiplus;
    switch (s.kind) {
    case SK_POINT: {
        double r = s.r > 0 ? s.r : 1;
        p.AddEllipse((REAL)(s.x - r), (REAL)(s.y - r), (REAL)(2 * r), (REAL)(2 * r));
        return true;
    }
    case SK_LINE:
        p.AddLine((REAL)s.x, (REAL)s.y, (REAL)s.x2, (REAL)s.y2);
        return true;
    case SK_POLY: {
        if (s.pts.size() < 4) return false;
        int n = (int)(s.pts.size() / 2);
        std::vector<PointF> v;
        v.reserve((size_t)n);
        for (int i = 0; i < n; i++)
            v.push_back(PointF((REAL)(s.pts[(size_t)i * 2] + s.x), (REAL)(s.pts[(size_t)i * 2 + 1] + s.y)));
        if (s.closed) p.AddPolygon(v.data(), n);
        else          p.AddLines(v.data(), n);
        return true;
    }
    case SK_RECT: {
        // ★ 圆角钳制只写这一处（以前绘制和命中各抄了一份）
        double rtl = s.rtl, rtr = s.rtr, rbr = s.rbr, rbl = s.rbl;
        if (s.r > 0) {
            if (rtl == 0) rtl = s.r;
            if (rtr == 0) rtr = s.r;
            if (rbr == 0) rbr = s.r;
            if (rbl == 0) rbl = s.r;
        }
        if (rtl <= 0 && rtr <= 0 && rbr <= 0 && rbl <= 0)
            p.AddRectangle(RectF((REAL)s.x, (REAL)s.y, (REAL)s.w, (REAL)s.h));
        else
            addRoundRect(p, s.x, s.y, s.w, s.h, rtl, rtr, rbr, rbl);
        return true;
    }
    case SK_CIRCLE:
        p.AddEllipse((REAL)(s.x - s.r), (REAL)(s.y - s.r), (REAL)(2 * s.r), (REAL)(2 * s.r));
        return true;
    case SK_ELLIPSE:
        p.AddEllipse((REAL)(s.x - s.rx), (REAL)(s.y - s.ry), (REAL)(2 * s.rx), (REAL)(2 * s.ry));
        return true;
    case SK_TEXT: {
        // 文字绘制走 DrawString；命中用它的排版框（同一套度量，仍然是一致的）
        if (!ensureGdiplus()) return false;
        Bitmap bmp(1, 1, PixelFormat32bppARGB);
        Graphics g(&bmp);
        Font* f = nullptr;
        RectF box;
        textBox(g, s, &box, &f);
        if (box.Width <= 0 || box.Height <= 0) return false;
        p.AddRectangle(box);
        return true;
    }
    case SK_IMAGE:
        p.AddRectangle(RectF((REAL)s.x, (REAL)s.y, (REAL)s.w, (REAL)s.h));
        return true;
    case SK_PATH: {
        // 路径指令流（见文件头说明）。参数不够 / 操作码不认识 → 直接报错，不静默。
        size_t i = 0;
        // 坐标统一加 (x, y) 偏移：这样 ui.set(p, "x", ...) 能整体移动路径（和 poly 一致）
        auto N = [&](size_t k) -> REAL { return (REAL)(s.cmds[i + k] + (k % 2 == 1 ? s.x : s.y)); };
        PointF cur((REAL)s.x, (REAL)s.y);
        while (i < s.cmds.size()) {
            int op = (int)s.cmds[i];
            if (op == 1) {                       // 移动：开一个新子路径
                if (i + 2 >= s.cmds.size()) return true;
                cur = PointF((REAL)(s.cmds[i + 1] + s.x), (REAL)(s.cmds[i + 2] + s.y));
                p.StartFigure();
                p.AddLine(cur, cur);
                i += 3;
            } else if (op == 2) {                // 直线
                if (i + 2 >= s.cmds.size()) return true;
                PointF to(N(1), N(2));
                p.AddLine(cur, to); cur = to; i += 3;
            } else if (op == 3) {                // 二次贝塞尔
                if (i + 4 >= s.cmds.size()) return true;
                PointF c(N(1), N(2)), to(N(3), N(4));
                p.AddBezier(cur, c, c, to); cur = to; i += 5;
            } else if (op == 4) {                // 三次贝塞尔
                if (i + 6 >= s.cmds.size()) return true;
                PointF c1(N(1), N(2)), c2(N(3), N(4)), to(N(5), N(6));
                p.AddBezier(cur, c1, c2, to); cur = to; i += 7;
            } else if (op == 5) {                // 弧：外接矩形 + 起始角 + 扫过角（度）
                if (i + 6 >= s.cmds.size()) return true;
                p.AddArc(N(1), N(2), N(3), N(4), N(5), N(6));
                i += 7;
            } else if (op == 6) {                // 闭合
                p.CloseFigure(); i += 1;
            } else {
                throw std::runtime_error("路径指令第 " + std::to_string(i + 1) +
                                    " 项不是合法操作码（1 移动 2 直线 3 二次 4 三次 5 弧 6 闭合）");
            }
        }
        return true;
    }
    case SK_GROUP:               // 组没有自己的几何（"形状"是子图形的并集，单独递归处理）
        return false;
    }
    return false;
}

// 组的包围盒 = 子图形包围盒的并集（近似：不旋转子图形自己算，够用）
bool groupBounds(Obj& grp, Gdiplus::RectF* out);

// 旋转/缩放的锚点：显式给了就用它，否则用包围盒中心
void shapeAnchor(Shape& s, double* ax, double* ay, Obj* owner = nullptr) {
    if (s.hasAnchor) { *ax = s.anchorX; *ay = s.anchorY; return; }
    if (s.kind == SK_GROUP && owner) {                 // 组：用子图形的并集
        Gdiplus::RectF b;
        if (groupBounds(*owner, &b)) {
            *ax = (double)b.X + (double)b.Width / 2;
            *ay = (double)b.Y + (double)b.Height / 2;
            return;
        }
    }
    Gdiplus::GraphicsPath p;
    if (buildPath(p, s)) {
        Gdiplus::RectF b;
        if (p.GetBounds(&b) == Gdiplus::Ok && b.Width >= 0 && b.Height >= 0) {
            *ax = (double)b.X + (double)b.Width / 2;
            *ay = (double)b.Y + (double)b.Height / 2;
            return;
        }
    }
    *ax = s.x; *ay = s.y;
}

// ★ 命中的关键：把屏幕上的点【反向变换】回图形的本地坐标系，
//   然后照常和未变换的路径比较 —— 所以旋转/缩放之后照样点得准。
//   owner：组要从子图形算包围盒中心当锚点，所以必须把组自己传进来
//   （和 applyShapeTransform 用的是同一个 shapeAnchor，否则"画在哪"和"点在哪"会漂）
void invTransformPoint(Shape& s, double* x, double* y, Obj* owner = nullptr) {
    if (s.angle == 0 && s.scale == 1) return;
    double ax = 0, ay = 0;
    shapeAnchor(s, &ax, &ay, owner);
    double px = *x - ax, py = *y - ay;
    double sc = (s.scale != 0 ? s.scale : 1e-9);
    px /= sc; py /= sc;
    double a = -s.angle * 3.14159265358979323846 / 180.0;
    double c = std::cos(a), sn = std::sin(a);
    *x = ax + px * c - py * sn;
    *y = ay + px * sn + py * c;
}

void drawShape(Gdiplus::Graphics& g, Shape& s) {
    using namespace Gdiplus;
    g.SetSmoothingMode(s.aa ? SmoothingModeAntiAlias : SmoothingModeNone);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);       // 1px 线落在整数坐标上更实

    SolidBrush fillBr(Color(s.fillColor));
    Pen strokePen(Color(s.strokeColor), (REAL)(s.stroke > 0 ? s.stroke : 1));
    strokePen.SetLineJoin(s.join == "miter" ? LineJoinMiter :
                          s.join == "bevel" ? LineJoinBevel : LineJoinRound);
    strokePen.SetStartCap(s.cap == "round" ? LineCapRound :
                          s.cap == "square" ? LineCapSquare : LineCapFlat);
    strokePen.SetEndCap(s.cap == "round" ? LineCapRound :
                        s.cap == "square" ? LineCapSquare : LineCapFlat);
    if (!s.dash.empty()) {                           // 虚线样式 "6 3"
        std::vector<REAL> pat;
        const char* q = s.dash.c_str();
        while (*q) {
            while (*q == ' ' || *q == ',') q++;
            if (!*q) break;
            char* e = nullptr;
            double v = strtod(q, &e);
            if (e == q) break;
            if (v > 0) pat.push_back((REAL)v);
            q = e;
        }
        if (!pat.empty()) strokePen.SetDashPattern(pat.data(), (INT)pat.size());
    }
    bool hasStroke = s.stroke > 0;

    // ★ 裁剪 / 变换：需要还原绘图状态的两件事，缺一不可
    //   · 裁剪：只在本图形有裁剪矩形时才 Save（没裁剪的图形零开销）
    //   · 变换：★ 必须 Save/Restore —— 不然"转过的图形"会把旋转/缩放【留给后面画的
    //     所有图形】（以前每帧都新建一个 Graphics，而且转过的图形恰好排在最后，
    //     这个 bug 一直藏着；第四批的局部重画把绘制顺序打乱了，它立刻现形）
    //   位置很讲究：Save 在变换【之前】、SetClip 在变换【之后】——
    //   这样裁剪矩形是"图形自己的坐标"里的（转过的图形，裁剪也跟着转），
    //   和命中测试（先 invTransformPoint 再 insideClip）严格一致。
    GraphicsState st;
    bool clipped = hasClip(s);
    bool xformed = (s.angle != 0 || s.scale != 1);
    if (clipped || xformed) st = g.Save();

    // ★ 变换：绕锚点旋转 + 缩放。注意顺序 —— 先平移把锚点移到原点，旋转/缩放，
    //   再平移回去（这就是"绕锚点转"）。
    if (xformed) {
        double ax = 0, ay = 0;
        shapeAnchor(s, &ax, &ay);
        g.TranslateTransform((REAL)ax, (REAL)ay);
        if (s.angle != 0) g.RotateTransform((REAL)s.angle);
        if (s.scale != 1) g.ScaleTransform((REAL)s.scale, (REAL)s.scale);
        g.TranslateTransform((REAL)-ax, (REAL)-ay);
    }
    if (clipped) g.SetClip(RectF((REAL)s.clipX, (REAL)s.clipY, (REAL)s.clipW, (REAL)s.clipH));

    if (s.kind == SK_TEXT) {
        // 文字用灰度抗锯齿（AntiAliasGridFit）：清晰、不打彩边，而且离屏与屏上一致，
        // 所以 ui.pixel 的断言才有意义。要 ClearType 那种彩边锐利感是以后的可选项。
        g.SetTextRenderingHint(s.aa ? TextRenderingHintAntiAliasGridFit
                                    : TextRenderingHintSingleBitPerPixelGridFit);
        Font* font = nullptr;
        RectF box;
        if (s.wrap > 0 || s.lineGap > 0) {
            // ★ 折行：按自己算出来的行逐行画 —— 只有这样，"量出来的"和"画出来的"才一致
            TextLayout L;
            textBox(g, s, &box, &font, &L);
            if (font && !L.lines.empty() && box.Width > 0) {
                SolidBrush br(Color(s.color));
                StringFormat sf(StringFormat::GenericTypographic());
                sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
                double gap = s.lineGap > 0 ? s.lineGap : 0;
                auto mw = [&](const std::wstring& w) -> double {
                    if (w.empty()) return 0.0;
                    RectF m = measureText(g, font, w);
                    return (double)m.Width;
                };
                for (size_t li = 0; li < L.lines.size(); li++) {
                    std::wstring ln = L.lines[li];
                    double lw = L.widths[li];
                    if (s.trim == "ellipsis" && s.wrap > 0 && lw > (double)s.wrap) {
                        std::wstring ell = L"...";
                        double ew = mw(ell);
                        size_t keep = ln.size();
                        while (keep > 0 && mw(ln.substr(0, keep)) + ew > (double)s.wrap) keep--;
                        ln = ln.substr(0, keep) + ell;
                        lw = mw(ln);
                    }
                    double lx = (double)box.X;
                    if (s.align == 1)      lx = (double)box.X + ((double)box.Width - lw) / 2;
                    else if (s.align == 2) lx = (double)box.X + (double)box.Width - lw;
                    double ly = (double)box.Y + (double)li * (L.lineH + gap);
                    // 右下留余量：GDI+ 在"文字装不下排版框"时会整段不画（见下面注释）
                    RectF db((REAL)lx, (REAL)(ly - 2), (REAL)(lw + 8), (REAL)(L.lineH + 6));
                    g.DrawString(ln.c_str(), (INT)ln.size(), font, db, &sf, &br);
                }
            }
        } else {
        textBox(g, s, &box, &font);
        if (font && box.Width > 0 && box.Height > 0) {
            std::wstring wt = aelib::utf8ToUtf16(s.text);
            SolidBrush br(Color(s.color));
            StringFormat sf(StringFormat::GenericTypographic());
            sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
            sf.SetTrimming(StringTrimmingNone);
            // ★ 排版框往【右下】多给一点余量。原因：GDI+ 在"文字排不下排版框"时
            //   会整段一个字都不画（不是裁剪、不是省略号，是全空）。而纯中文/全角标点
            //   的实际排版宽度比 MeasureString 结果大一丁点（亚像素差），用"刚好等于
            //   量出尺寸"的框就会踩中这个坑 —— 表现是"中文完全不显示、拉丁字母正常"。
            //   多给的部分在右下，左上原点不变，align/valign 的位置语义不受影响。
            RectF drawBox(box.X, box.Y, box.Width + 8.0f, box.Height + 8.0f);
            g.DrawString(wt.c_str(), (INT)wt.size(), font, drawBox, &sf, &br);
        }
        }
    } else if (s.kind == SK_IMAGE) {
        // 贴图：来源可以是图片文件，也可以是画布（画布会按需先渲染）
        Obj* src = objAt(s.srcId);
        Bitmap* bm = nullptr;
        if (src && src->kind == OBJ_IMAGE)       bm = src->img;
        else if (src && src->kind == OBJ_CANVAS) { ensureCanvasRendered(*src); bm = src->bmp; }
        if (bm) {
            // ★ 只有真的在缩放时才用双三次插值：尺寸没变时插值 = 白白重采样一遍，
            //   一张全屏位图每帧多花好几毫秒（当年量到过 demo 帧率因此掉一半）。
            double sw = (double)bm->GetWidth(), sh = (double)bm->GetHeight();
            bool scaled = (std::fabs(s.w - sw) > 0.01 || std::fabs(s.h - sh) > 0.01);
            g.SetInterpolationMode((s.aa && scaled) ? InterpolationModeHighQualityBicubic
                                                    : InterpolationModeNearestNeighbor);
            // 只在【尺寸没变】时开高速像素模式（1:1 没有插值可做，纯粹省时间；
            // 这两个开关是"直接画在屏幕 DC 上"那个年代量出来的，现在画进 DIB 后
            // 整窗贴图只要 ~1ms，但高速模式仍然更快，就留着了）。
            // 缩放时保持高质量模式，不然采样会偏。
            if (!scaled) {
                g.SetPixelOffsetMode(PixelOffsetModeHighSpeed);
                g.SetSmoothingMode(SmoothingModeNone);
            }
            g.DrawImage(bm, RectF((REAL)s.x, (REAL)s.y, (REAL)s.w, (REAL)s.h));
            g.SetInterpolationMode(InterpolationModeDefault);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);      // 恢复：图形线条还用得上
        }
    } else {
        // ★ 其它种类统统走同一条路径：几何来自 buildPath（命中测试用的是同一份）
        GraphicsPath path;
        if (buildPath(path, s)) {
            bool fillable = s.fill && s.kind != SK_LINE && !(s.kind == SK_POLY && !s.closed);
            // 渐变：linear 用包围盒 + 角度，radial 用轮廓本身
            Brush* br = &fillBr;
            LinearGradientBrush* lgb = nullptr;
            PathGradientBrush*   pgb = nullptr;
            if (s.gradient == "linear") {
                RectF bb;
                if (path.GetBounds(&bb) == Ok && bb.Width > 0 && bb.Height > 0) {
                    lgb = new LinearGradientBrush(bb, Color(s.fillColor), Color(s.fillColor2),
                                                  (REAL)s.gradientAngle);
                    br = lgb;
                }
            } else if (s.gradient == "radial") {
                pgb = new PathGradientBrush(&path);
                pgb->SetCenterColor(Color(s.fillColor));
                // ★ GDI+ 要求"边缘颜色个数 = 路径点数"，只给 1 个会直接失败（brush 保持默认色）
                INT pc = path.GetPointCount();
                if (pc < 1) pc = 1;
                std::vector<Color> sc((size_t)pc, Color(s.fillColor2));
                INT cnt = pc;
                pgb->SetSurroundColors(sc.data(), &cnt);
                br = pgb;
            }
            if (fillable)   g.FillPath(br, &path);
            if (hasStroke)  g.DrawPath(&strokePen, &path);
            delete lgb;
            delete pgb;
        }
    }
    if (clipped || xformed) g.Restore(st);
}

// 一个表面里的条目（图形 + 组），按 (z, 创建顺序) 从下往上
std::vector<Obj*> shapesInDrawOrder(Obj& win, bool onlyVisible) {
    std::vector<Obj*> v;
    for (int64_t sid : win.shapes) {
        Obj* s = objAt(sid);
        if (s && (s->kind == OBJ_SHAPE || s->kind == OBJ_GROUP) && (!onlyVisible || s->sh.visible))
            v.push_back(s);
    }
    std::stable_sort(v.begin(), v.end(), [](Obj* a, Obj* b) {
        if (a->sh.z != b->sh.z) return a->sh.z < b->sh.z;
        return a->sh.seq < b->sh.seq;
    });
    return v;
}

// 把图形的"变换 + 裁剪"施加到 Graphics 上（绘制和组都要用同一套）
void applyShapeTransform(Gdiplus::Graphics& g, Shape& s, Obj* owner = nullptr) {
    if (s.angle != 0 || s.scale != 1) {
        double ax = 0, ay = 0;
        shapeAnchor(s, &ax, &ay, owner);
        g.TranslateTransform((REAL)ax, (REAL)ay);
        if (s.angle != 0) g.RotateTransform((REAL)s.angle);
        if (s.scale != 1) g.ScaleTransform((REAL)s.scale, (REAL)s.scale);
        g.TranslateTransform((REAL)-ax, (REAL)-ay);
    }
}
void applyShapeClip(Gdiplus::Graphics& g, Shape& s) {
    // ★ CombineModeIntersect：裁剪要【相交】而不是【替换】——
    //   默认的 Replace 会让内层裁剪把外层的（以及脏矩形的）裁剪顶掉
    if (hasClip(s))
        g.SetClip(RectF((REAL)s.clipX, (REAL)s.clipY, (REAL)s.clipW, (REAL)s.clipH),
                  Gdiplus::CombineModeIntersect);
}

// =============================================================================
//  ★ 脏矩形：几何（包围盒推算）
// =============================================================================
Gdiplus::RectF inflateRect(const Gdiplus::RectF& r, double pad) {
    return Gdiplus::RectF((REAL)(r.X - pad), (REAL)(r.Y - pad),
                          (REAL)(r.Width + 2 * pad), (REAL)(r.Height + 2 * pad));
}
bool rectsOverlap(const Gdiplus::RectF& a, const Gdiplus::RectF& b) {
    return !(a.X + a.Width <= b.X || b.X + b.Width <= a.X ||
             a.Y + a.Height <= b.Y || b.Y + b.Height <= a.Y);
}
void rectUnion(Gdiplus::RectF& a, const Gdiplus::RectF& b) {
    if (b.Width <= 0 || b.Height <= 0) return;
    if (a.Width <= 0 || a.Height <= 0) { a = b; return; }
    double x1 = (std::min)((double)a.X, (double)b.X);
    double y1 = (std::min)((double)a.Y, (double)b.Y);
    double x2 = (std::max)((double)a.X + a.Width,  (double)b.X + b.Width);
    double y2 = (std::max)((double)a.Y + a.Height, (double)b.Y + b.Height);
    a = Gdiplus::RectF((REAL)x1, (REAL)y1, (REAL)(x2 - x1), (REAL)(y2 - y1));
}
void rectIntersect(Gdiplus::RectF& a, const Gdiplus::RectF& b) {
    double x1 = (std::max)((double)a.X, (double)b.X);
    double y1 = (std::max)((double)a.Y, (double)b.Y);
    double x2 = (std::min)((double)a.X + a.Width,  (double)b.X + b.Width);
    double y2 = (std::min)((double)a.Y + a.Height, (double)b.Y + b.Height);
    if (x2 <= x1 || y2 <= y1) { a = Gdiplus::RectF(0, 0, 0, 0); return; }
    a = Gdiplus::RectF((REAL)x1, (REAL)y1, (REAL)(x2 - x1), (REAL)(y2 - y1));
}
// 命中测试的粗筛余量：细判里把路径按 (笔宽+6) 加宽，所以命中区可能比包围盒大 3px，
// 再留 1px 给浮点误差 —— 宁可多放几个进去细判，也不能漏掉
const double kHitSlack = 4.0;
bool pointInBoundsNear(const Gdiplus::RectF& r, double x, double y, double slack) {
    return x >= (double)r.X - slack && x <= (double)r.X + (double)r.Width + slack &&
           y >= (double)r.Y - slack && y <= (double)r.Y + (double)r.Height + slack;
}
// 绕 (ax,ay) 缩放 + 旋转后的包围盒（和 GDI+ 的 T(a)·R·S·T(-a) 顺序一致）
Gdiplus::RectF rotScaleRect(const Gdiplus::RectF& r, double ax, double ay,
                            double angle, double scale) {
    double s = (scale != 0 ? scale : 1e-9);
    double a = angle * 3.14159265358979323846 / 180.0;
    double c = std::cos(a), sn = std::sin(a);
    double xs[4] = { (double)r.X, (double)r.X + r.Width, (double)r.X, (double)r.X + r.Width };
    double ys[4] = { (double)r.Y, (double)r.Y, (double)r.Y + r.Height, (double)r.Y + r.Height };
    double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
    for (int i = 0; i < 4; i++) {
        double px = (xs[i] - ax) * s, py = (ys[i] - ay) * s;
        double qx = ax + px * c - py * sn;
        double qy = ay + px * sn + py * c;
        if (qx < x1) x1 = qx;
        if (qx > x2) x2 = qx;
        if (qy < y1) y1 = qy;
        if (qy > y2) y2 = qy;
    }
    return Gdiplus::RectF((REAL)x1, (REAL)y1, (REAL)(x2 - x1), (REAL)(y2 - y1));
}
// 量文字用的 Graphics（1x1 位图就行，只用来量尺寸）
Gdiplus::Graphics& measureGraphics() {
    static Gdiplus::Bitmap*   mb = nullptr;
    static Gdiplus::Graphics* mg = nullptr;
    if (!mg) {
        ensureGdiplus();
        mb = new Gdiplus::Bitmap(1, 1, PixelFormat32bppARGB);
        mg = new Gdiplus::Graphics(mb);
    }
    return *mg;
}
// 图形/组【自己坐标系】里"画出来占的地方"（含描边余量；不含自己的旋转缩放和祖先）
bool localBounds(Obj& o, Gdiplus::RectF* out) {
    if (o.kind == OBJ_GROUP) return groupBounds(o, out);
    Shape& s = o.sh;
    if (s.kind == SK_IMAGE) {
        *out = Gdiplus::RectF((REAL)s.x, (REAL)s.y, (REAL)s.w, (REAL)s.h);
        return s.w > 0 && s.h > 0;
    }
    if (s.kind == SK_TEXT) {
        Gdiplus::Font* f = nullptr;
        Gdiplus::RectF box;
        textBox(measureGraphics(), s, &box, &f);
        if (box.Width <= 0 || box.Height <= 0) return false;
        *out = inflateRect(box, 3);        // 画文字时右下多给了点余量，这里一并算上
        return true;
    }
    Gdiplus::GraphicsPath p;
    if (!buildPath(p, s)) return false;
    Gdiplus::RectF b;
    if (p.GetBounds(&b) != Gdiplus::Ok || b.Width < 0 || b.Height < 0) return false;
    double pad = 1.0 + (s.stroke > 0 ? s.stroke / 2 : 0);    // 描边以路径为中心线
    *out = inflateRect(b, pad);
    return true;
}
// 把"某个对象自己坐标系里的矩形"一路映射到它所在表面的坐标
//   （自己的旋转缩放 → 祖先组逐层的 平移 + 旋转缩放；组的平移在旋转【之后】施加）
bool toSurfaceRect(Obj& o, const Gdiplus::RectF& local, Gdiplus::RectF* out) {
    Gdiplus::RectF b = local;
    Obj* cur = &o;
    int guard = 0;
    while (cur && guard++ < 64) {
        Shape& s = cur->sh;
        if (s.angle != 0 || s.scale != 1) {
            double ax = 0, ay = 0;
            shapeAnchor(s, &ax, &ay, cur);
            b = rotScaleRect(b, ax, ay, s.angle, s.scale);
        }
        if (cur->kind == OBJ_GROUP) { b.X += (REAL)s.x; b.Y += (REAL)s.y; }
        if (cur->kind != OBJ_SHAPE && cur->kind != OBJ_GROUP) break;    // 到表面了
        Obj* up = objAt(s.winId);
        if (!up || up->kind != OBJ_GROUP) break;                        // 到表面了
        cur = up;
    }
    *out = b;
    return b.Width > 0 && b.Height > 0;
}
// 图形自己坐标系里的矩形 → 表面坐标，并且被【祖先的裁剪】削一遍
//   （被祖先裁掉的部分根本画不出来，没必要重画）
void clipByAncestors(Obj& o, Gdiplus::RectF* r) {
    Obj* g = objAt(o.sh.winId);
    int guard = 0;
    while (g && g->kind == OBJ_GROUP && guard++ < 64) {
        if (hasClip(g->sh)) {
            Gdiplus::RectF cr((REAL)g->sh.clipX, (REAL)g->sh.clipY,
                              (REAL)g->sh.clipW, (REAL)g->sh.clipH);
            Gdiplus::RectF cw;
            if (toSurfaceRect(*g, cr, &cw)) {
                rectIntersect(*r, cw);
                if (r->Width <= 0 || r->Height <= 0) return;
            }
        }
        g = objAt(g->sh.winId);
    }
}
// ★ 算一个条目的两个包围盒（一个函数里一起算，省得建两遍路径）：
//    pb = 【父坐标系】里占的地方（只叠自己的变换）—— 命中粗筛用它，
//         因为 collectHits 递归进组之后，手里那个点的坐标系就是"父坐标系"
//    wb = 【表面坐标系】里占的地方（再叠祖先的变换 + 自己与祖先的裁剪）—— 脏矩形用它
//   两个都跟着 wbOk 一起失效。混淆这两个坐标系会表现为"嵌套组里的子图形点不到"，
//   所以名字和用途都写清楚。
void computeItemBounds(Obj& o) {
    Gdiplus::RectF b;
    bool ok = localBounds(o, &b);
    Gdiplus::RectF pb(0, 0, 0, 0);
    if (ok) {
        if (o.kind == OBJ_GROUP && hasClip(o.sh)) {            // 组的裁剪在子图形坐标系里
            Gdiplus::RectF cr((REAL)o.sh.clipX, (REAL)o.sh.clipY,
                              (REAL)o.sh.clipW, (REAL)o.sh.clipH);
            rectIntersect(b, cr);
            if (b.Width <= 0 || b.Height <= 0) ok = false;
        }
        pb = b;
        if (o.sh.angle != 0 || o.sh.scale != 1) {              // 自己的旋转缩放（绕锚点）
            double ax = 0, ay = 0;
            shapeAnchor(o.sh, &ax, &ay, &o);
            pb = rotScaleRect(b, ax, ay, o.sh.angle, o.sh.scale);
        }
        // 组的 x/y 是"整组平移"，子图形的坐标里没有它 —— 所以要在父坐标里补上
        if (o.kind == OBJ_GROUP) { pb.X += (REAL)o.sh.x; pb.Y += (REAL)o.sh.y; }
    }
    Gdiplus::RectF wb = pb;
    if (ok) {
        if (hasClip(o.sh) && o.kind != OBJ_GROUP) {            // 图形自己的裁剪
            Gdiplus::RectF cr((REAL)o.sh.clipX, (REAL)o.sh.clipY,
                              (REAL)o.sh.clipW, (REAL)o.sh.clipH);
            Gdiplus::RectF cw;
            if (toSurfaceRect(o, cr, &cw)) rectIntersect(wb, cw);
        }
        // ★ 再一路叠上祖先组的变换（每层：绕它的锚点旋转缩放 → 再整组平移）
        Obj* up = objAt(o.sh.winId);
        int guard = 0;
        while (up && up->kind == OBJ_GROUP && guard++ < 64) {
            Shape& gs = up->sh;
            if (gs.angle != 0 || gs.scale != 1) {
                double ax = 0, ay = 0;
                shapeAnchor(gs, &ax, &ay, up);
                wb = rotScaleRect(wb, ax, ay, gs.angle, gs.scale);
            }
            wb.X += (REAL)gs.x;
            wb.Y += (REAL)gs.y;
            up = objAt(gs.winId);
        }
        clipByAncestors(o, &wb);                               // 祖先的裁剪：相交
        if (wb.Width <= 0 || wb.Height <= 0) ok = false;
    }
    o.wbOk = true;
    o.pb   = ok ? pb : Gdiplus::RectF(0, 0, 0, 0);
    o.wb   = ok ? wb : Gdiplus::RectF(0, 0, 0, 0);
    o.wbX = o.sh.x; o.wbY = o.sh.y; o.wbA = o.sh.angle; o.wbS = o.sh.scale;
}
// 图形/组在【表面坐标】里占的地方（脏矩形用）
bool itemWorldBounds(Obj& o, Gdiplus::RectF* out) {
    if (!o.wbOk) computeItemBounds(o);
    *out = o.wb;
    return o.wb.Width > 0 && o.wb.Height > 0;
}
// 图形/组在【父坐标系】里占的地方（命中粗筛用：那个点的坐标系就是这个）
bool itemParentBounds(Obj& o, Gdiplus::RectF* out) {
    if (!o.wbOk) computeItemBounds(o);
    *out = o.pb;
    return o.pb.Width > 0 && o.pb.Height > 0;
}
// 一个组里有多少个【叶子图形】（叶子 = 真正要画的图形，组本身不算）
//   跳过整组时用它统计"省了多少个图形"；结果不缓存，但同一帧只会走一次（见 dmgStamp）
int64_t countLeaves(Obj& g) {
    int64_t n = 0;
    for (int64_t cid : g.shapes) {
        Obj* c = objAt(cid);
        if (!c) continue;
        n += (c->kind == OBJ_GROUP) ? countLeaves(*c) : 1;
    }
    return n;
}
void invalidateSubtree(Obj& g) {
    for (int64_t cid : g.shapes) {
        Obj* c = objAt(cid);
        if (!c) continue;
        c->wbOk = false;
        if (c->kind == OBJ_GROUP) invalidateSubtree(*c);
    }
}
void remapSubtree(Obj& g, double dx, double dy) {
    for (int64_t cid : g.shapes) {
        Obj* c = objAt(cid);
        if (!c) continue;
        if (c->wbOk) { c->wb.X += (REAL)dx; c->wb.Y += (REAL)dy; }
        if (c->kind == OBJ_GROUP) remapSubtree(*c, dx, dy);     // 内层组跟着一起挪
        else if (!c->wbOk) { /* 保持无效，下次重算 */ }
    }
}

// =============================================================================
//  ★ 脏矩形：往表面清单里加伤害区
// =============================================================================
std::vector<int64_t> g_dmgVisiting;      // 画布互相贴的时候防无限递归

void propagateCanvas(Obj& cv, const Gdiplus::RectF* srcRect);

void addDamage(Obj& surf, const Gdiplus::RectF& r0) {
    if (surf.fullDamage) return;
    int sw = 0, sh = 0;
    surfSize(surf, &sw, &sh);
    if (sw <= 0 || sh <= 0) return;
    Gdiplus::RectF r = inflateRect(r0, 2);        // 抗锯齿/描边的边也要重画
    rectIntersect(r, Gdiplus::RectF(0, 0, (REAL)sw, (REAL)sh));
    if (r.Width <= 0 || r.Height <= 0) return;    // 完全在表面之外
    for (size_t i = 0; i < surf.damage.size(); i++) {
        if (rectsOverlap(surf.damage[i], r)) { rectUnion(surf.damage[i], r); goto notify; }
    }
    surf.damage.push_back(r);
    if (surf.damage.size() > 12) {                // 太碎了：还整面画省事
        surf.fullDamage = true;
        surf.damage.clear();
    } else {
        double area = 0;
        for (size_t i = 0; i < surf.damage.size(); i++)
            area += (double)surf.damage[i].Width * (double)surf.damage[i].Height;
        if (area > 0.6 * (double)sw * (double)sh) { surf.fullDamage = true; surf.damage.clear(); }
    }
notify:
    if (surf.kind == OBJ_WINDOW && surf.hwnd) {
        RECT rc;
        rc.left   = (LONG)std::floor(r.X);
        rc.top    = (LONG)std::floor(r.Y);
        rc.right  = (LONG)std::ceil((double)r.X + r.Width);
        rc.bottom = (LONG)std::ceil((double)r.Y + r.Height);
        InvalidateRect(surf.hwnd, &rc, FALSE);
    }
}
void damageSurfaceRect(Obj* surf, const Gdiplus::RectF& r) {
    if (!surf) return;
    addDamage(*surf, r);
    if (surf->kind == OBJ_CANVAS) {
        surf->dirty = true;                        // props().dirty 跟着走
        propagateCanvas(*surf, &r);
    }
}
void damageLocalRect(Obj* item, const Gdiplus::RectF& local) {
    if (!item) return;
    Obj* surf = rootSurface(item);
    if (!surf) return;
    Gdiplus::RectF w;
    if (!toSurfaceRect(*item, local, &w)) return;
    damageSurfaceRect(surf, w);
}
// 画布内容变了 → 所有贴了它的图形也要重画（矩形按贴图缩放映射过去）
void propagateCanvas(Obj& cv, const Gdiplus::RectF* srcRect) {
    int64_t cvId = idOfObj(&cv);
    for (size_t i = 0; i < g_dmgVisiting.size(); i++)
        if (g_dmgVisiting[i] == cvId) return;                  // 已经在这条链上了
    g_dmgVisiting.push_back(cvId);
    double cw = (double)cv.cw, chh = (double)cv.ch;
    for (size_t i = 0; i < g_objs.size(); i++) {               // 用下标：递归里可能 push_back
        Obj& o = g_objs[i];
        if (!o.alive || o.kind != OBJ_SHAPE || o.sh.kind != SK_IMAGE) continue;
        if (o.sh.srcId != cvId) continue;
        if (o.sh.w <= 0 || o.sh.h <= 0) continue;
        Gdiplus::RectF dst;
        if (srcRect && cw > 0 && chh > 0) {
            double sx = (double)o.sh.w / cw, sy = (double)o.sh.h / chh;
            dst = Gdiplus::RectF((REAL)((double)o.sh.x + (double)srcRect->X * sx),
                                 (REAL)((double)o.sh.y + (double)srcRect->Y * sy),
                                 (REAL)((double)srcRect->Width * sx),
                                 (REAL)((double)srcRect->Height * sy));
        } else {
            dst = Gdiplus::RectF((REAL)o.sh.x, (REAL)o.sh.y, (REAL)o.sh.w, (REAL)o.sh.h);
        }
        damageLocalRect(&o, dst);
    }
    g_dmgVisiting.pop_back();
}
void damageAll(Obj* o) {
    if (!o) return;
    Obj* surf = rootSurface(o);
    if (!surf) return;
    surf->fullDamage = true;
    surf->damage.clear();
    if (surf->kind == OBJ_CANVAS) {
        surf->dirty = true;
        propagateCanvas(*surf, nullptr);
    } else if (surf->hwnd) {
        InvalidateRect(surf->hwnd, nullptr, FALSE);
    }
}
void damageItemEx(Obj* o, bool allowRemap) {
    if (!o) return;
    if (o->kind == OBJ_WINDOW || o->kind == OBJ_CANVAS || o->kind == OBJ_IMAGE) { damageAll(o); return; }
    Obj* surf = rootSurface(o);
    if (!surf) return;
    Gdiplus::RectF oldR;
    bool had = o->wbOk;
    if (had) oldR = o->wb;
    double px = o->wbX, py = o->wbY, pa = o->wbA, ps = o->wbS;
    o->wbOk = false;                       // ★ 属性已经改了，缓存必须重算（否则新旧一样）
    Gdiplus::RectF newR;
    bool has = itemWorldBounds(*o, &newR);
    if (had) damageSurfaceRect(surf, oldR);
    if (has) damageSurfaceRect(surf, newR);
    // 组自己的变换变了 → 子孙缓存的包围盒跟着处理
    if (o->kind == OBJ_GROUP) {
        bool sameRS = (o->sh.angle == pa && o->sh.scale == ps);
        if (allowRemap && had && has && sameRS) remapSubtree(*o, o->sh.x - px, o->sh.y - py);
        else                                    invalidateSubtree(*o);
    }
    // 祖先组的包围盒（= 子图形的并集）也跟着变了：先把它算成伤害区，再就地重算
    Obj* p = objAt(o->sh.winId);
    int guard = 0;
    while (p && p->kind == OBJ_GROUP && guard++ < 64) {
        Gdiplus::RectF oa;
        bool h2 = p->wbOk;
        if (h2) oa = p->wb;
        p->wbOk = false;                                  // 强制重算（子集合变了）
        Gdiplus::RectF na;
        bool n2 = itemWorldBounds(*p, &na);
        if (h2) damageSurfaceRect(surf, oa);
        if (n2) damageSurfaceRect(surf, na);
        p = objAt(p->sh.winId);
    }
}
void damageItem(Obj* o) { damageItemEx(o, true); }

// =============================================================================
//  ★ 绘制：只画给定的（脏）区域
// =============================================================================
// 这个条目要不要画（没给区域 = 全画；给了 = 包围盒相交才画）
bool itemNeeded(Obj& o, const Gdiplus::RectF* clip) {
    if (!clip) return true;
    Gdiplus::RectF b;
    if (!itemWorldBounds(o, &b)) return false;      // 这里的 clip 是【表面坐标】
    return rectsOverlap(b, *clip);
}
int64_t g_paintGen = 0;                    // 一次重画的编号（统计"跳过"时不重复计）
// ★ 画一个表面里的条目；遇到组就递归（组的变换/裁剪作用于整组）
//   clip = 需要重画的区域（表面坐标；nullptr = 全画）；不相交的子树整棵跳过
void drawItems(Gdiplus::Graphics& g, Obj& surf, const Gdiplus::RectF* clip, PaintStats* st) {
    for (Obj* it : shapesInDrawOrder(surf, true)) {
        if (it->kind == OBJ_GROUP) {
            if (!itemNeeded(*it, clip)) {                 // 整组都在脏区外面 → 一个都不画
                if (st && it->dmgStamp != st->gen) {      // ★ 多个脏区时不重复统计
                    it->dmgStamp = st->gen;
                    st->skipped += countLeaves(*it);
                }
                continue;
            }
            GraphicsState gs = g.Save();
            g.TranslateTransform((REAL)it->sh.x, (REAL)it->sh.y);   // ★ 组的 x/y = 整组平移
            applyShapeTransform(g, it->sh, it);
            applyShapeClip(g, it->sh);
            drawItems(g, *it, clip, st);
            g.Restore(gs);
        } else {
            if (!itemNeeded(*it, clip)) {
                if (st && it->dmgStamp != st->gen) {
                    it->dmgStamp = st->gen;
                    st->skipped += 1;
                }
                continue;
            }
            drawShape(g, it->sh);
            if (st) st->drawn += 1;
        }
    }
}
// 把待重画的区域画进缓冲（窗口 → DIB；画布 → 位图）
bool flushSurface(Obj& surf) {
    if (!ensureGdiplus()) return false;
    int sw = 0, sh = 0;
    surfSize(surf, &sw, &sh);
    if (sw <= 0 || sh <= 0) return false;
    if (!surf.fullDamage && surf.damage.empty()) return false;      // 没有脏区，什么都不用干

    // 想要一张能画的 Graphics（窗口用离屏缓冲，画布用自己的位图）
    Gdiplus::Graphics* gp = nullptr;
    Gdiplus::Bitmap*   own = nullptr;
    if (surf.kind == OBJ_WINDOW) {
        if (!ensureWindowBuffer(surf)) return false;
        gp = new Gdiplus::Graphics(surf.memDC);
    } else {
        if (!surf.bmp || (int)surf.bmp->GetWidth() != sw || (int)surf.bmp->GetHeight() != sh) {
            delete surf.bmp;
            surf.bmp = new Gdiplus::Bitmap(sw, sh, PixelFormat32bppARGB);
            surf.fullDamage = true;
        }
        own = surf.bmp;
        gp = new Gdiplus::Graphics(own);
    }
    Gdiplus::Graphics& g = *gp;

    bool wasFull = surf.fullDamage || surf.damage.empty();
    std::vector<Gdiplus::RectF> rects;
    if (wasFull) rects.push_back(Gdiplus::RectF(0, 0, (REAL)sw, (REAL)sh));
    else         rects.swap(surf.damage);

    int64_t drawn = 0, skipped = 0;
    const int64_t gen = ++g_paintGen;          // ★ 这次重画的编号（跨多个脏区去重）
    Gdiplus::RectF acc(0, 0, 0, 0);
    for (size_t i = 0; i < rects.size(); i++) {
        Gdiplus::RectF r = rects[i];
        rectIntersect(r, Gdiplus::RectF(0, 0, (REAL)sw, (REAL)sh));
        if (r.Width <= 0 || r.Height <= 0) continue;
        if (i == 0) acc = r; else rectUnion(acc, r);
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);      // 刷底/清空要精确到像素
        if (surf.kind == OBJ_CANVAS) {
            // 画布是透明底：把这块清成全透明（SourceCopy 会把 alpha 一起写下去）
            Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.FillRectangle(&clear, r);
            g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        } else {
            Gdiplus::SolidBrush bg(Gdiplus::Color(fromColorRef(GetSysColor(COLOR_WINDOW))));
            g.FillRectangle(&bg, r);
        }
        Gdiplus::GraphicsState gs = g.Save();
        g.SetClip(r, Gdiplus::CombineModeIntersect);
        PaintStats ps;
        ps.gen = gen;                          // 同一帧的多个脏区共享一个编号
        drawItems(g, surf, &r, &ps);
        g.Restore(gs);
        drawn += ps.drawn;
        skipped += ps.skipped;
    }
    surf.damage.clear();
    surf.fullDamage = false;
    surf.dirty = false;
    surf.paints++;
    surf.lastRects = (int64_t)rects.size();
    surf.lastDrawn = drawn;
    surf.lastSkipped = skipped;
    surf.lastFull = wasFull;
    surf.lastRegion = acc;
    delete gp;                                             // Bitmap 是 surf 自己的，别删
    return true;
}

// ── 窗口的离屏缓冲（DIB）────────────────────────────────────────────────────
void releaseWindowBuffer(Obj& win) {
    if (win.memDC && win.dibOld) SelectObject(win.memDC, win.dibOld);
    if (win.dib) DeleteObject(win.dib);
    if (win.memDC) DeleteDC(win.memDC);
    win.memDC = nullptr; win.dib = nullptr; win.dibOld = nullptr; win.dibBits = nullptr;
    win.bufW = win.bufH = 0;
}
bool ensureWindowBuffer(Obj& win) {
    int w = 0, h = 0;
    surfSize(win, &w, &h);
    if (w <= 0 || h <= 0) return false;
    if (win.dib && win.memDC && win.bufW == w && win.bufH == h) return true;
    releaseWindowBuffer(win);
    HDC screen = GetDC(nullptr);
    win.memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;      // ★ 负数 = 自上而下：内存第一行就是屏幕第一行
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    win.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (win.memDC && win.dib) {
        win.dibOld = SelectObject(win.memDC, win.dib);
        win.dibBits = bits;
        win.bufW = w;
        win.bufH = h;
        win.fullDamage = true;            // 新缓冲 → 下次整面画一遍
        return true;
    }
    releaseWindowBuffer(win);
    return false;
}
// 把缓冲贴到窗口（只贴这次的更新区，包围盒就够 —— 缓冲里永远是一张完整画面）
void blitWindowBuffer(Obj& win, HDC hdc) {
    if (!win.memDC || !win.dib) return;
    RECT ur;
    if (!GetUpdateRect(win.hwnd, &ur, FALSE)) {
        RECT cr;                                        // 更新区是空？那就整块贴上（保险）
        if (!GetClientRect(win.hwnd, &cr)) return;
        ur = cr;
    }
    int x = (int)ur.left, y = (int)ur.top;
    int w = (int)(ur.right - ur.left), h = (int)(ur.bottom - ur.top);
    if (w <= 0 || h <= 0) return;
    BitBlt(hdc, x, y, w, h, win.memDC, x, y, SRCCOPY);
}
// 从缓冲里读一个像素（先把待重画的画完，保证读到的就是屏幕上那张画）
bool readSurfacePixel(Obj& surf, int x, int y, Argb* out) {
    int sw = 0, sh = 0;
    surfSize(surf, &sw, &sh);
    if (x < 0 || y < 0 || x >= sw || y >= sh) return false;
    flushSurface(surf);                                  // 只画脏区（第一次是整面）
    if (surf.kind == OBJ_WINDOW) {
        if (!surf.dibBits && !ensureWindowBuffer(surf)) return false;
        if (!surf.dibBits) return false;
        flushSurface(surf);
        if (!surf.dibBits) return false;
        const uint32_t* px = (const uint32_t*)surf.dibBits;
        uint32_t v = px[(size_t)y * (size_t)surf.bufW + (size_t)x];   // 内存序 = BGRA
        *out = (Argb)(v & 0x00FFFFFFu);                  // 和以前一样只给 RGB
        return true;
    }
    if (!surf.bmp) { flushSurface(surf); if (!surf.bmp) return false; }
    Gdiplus::Color c;
    if (surf.bmp->GetPixel((INT)x, (INT)y, &c) != Gdiplus::Ok) return false;
    *out = ((Argb)c.GetR() << 16) | ((Argb)c.GetG() << 8) | (Argb)c.GetB();   // 只给 RGB
    return true;
}

// 客户区尺寸
void clientSize(Obj& win, int* w, int* h) {
    RECT cr;
    GetClientRect(win.hwnd, &cr);
    *w = (int)(cr.right - cr.left);
    *h = (int)(cr.bottom - cr.top);
}

// COLORREF(0x00BBGGRR) → GDI+ ARGB(0xAARRGGBB)：位序和 alpha 都得转，
// 直接 Color(GetSysColor(...)) 会得到 alpha=0 的"全透明色"（背景变透明黑）
Argb fromColorRef(COLORREF c) {
    return 0xFF000000u | ((Argb)GetRValue(c) << 16) | ((Argb)GetGValue(c) << 8) | (Argb)GetBValue(c);
}

// ★ 窗口重画：把待重画的画进离屏缓冲，再把缓冲贴到屏幕 —— 不再直接画在屏幕 DC 上
//   （改一个小属性只重画那一小块，而且全程不闪）
void paintWindow(Obj& win, HDC hdc) {
    int w = 0, h = 0;
    clientSize(win, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (!ensureWindowBuffer(win)) return;
    flushSurface(win);
    blitWindowBuffer(win, hdc);
}

// ★ 画布：按需把它的图形渲染进自己的位图（脏了才重画 = 缓存；现在只重画脏的那块）
void ensureCanvasRendered(Obj& cv) {
    if (cv.rendering) return;                       // 防自环（画布里贴了它自己）
    if (!cv.fullDamage && cv.damage.empty() && cv.bmp) return;
    cv.rendering = true;
    flushSurface(cv);
    cv.rendering = false;
}

// ── 创建图形：统一入口 ──────────────────────────────────────────────────────：第 1 个参数是窗口 id，后面按种类取
int64_t createShape(AeVM* vm, AeValue* argv, int argc, const char* fn, ShapeKind kind,
                    int numStart, int numCount, std::vector<double> nums, bool hasPoints) {
    // ★ 只拿窗口 id，不拿 Obj*：下面 g_objs.push_back 可能让 vector 重新分配，
    //   之前取到的指针会悬空 —— 这类 use-after-free 的表现是"图形偶发画不出来"，
    //   极难查，所以这里一律按 id 重新取。
    int64_t winId = aelib::argInt(vm, argv, argc, 0, fn);
    if (!surfOf(vm, winId, fn)) return 0;        // ★ 窗口或画布都行
    if (!ensureGdiplus()) return 0;      // 原因已记录在 lastError

    g_objs.push_back(Obj());
    Obj& o = g_objs.back();
    o.kind = OBJ_SHAPE;
    o.sh.kind = kind;
    o.sh.winId = winId;
    o.sh.seq = ++g_seq;
    // 点/线的默认外观更合用一点：点是实心小圆点
    if (kind == SK_POINT) { o.sh.fill = true; o.sh.stroke = 0; o.sh.r = 2; }
    if (kind == SK_LINE)  { o.sh.stroke = 1; o.sh.r = 0; }

    if (hasPoints) {
        if (argc < 2 || !ae_is_table(argv[1])) {
            aelib::raise(vm, std::string(fn) + " 的第 2 个参数需要点数组 {x1,y1,x2,y2,...}");
            g_objs.pop_back();
            return 0;
        }
        if (!readPoints(vm, argv[1], &o.sh.pts, fn)) { g_objs.pop_back(); return 0; }
    } else {
        for (int i = 0; i < numCount; i++)
            nums.push_back(argNum(vm, argv[numStart + i], fn, "坐标/尺寸", numStart + i + 1));
        if (kind == SK_POINT)   { o.sh.x = nums[0]; o.sh.y = nums[1]; }
        if (kind == SK_LINE)    { o.sh.x = nums[0]; o.sh.y = nums[1]; o.sh.x2 = nums[2]; o.sh.y2 = nums[3]; }
        if (kind == SK_RECT)    { o.sh.x = nums[0]; o.sh.y = nums[1]; o.sh.w = nums[2]; o.sh.h = nums[3]; }
        if (kind == SK_CIRCLE)  { o.sh.x = nums[0]; o.sh.y = nums[1]; o.sh.r = nums[2]; }
        if (kind == SK_ELLIPSE) { o.sh.x = nums[0]; o.sh.y = nums[1]; o.sh.rx = nums[2]; o.sh.ry = nums[3]; }
    }
    int64_t id = (int64_t)g_objs.size();      // 刚 push 进来的就是最后一个
    Obj* win = objAt(winId);                  // ★ 重新取（上面的指针可能已失效）
    if (win) {
        win->shapes.push_back(id);
        invalidateSurface(win);
    }
    return id;
}

// =============================================================================
//  创建图形：对外的 7 个函数
// =============================================================================
static int nat_point(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.point", SK_POINT, 1, 2, {}, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}
static int nat_line(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.line", SK_LINE, 1, 4, {}, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}
static int nat_poly(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.poly", SK_POLY, 1, 0, {}, true);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}
static int nat_rect(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.rect", SK_RECT, 1, 4, {}, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}
static int nat_roundRect(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.roundRect", SK_RECT, 1, 4, {}, false);
    if (!id) { ae_push_null(vm); return 1; }
    Obj* o = objAt(id);
    o->sh.r = argNum(vm, argv[5], "ui.roundRect", "圆角", 6);      // 统一圆角；四角不同用 setAll
    invalidateOfShape(o);
    ae_push_int(vm, id);
    return 1;
}
static int nat_circle(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.circle", SK_CIRCLE, 1, 3, {}, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}
static int nat_ellipse(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = createShape(vm, argv, argc, "ui.ellipse", SK_ELLIPSE, 1, 4, {}, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}

// text(win, x, y, 内容) → id：文字也是图形对象（位置/颜色/字号/对齐都能随时改）
static int nat_text(AeVM* vm, int argc, AeValue* argv) {
    int64_t winId = aelib::argInt(vm, argv, argc, 0, "ui.text");
    if (!surfOf(vm, winId, "ui.text")) return 0;   // ★ 窗口/画布/组都行
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    double x = argNum(vm, argv[1], "ui.text", "x", 2);
    double y = argNum(vm, argv[2], "ui.text", "y", 3);
    const std::string& txt = aelib::argStr(vm, argv, argc, 3, "ui.text");

    g_objs.push_back(Obj());                    // ★ 之后不要再持有旧 Obj*（会失效）
    Obj& o = g_objs.back();
    o.kind = OBJ_SHAPE;
    o.sh.kind = SK_TEXT;
    o.sh.winId = winId;
    o.sh.seq = ++g_seq;
    o.sh.x = x;
    o.sh.y = y;
    o.sh.text = txt;
    o.sh.font = kDefaultFont;
    o.sh.size = 16;                             // 字号单位：像素
    o.sh.color = 0xFF000000;
    o.sh.stroke = 0;                            // 文字没有描边概念
    int64_t id = (int64_t)g_objs.size();
    Obj* win = objAt(winId);
    if (win) { win->shapes.push_back(id); invalidate(win); }
    ae_push_int(vm, id);
    return 1;
}

// textSize(id) → {w, h}：文字框的实际尺寸（对齐时要用它）
static int nat_textSize(AeVM* vm, int argc, AeValue* argv) {
    Obj* o = shapeOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.textSize"), "ui.textSize");
    if (!o) return 0;
    if (o->sh.kind != SK_TEXT) {
        aelib::raise(vm, std::string("ui.textSize：只有文字有尺寸可量（当前是 ") + kindName(o->sh.kind) + "）");
        return 0;
    }
    if (ae_is_str(argv[1])) {                   // 可选第 2 参数：临时按另一段内容量
        size_t n = 0;
        AeValue v;
        (void)v;
        const char* p = ae_to_str(argv[1], &n);
        Shape tmp = o->sh;
        tmp.text = std::string(p, n);
        if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
        Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&bmp);
        Font* font = nullptr;
        Gdiplus::RectF box;
        textBox(g, tmp, &box, &font);
        AeValue t = ae_new_table(vm);
        ae_table_set(vm, t, ae_make_str(vm, "w"), ae_int_value((int64_t)llround(box.Width)));
        ae_table_set(vm, t, ae_make_str(vm, "h"), ae_int_value((int64_t)llround(box.Height)));
        ae_push_table(vm, t);
        return 1;
    }
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    Font* font = nullptr;
    Gdiplus::RectF box;
    textBox(g, o->sh, &box, &font);
    AeValue tb = ae_new_table(vm);
    ae_table_set(vm, tb, ae_make_str(vm, "w"), ae_int_value((int64_t)llround(box.Width)));
    ae_table_set(vm, tb, ae_make_str(vm, "h"), ae_int_value((int64_t)llround(box.Height)));
    ae_push_table(vm, tb);
    return 1;
}

// =============================================================================
//  属性读写：set / setAll / get / props
// =============================================================================
static int nat_set(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.set");
    const std::string& name = aelib::argStr(vm, argv, argc, 1, "ui.set");
    Obj* o = objAt(id);
    if (!o) { objOf(vm, id, "ui.set"); return 0; }

    if (o->kind == OBJ_SHAPE || o->kind == OBJ_GROUP) {     // ★ 组也有全套图形属性
        if (!setShapeProp(vm, o, name, argv[2], "ui.set", 3)) return 0;
        invalidateOfShape(o);
        ae_push_int(vm, id);
        return 1;
    }
    // 窗口也支持同一套写法：title / x / y / w / h / visible / resizable / maximizable
    HWND h = o->hwnd;
    if (name == "title") {
        const std::string& t = aelib::argStr(vm, argv, argc, 2, "ui.set");
        SetWindowTextW(h, aelib::utf8ToUtf16(t).c_str());
        invalidate(o);
    } else if (name == "x" || name == "y") {
        RECT r; GetWindowRect(h, &r);
        double v = argNum(vm, argv[2], "ui.set", name.c_str(), 3);
        SetWindowPos(h, nullptr, name == "x" ? (int)v : r.left, name == "y" ? (int)v : r.top,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (name == "w" || name == "h") {
        RECT cr; GetClientRect(h, &cr);
        double v = argNum(vm, argv[2], "ui.set", name.c_str(), 3);
        int cw = name == "w" ? (int)v : (int)(cr.right - cr.left);
        int ch = name == "h" ? (int)v : (int)(cr.bottom - cr.top);
        int ow = 0, oh = 0;
        clientToWindowSize(h, cw, ch, &ow, &oh);
        SetWindowPos(h, nullptr, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        invalidate(o);
    } else if (name == "visible") {
        if (!(ae_is_bool(argv[2]) || ae_is_int(argv[2])))
            { aelib::raise(vm, "ui.set 设置 visible 需要 bool"); return 0; }
        ShowWindow(h, argv[2].i ? SW_SHOW : SW_HIDE);
    } else if (name == "frameless") {
        if (!(ae_is_bool(argv[2]) || ae_is_int(argv[2])))
            { aelib::raise(vm, "ui.set 设置 frameless 需要 bool"); return 0; }
        bool on = argv[2].i != 0;
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        if (on) st &= ~(LONG_PTR)(WS_CAPTION | WS_SYSMENU);
        else    st |= (LONG_PTR)(WS_CAPTION | WS_SYSMENU);
        SetWindowLongPtrW(h, GWL_STYLE, st);
        o->frameless = on;
        redrawFrame(h);
    } else if (name == "cursor") {
        const std::string& c = aelib::argStr(vm, argv, argc, 2, "ui.set");
        if (!validCursorName(c)) {
            aelib::raise(vm, "ui.set 不认识的鼠标形状 '" + c + "'；可用：" + kCursorNames +
                             "（空串 = 系统箭头）");
            return 0;
        }
        o->cursorName = c;
        o->curCursor  = c;
        if (loadCursorByName(c)) applyCursor(*o);
    } else if (name == "resizable" || name == "maximizable") {
        if (!(ae_is_bool(argv[2]) || ae_is_int(argv[2])))
            { aelib::raise(vm, "ui.set 设置 " + name + " 需要 bool"); return 0; }
        bool on = argv[2].i != 0;
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        LONG_PTR mask = (name == "resizable") ? WS_THICKFRAME : WS_MAXIMIZEBOX;
        if (on) st |= mask; else st &= ~mask;
        SetWindowLongPtrW(h, GWL_STYLE, st);
        redrawFrame(h);
    } else {
        aelib::raise(vm, "ui.set 不认识窗口属性 '" + name +
                         "'；窗口可用：title x y w h visible resizable maximizable frameless cursor");
        return 0;
    }
    ae_push_int(vm, id);
    return 1;
}

static int nat_setAll(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.setAll");
    Obj* o = objAt(id);
    if (!o) { objOf(vm, id, "ui.setAll"); return 0; }
    if (argc < 2 || !ae_is_table(argv[1])) {
        aelib::raise(vm, "ui.setAll 的第 2 个参数需要表，例如 {x = 10, fill = true}");
        return 0;
    }
    // 只按属性表里已知的名字去取：不用遍历表的键，行为也更可预测
    for (int i = 0; i < kPropCount + kTextPropCount; i++) {
        const char* pname = (i < kPropCount) ? kProps[i].name : kTextProps[i - kPropCount].name;
        AeValue v;
        if (!ae_table_get(vm, argv[1], pname, &v)) continue;
        if (o->kind == OBJ_SHAPE || o->kind == OBJ_GROUP) {
            if (!setShapeProp(vm, o, pname, v, "ui.setAll", 2)) return 0;
        } else {
            // 窗口：借用 set 的分支（重建一个 3 参数调用太重，这里直接内联常用项）
            std::string nm = pname;
            if (i >= kPropCount) {
                aelib::raise(vm, "ui.setAll：" + nm + " 只有文字有（文字用 ui.text 创建）");
                return 0;
            }
            if (nm == "x" || nm == "y" || nm == "w" || nm == "h") {
                if (!ae_is_num(v)) { aelib::raise(vm, "ui.setAll 的 " + nm + " 需要数值"); return 0; }
                RECT wr; GetWindowRect(o->hwnd, &wr);
                RECT cr; GetClientRect(o->hwnd, &cr);
                double d = ae_is_int(v) ? (double)v.i : v.f;
                if (nm == "x" || nm == "y") {
                    SetWindowPos(o->hwnd, nullptr, nm == "x" ? (int)d : wr.left, nm == "y" ? (int)d : wr.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else {
                    int cw = nm == "w" ? (int)d : (int)(cr.right - cr.left);
                    int ch = nm == "h" ? (int)d : (int)(cr.bottom - cr.top);
                    int ow = 0, oh = 0;
                    clientToWindowSize(o->hwnd, cw, ch, &ow, &oh);
                    SetWindowPos(o->hwnd, nullptr, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else if (nm == "visible") {
                ShowWindow(o->hwnd, (ae_is_bool(v) || ae_is_int(v)) && v.i ? SW_SHOW : SW_HIDE);
            }
        }
    }
    // 窗口的 title 不在属性表里（形状才有名字冲突），单独处理
    if (o->kind == OBJ_WINDOW) {
        std::string t;
        if (ae_table_str(vm, argv[1], "title", &t))
            SetWindowTextW(o->hwnd, aelib::utf8ToUtf16(t).c_str());
        invalidate(o);
    } else {
        invalidateOfShape(o);
    }
    ae_push_int(vm, id);
    return 1;
}

static int nat_get(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.get");
    const std::string& name = aelib::argStr(vm, argv, argc, 1, "ui.get");
    Obj* o = objAt(id);
    if (!o) { objOf(vm, id, "ui.get"); return 0; }

    if (o->kind == OBJ_CANVAS) {
        if (name == "kind") { ae_push_str(vm, "canvas"); return 1; }
        if (name == "w")    { ae_push_int(vm, o->cw); return 1; }
        if (name == "h")    { ae_push_int(vm, o->ch); return 1; }
        aelib::raise(vm, "ui.get 画布只支持 kind / w / h");
        return 0;
    }
    if (o->kind == OBJ_IMAGE) {
        if (name == "kind") { ae_push_str(vm, "image"); return 1; }
        if (name == "path") { ae_push_str(vm, o->imgPath); return 1; }
        if (name == "w")    { ae_push_int(vm, o->img ? (int64_t)o->img->GetWidth() : 0); return 1; }
        if (name == "h")    { ae_push_int(vm, o->img ? (int64_t)o->img->GetHeight() : 0); return 1; }
        aelib::raise(vm, "ui.get 图片只支持 kind / path / w / h");
        return 0;
    }
    if (o->kind == OBJ_WINDOW) {
        HWND h = o->hwnd;
        RECT wr; GetWindowRect(h, &wr);
        RECT cr; GetClientRect(h, &cr);
        if (name == "title")     { ae_push_str(vm, aelib::utf16ToUtf8(winTitle(h))); return 1; }
        if (name == "x")         { ae_push_int(vm, wr.left); return 1; }
        if (name == "y")         { ae_push_int(vm, wr.top); return 1; }
        if (name == "w")         { ae_push_int(vm, cr.right - cr.left); return 1; }
        if (name == "h")         { ae_push_int(vm, cr.bottom - cr.top); return 1; }
        if (name == "visible")   { return retBool(vm, IsWindowVisible(h) != 0); }
        if (name == "resizable") { return retBool(vm, (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME) != 0); }
        if (name == "maximizable") { return retBool(vm, (GetWindowLongPtrW(h, GWL_STYLE) & WS_MAXIMIZEBOX) != 0); }
        if (name == "kind")      { ae_push_str(vm, "window"); return 1; }
        if (name == "cursor")    { ae_push_str(vm, o->cursorName); return 1; }
        if (name == "frameless") { return retBool(vm, o->frameless); }
        aelib::raise(vm, "ui.get 不认识窗口属性 '" + name +
                         "'；窗口可用：title x y w h visible resizable maximizable frameless cursor"
                         "（kind 只读，返回 \"window\"）");
        return 0;
    }

    Shape& s = o->sh;
    if (name == "cmds") {                      // 路径指令流读回来（只有 path 有）
        AeValue t = ae_new_table(vm);
        for (size_t i = 0; i < s.cmds.size(); i++) {
            double d = s.cmds[i];
            AeValue pv = ae_int_value(0);
            if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) pv = ae_int_value((int64_t)d);
            else { pv.kind = AE_FLOAT; pv.f = d; }
            ae_table_set(vm, t, ae_int_value((int64_t)i + 1), pv);
        }
        ae_push_table(vm, t);
        return 1;
    }
    if (name == "points") {
        if (s.kind != SK_POLY) {                    // 和 ui.set 一致：只有 poly 有 points
            aelib::raise(vm, std::string("ui.get：只有 poly 有 points 属性（当前是 ") + kindName(s.kind) + "）");
            return 0;
        }
        AeValue t = ae_new_table(vm);
        for (size_t i = 0; i < s.pts.size(); i++)
            { double d = s.pts[i]; AeValue pv; if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) pv = ae_int_value((int64_t)d); else { pv = ae_int_value(0); pv.kind = AE_FLOAT; pv.f = d; } ae_table_set(vm, t, ae_int_value((int64_t)i + 1), pv); }
        ae_push_table(vm, t);
        return 1;
    }
    if (name == "anchorAuto") { ae_push_bool(vm, s.hasAnchor ? 0 : 1); return 1; }
    if (name == "kind") { ae_push_str(vm, kindName(s.kind)); return 1; }
    if (name == "win")  { ae_push_int(vm, s.winId); return 1; }
    if (name == "id")   { ae_push_int(vm, id); return 1; }
    {
        std::string why = propCheck(s.kind, name);
        if (!why.empty()) {
            aelib::raise(vm, "ui.get：" + why + "；" +
                             (s.kind == SK_TEXT ? std::string("文字可用：") + kTextPropNames
                                                : std::string("可用：") + kPropNamesAll));
            return 0;
        }
    }
    if (const TextPropDef* td = findTextProp(name)) {
        switch (td->type) {
            case TP_STR:   ae_push_str(vm, s.*(td->sval)); break;
            case TP_NUM:   pushNum(vm, s.*(td->num)); break;
            case TP_BOOL:  ae_push_bool(vm, s.*(td->bl) ? 1 : 0); break;
            case TP_COLOR: ae_push_str(vm, colorToString(s.*(td->col))); break;
            case TP_HALIGN: ae_push_str(vm, hAlignName(s.*(td->al))); break;
            case TP_VALIGN: ae_push_str(vm, vAlignName(s.*(td->al))); break;
        }
        return 1;
    }
    const PropDef* d = findProp(name);
    if (!d) {
        aelib::raise(vm, "ui.get 不认识属性 '" + name + "'；可用：" +
                         (s.kind == SK_TEXT ? kTextPropNames : kPropNamesAll));
        return 0;
    }
    switch (d->type) {
        case PT_NUM:   pushNum(vm, s.*(d->num)); break;
        case PT_BOOL:  ae_push_bool(vm, s.*(d->bl) ? 1 : 0); break;
        case PT_COLOR: ae_push_str(vm, colorToString(s.*(d->col))); break;
        case PT_STR:   ae_push_str(vm, s.*(d->sval)); break;
    }
    return 1;
}

static int nat_props(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.props");
    Obj* o = objAt(id);
    if (!o) { objOf(vm, id, "ui.props"); return 0; }
    AeValue t = ae_new_table(vm);
    auto set = [&](const char* k, AeValue v) { ae_table_set(vm, t, ae_make_str(vm, k), v); };
    set("id", ae_int_value(id));
    if (o->kind == OBJ_GROUP) {
        Shape& g = o->sh;
        set("kind", ae_make_str(vm, "group"));
        set("win", ae_int_value(g.winId));
        set("x", ae_num_value(g.x));
        set("y", ae_num_value(g.y));
        set("angle", ae_num_value(g.angle));
        set("scale", ae_num_value(g.scale));
        set("visible", ae_bool_value(g.visible));
        set("z", ae_num_value(g.z));
        set("clipX", ae_int_value((int64_t)llround(g.clipX)));
        set("clipY", ae_int_value((int64_t)llround(g.clipY)));
        set("clipW", ae_int_value((int64_t)llround(g.clipW)));
        set("clipH", ae_int_value((int64_t)llround(g.clipH)));
        set("shapes", ae_int_value((int64_t)o->shapes.size()));
        Gdiplus::RectF b;
        set("hasBounds", ae_bool_value(groupBounds(*o, &b)));
        ae_push_table(vm, t);
        return 1;
    }
    if (o->kind == OBJ_CANVAS) {
        set("kind", ae_make_str(vm, "canvas"));
        set("w", ae_int_value(o->cw));
        set("h", ae_int_value(o->ch));
        set("shapes", ae_int_value((int64_t)o->shapes.size()));
        set("dirty", ae_bool_value(o->dirty));
        ae_push_table(vm, t);
        return 1;
    }
    if (o->kind == OBJ_IMAGE) {
        set("kind", ae_make_str(vm, "image"));
        set("path", ae_make_str(vm, o->imgPath));
        set("w", ae_int_value(o->img ? (int64_t)o->img->GetWidth() : 0));
        set("h", ae_int_value(o->img ? (int64_t)o->img->GetHeight() : 0));
        ae_push_table(vm, t);
        return 1;
    }
    if (o->kind == OBJ_WINDOW) {
        HWND h = o->hwnd;
        RECT wr; GetWindowRect(h, &wr);
        RECT cr; GetClientRect(h, &cr);
        set("kind", ae_make_str(vm, "window"));
        set("title", ae_make_str(vm, aelib::utf16ToUtf8(winTitle(h))));
        set("x", ae_int_value(wr.left));
        set("y", ae_int_value(wr.top));
        set("w", ae_int_value(cr.right - cr.left));
        set("h", ae_int_value(cr.bottom - cr.top));
        set("visible", ae_bool_value(IsWindowVisible(h) != 0));
        set("resizable", ae_bool_value((GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME) != 0));
        set("maximizable", ae_bool_value((GetWindowLongPtrW(h, GWL_STYLE) & WS_MAXIMIZEBOX) != 0));
        ae_push_table(vm, t);
        return 1;
    }
    Shape& s = o->sh;
    set("kind", ae_make_str(vm, kindName(s.kind)));
    set("win", ae_int_value(s.winId));
    for (int i = 0; i < kPropCount; i++) {
        const PropDef& d = kProps[i];
        switch (d.type) {
            case PT_NUM: { AeValue nv; double dv = s.*(d.num); if (dv == std::floor(dv) && dv >= -9.0e15 && dv <= 9.0e15) nv = ae_int_value((int64_t)dv); else { nv = ae_int_value(0); nv.kind = AE_FLOAT; nv.f = dv; } set(d.name, nv); break; }
            case PT_BOOL:  set(d.name, ae_bool_value(s.*(d.bl) ? 1 : 0)); break;
            case PT_COLOR: set(d.name, ae_make_str(vm, colorToString(s.*(d.col)))); break;
            case PT_STR:   set(d.name, ae_make_str(vm, s.*(d.sval))); break;
        }
    }
    if (s.kind == SK_TEXT) {
        for (int i = 0; i < kTextPropCount; i++) {
            const TextPropDef& d = kTextProps[i];
            switch (d.type) {
                case TP_STR:   set(d.name, ae_make_str(vm, s.*(d.sval))); break;
                case TP_NUM:   { double dv = s.*(d.num); AeValue nv = ae_int_value(0);
                                 if (dv == std::floor(dv) && dv >= -9.0e15 && dv <= 9.0e15) nv = ae_int_value((int64_t)dv);
                                 else { nv.kind = AE_FLOAT; nv.f = dv; }
                                 set(d.name, nv); break; }
                case TP_BOOL:  set(d.name, ae_bool_value(s.*(d.bl) ? 1 : 0)); break;
                case TP_COLOR: set(d.name, ae_make_str(vm, colorToString(s.*(d.col)))); break;
                case TP_HALIGN: set(d.name, ae_make_str(vm, hAlignName(s.*(d.al)))); break;
                case TP_VALIGN: set(d.name, ae_make_str(vm, vAlignName(s.*(d.al)))); break;
            }
        }
        // 真正用上的字体（族名不存在时会兜底，这里能看到兜底成了什么）
        set("fontUsed", ae_make_str(vm, getFont(s)->used));
        Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics mg(&bmp);
        Font* fl = nullptr;
        Gdiplus::RectF bx;
        textBox(mg, s, &bx, &fl);
        set("tw", ae_int_value((int64_t)llround(bx.Width)));
        set("th", ae_int_value((int64_t)llround(bx.Height)));
        ae_push_table(vm, t);
        return 1;
    }
    set("anchorAuto", ae_bool_value(s.hasAnchor ? 0 : 1));
    if (s.kind == SK_PATH) {                      // 路径的指令流也读得回来
        AeValue ca = ae_new_table(vm);
        for (size_t i = 0; i < s.cmds.size(); i++) {
            double d = s.cmds[i];
            AeValue pv = ae_int_value(0);
            if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) pv = ae_int_value((int64_t)d);
            else { pv.kind = AE_FLOAT; pv.f = d; }
            ae_table_set(vm, ca, ae_int_value((int64_t)i + 1), pv);
        }
        set("cmds", ca);
    }
    AeValue pa = ae_new_table(vm);
    for (size_t i = 0; i < s.pts.size(); i++) {
        double d = s.pts[i];
        AeValue pv = ae_int_value(0);
        if (d == std::floor(d) && d >= -9.0e15 && d <= 9.0e15) pv = ae_int_value((int64_t)d);
        else { pv.kind = AE_FLOAT; pv.f = d; }
        ae_table_set(vm, pa, ae_int_value((int64_t)i + 1), pv);
    }
    set("points", pa);
    ae_push_table(vm, t);
    return 1;
}

// ── 画布 / 图片 / 贴图 ──────────────────────────────────────────────────────
//  canvas(w, h) → id：离屏画布。图形可以建在它上面（和窗口同一套 API），
//      它是【缓存】：脏了才重画；用 drawImage 贴到窗口时只贴一张位图。
static int nat_canvas(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    int64_t w = aelib::argInt(vm, argv, argc, 0, "ui.canvas");
    int64_t h = aelib::argInt(vm, argv, argc, 1, "ui.canvas");
    if (w <= 0 || h <= 0 || w > 32767 || h > 32767) {
        aelib::fail("ui.canvas：尺寸必须在 1..32767 之间，得到 " + std::to_string(w) + "×" + std::to_string(h));
        ae_push_null(vm);
        return 1;
    }
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    g_objs.push_back(Obj());
    Obj& o = g_objs.back();
    o.kind = OBJ_CANVAS;
    o.cw = (int)w;
    o.ch = (int)h;
    ae_push_int(vm, idOfObj(&o));
    return 1;
}

//  image("a.png") → id：从文件加载图片（打不开 → null + lastError，属预期失败）
static int nat_image(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    aelib::clearError();
    const std::string& p = aelib::argStr(vm, argv, argc, 0, "ui.image");
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    Gdiplus::Bitmap* b = new Gdiplus::Bitmap(aelib::utf8ToUtf16(p).c_str());
    if (!b || b->GetLastStatus() != Gdiplus::Ok) {
        delete b;
        aelib::fail("ui.image：打不开图片 '" + p + "'（路径不对，或格式不支持）");
        ae_push_null(vm);
        return 1;
    }
    g_objs.push_back(Obj());
    Obj& o = g_objs.back();
    o.kind = OBJ_IMAGE;
    o.img = b;
    o.imgPath = p;
    ae_push_int(vm, idOfObj(&o));
    return 1;
}

//  drawImage(目标, 来源, x, y[, w, h]) → 图形 id：把图片/画布贴到窗口或另一个画布上
//    w/h 不写就用来源的原始尺寸
static int nat_drawImage(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    int64_t dstId = aelib::argInt(vm, argv, argc, 0, "ui.drawImage");
    Obj* dst = surfOf(vm, dstId, "ui.drawImage");
    if (!dst) return 0;
    int64_t srcId = aelib::argInt(vm, argv, argc, 1, "ui.drawImage");
    Obj* src = objAt(srcId);
    if (!src || (src->kind != OBJ_IMAGE && src->kind != OBJ_CANVAS)) {
        aelib::raise(vm, "ui.drawImage 的第 2 个参数需要图片（ui.image）或画布（ui.canvas）的 id");
        return 0;
    }
    double x = argNum(vm, argv[2], "ui.drawImage", "x", 3);
    double y = argNum(vm, argv[3], "ui.drawImage", "y", 4);
    double w = 0, h = 0;
    if (argc > 4) w = argNum(vm, argv[4], "ui.drawImage", "w", 5);
    if (argc > 5) h = argNum(vm, argv[5], "ui.drawImage", "h", 6);
    if (src->kind == OBJ_IMAGE) {
        if (w <= 0) w = (double)src->img->GetWidth();
        if (h <= 0) h = (double)src->img->GetHeight();
    } else {
        if (w <= 0) w = (double)src->cw;
        if (h <= 0) h = (double)src->ch;
    }
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }

    bool srcIsCanvas = (src->kind == OBJ_CANVAS);
    g_objs.push_back(Obj());                        // ★ 之后不要再用旧指针
    Obj& o = g_objs.back();
    o.kind = OBJ_SHAPE;
    o.sh.kind = SK_IMAGE;
    o.sh.winId = dstId;
    o.sh.seq = ++g_seq;
    o.sh.srcId = srcId;
    o.sh.x = x; o.sh.y = y; o.sh.w = w; o.sh.h = h;
    int64_t id = (int64_t)g_objs.size();
    Obj* d = objAt(dstId);
    if (d) { d->shapes.push_back(id); invalidateSurface(d); }
    if (srcIsCanvas) {                              // 画布脏了要连累贴它的人重画
        Obj* s2 = objAt(srcId);
        if (s2) s2->users.push_back(dstId);
    }
    ae_push_int(vm, id);
    return 1;
}

// ── 路径：ui.path(win[, 指令数组]) + 追加器 ─────────────────────────────────
//   路径是【对象】：指令存在 cmds 属性里，能用 set/get/props 读写、能序列化。
//   追加器只是"往 cmds 里加几个数字"的糖，返回路径 id 便于连着写。
static int nat_path(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    int64_t winId = aelib::argInt(vm, argv, argc, 0, "ui.path");
    if (!surfOf(vm, winId, "ui.path")) return 0;   // ★ 窗口/画布/组都行
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    g_objs.push_back(Obj());
    Obj& o = g_objs.back();
    o.kind = OBJ_SHAPE; o.sh.kind = SK_PATH; o.sh.winId = winId; o.sh.seq = ++g_seq;
    if (argc > 1) {
        if (!ae_is_table(argv[1])) {
            aelib::raise(vm, "ui.path 的第 2 个参数需要指令数组，例如 {1,10,10, 2,100,10, 6}");
            g_objs.pop_back();
            return 0;
        }
        if (!readCmds(vm, argv[1], &o.sh.cmds, "ui.path")) { g_objs.pop_back(); return 0; }
    }
    int64_t id = (int64_t)g_objs.size();
    Obj* win = objAt(winId);
    if (win) { win->shapes.push_back(id); invalidate(win); }
    ae_push_int(vm, id);
    return 1;
}

// 往路径追加一条指令（第一个参数是路径 id，后面是该指令的数字）
bool pathAppend(AeVM* vm, AeValue* argv, int argc, const char* fn, double op, int numCount) {
    Obj* o = shapeOf(vm, aelib::argInt(vm, argv, argc, 0, fn), fn);
    if (!o) return false;
    if (o->sh.kind != SK_PATH) {
        aelib::raise(vm, std::string(fn) + " 只能用在 ui.path 建出来的路径上（当前是 " +
                         kindName(o->sh.kind) + "）");
        return false;
    }
    o->sh.cmds.push_back(op);
    for (int i = 0; i < numCount; i++)
        o->sh.cmds.push_back(argNum(vm, argv[i + 1], fn, "坐标", i + 2));
    invalidateOfShape(o);
    return true;
}
static int nat_moveTo(AeVM* vm, int argc, AeValue* argv) {          // 移动
    if (!pathAppend(vm, argv, argc, "ui.moveTo", 1, 2)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.moveTo")); return 1;
}
static int nat_lineTo(AeVM* vm, int argc, AeValue* argv) {          // 直线
    if (!pathAppend(vm, argv, argc, "ui.lineTo", 2, 2)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.lineTo")); return 1;
}
static int nat_quadTo(AeVM* vm, int argc, AeValue* argv) {          // 二次贝塞尔
    if (!pathAppend(vm, argv, argc, "ui.quadTo", 3, 4)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.quadTo")); return 1;
}
static int nat_curveTo(AeVM* vm, int argc, AeValue* argv) {         // 三次贝塞尔
    if (!pathAppend(vm, argv, argc, "ui.curveTo", 4, 6)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.curveTo")); return 1;
}
static int nat_arc(AeVM* vm, int argc, AeValue* argv) {             // 弧
    if (!pathAppend(vm, argv, argc, "ui.arc", 5, 6)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.arc")); return 1;
}
static int nat_closePath(AeVM* vm, int argc, AeValue* argv) {       // 闭合
    if (!pathAppend(vm, argv, argc, "ui.closePath", 6, 0)) return 0;
    ae_push_int(vm, aelib::argInt(vm, argv, argc, 0, "ui.closePath")); return 1;
}

// measure(文字[, 选项]) → {w, h, lines, lineH, lineW, charW, lineStart}
//   不建对象也能量文字（排版试宽度用）。选项：font size bold italic wrap lineGap
//   charW 是【逐字符在行内的 x 偏移】，lineStart 是每行首字符的下标 ——
//   自己做文本输入框时，光标位置就是 lineStart[i] 之后第 k 个字符的 charW。
static int nat_measure(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    if (!ae_is_str(argv[0])) {
        aelib::raise(vm, "ui.measure 的第 1 个参数需要字符串");
        return 0;
    }
    size_t n = 0;
    const char* p = ae_to_str(argv[0], &n);
    Shape tmp;
    tmp.kind = SK_TEXT;
    tmp.text = std::string(p, n);
    tmp.font = kDefaultFont;
    tmp.size = 16;
    if (argc > 1 && ae_is_table(argv[1])) {
        std::string sv; double dv = 0; bool bv = false;
        if (ae_table_str(vm, argv[1], "font", &sv))    tmp.font = sv;
        if (ae_table_num(vm, argv[1], "size", &dv))    tmp.size = dv;
        if (ae_table_bool(vm, argv[1], "bold", &bv))   tmp.bold = bv;
        if (ae_table_bool(vm, argv[1], "italic", &bv)) tmp.italic = bv;
        if (ae_table_num(vm, argv[1], "wrap", &dv))    tmp.wrap = dv;
        if (ae_table_num(vm, argv[1], "lineGap", &dv)) tmp.lineGap = dv;
    } else if (argc > 1 && !ae_is_null(argv[1])) {
        aelib::raise(vm, "ui.measure 的第 2 个参数需要选项表，例如 {size = 20, wrap = 120}");
        return 0;
    }
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    TextLayout L;
    if (!layoutText(g, tmp, &L)) { ae_push_null(vm); return 1; }

    AeValue t = ae_new_table(vm);
    auto set = [&](const char* k, AeValue v) { ae_table_set(vm, t, ae_make_str(vm, k), v); };
    set("w", ae_int_value((int64_t)llround(L.w)));
    set("h", ae_int_value((int64_t)llround(L.h)));
    set("lines", ae_int_value((int64_t)L.lines.size()));
    set("lineH", ae_int_value((int64_t)llround(L.lineH)));
    AeValue cw = ae_new_table(vm);
    for (size_t i = 0; i < L.charX.size(); i++)
        ae_table_set(vm, cw, ae_int_value((int64_t)i + 1), ae_int_value((int64_t)llround(L.charX[i])));
    set("charW", cw);
    AeValue ls = ae_new_table(vm);
    for (size_t i = 0; i < L.starts.size(); i++)
        ae_table_set(vm, ls, ae_int_value((int64_t)i + 1), ae_int_value((int64_t)L.starts[i]));
    set("lineStart", ls);
    AeValue lw = ae_new_table(vm);
    for (size_t i = 0; i < L.widths.size(); i++)
        ae_table_set(vm, lw, ae_int_value((int64_t)i + 1), ae_int_value((int64_t)llround(L.widths[i])));
    set("lineW", lw);
    ae_push_table(vm, t);
    return 1;
}

// ── 分组 / 容器 ─────────────────────────────────────────────────────────────
//  group(父) → id：父可以是窗口、画布、或另一个组（可嵌套）。
//  组自己不画东西，它只是"一组图形的共同外壳"：x/y/angle/scale/clip/visible/z 一起生效。
static int nat_group(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    int64_t pid = aelib::argInt(vm, argv, argc, 0, "ui.group");
    if (!surfOf(vm, pid, "ui.group")) return 0;
    if (!ensureGdiplus()) { ae_push_null(vm); return 1; }
    g_objs.push_back(Obj());
    Obj& o = g_objs.back();
    o.kind = OBJ_GROUP;
    o.parent = pid;
    o.sh.kind = SK_GROUP;
    o.sh.winId = pid;
    o.sh.seq = ++g_seq;
    int64_t id = (int64_t)g_objs.size();
    Obj* p = objAt(pid);
    if (p) { p->shapes.push_back(id); invalidateSurface(p); }
    ae_push_int(vm, id);
    return 1;
}

// 递归清理一个对象（组会带走所有子图形，回调句柄一并释放）
void killObj(AeVM* vm, Obj& o) {
    for (int64_t cid : o.shapes) {
        Obj* c = objAt(cid);
        if (!c) continue;
        killObj(vm, *c);
        c->alive = false;
    }
    o.shapes.clear();
    releaseHandlers(vm, o);
    if (o.kind == OBJ_CANVAS) { delete o.bmp; o.bmp = nullptr; }
    if (o.kind == OBJ_IMAGE)  { delete o.img; o.img = nullptr; }
    if (o.kind == OBJ_WINDOW) releaseWindowBuffer(o);      // ★ 离屏缓冲也要还给系统
}

// ── 定时器接口 ──────────────────────────────────────────────────────────────
// ── ★ 定时器精度 ────────────────────────────────────────────────────────────
//   默认 Sleep(1) 实际会睡满一个系统时钟节拍（约 15.6ms），于是"每帧定时器 +
//   重画"的动画只能跑到 30fps 上下。有定时器活着时把系统计时器精度提到 1ms
//   （timeBeginPeriod），动画立刻回到 60fps；定时器全部结束就还回去
//   （一直占着高精度会多耗电，也会影响同机器上其它程序的调度）。
bool g_hiResTimer = false;
void setTimerPrecision(bool on) {
    if (on == g_hiResTimer) return;
    g_hiResTimer = on;
    if (on) timeBeginPeriod(1); else timeEndPeriod(1);
}
void updateTimerPrecision() {
    bool any = false;
    for (Timer& t : g_timers) if (t.alive) { any = true; break; }
    setTimerPrecision(any);
}

static int64_t addTimer(AeVM* vm, int argc, AeValue* argv, const char* fn, bool once, int64_t ms, bool perFrame) {
    g_vm = vm;
    if (!ae_is_func(argv[argc - 1])) {
        aelib::raise(vm, std::string(fn) + " 的最后一个参数需要函数");
        return 0;
    }
    int h = ae_retain_func(vm, argv[argc - 1]);
    if (h <= 0) { aelib::fail("无法持有回调（内部错误）"); return 0; }
    Timer t;
    t.id = ++g_timerSeq;
    t.ms = perFrame ? 0 : ms;
    t.once = once;
    t.handle = h;
    t.nextAt = (int64_t)(GetTickCount64() - g_startTick) + (perFrame ? 0 : ms);
    g_timers.push_back(t);
    updateTimerPrecision();
    return t.id;
}

// timer(毫秒, fn) → id：周期回调
static int nat_timer(AeVM* vm, int argc, AeValue* argv) {
    int64_t ms = aelib::argInt(vm, argv, argc, 0, "ui.timer");
    if (ms < 1) { aelib::raise(vm, "ui.timer 的周期要 >= 1 毫秒"); return 0; }
    int64_t id = addTimer(vm, argc, argv, "ui.timer", false, ms, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}

// timerOnce(毫秒, fn) → id：只触发一次
static int nat_timerOnce(AeVM* vm, int argc, AeValue* argv) {
    int64_t ms = aelib::argInt(vm, argv, argc, 0, "ui.timerOnce");
    if (ms < 0) { aelib::raise(vm, "ui.timerOnce 的延时不能是负数"); return 0; }
    int64_t id = addTimer(vm, argc, argv, "ui.timerOnce", true, ms, false);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}

// tick(fn) → id：每帧回调（动画不用再自己写 sleep + pump 循环）
static int nat_tick(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = addTimer(vm, argc, argv, "ui.tick", false, 0, true);
    if (id) ae_push_int(vm, id); else ae_push_null(vm);
    return 1;
}

// cancelTimer(id) → true/false
static int nat_cancelTimer(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.cancelTimer");
    Timer* t = findTimer(id);
    if (!t || !t->alive) return retBool(vm, false);
    ae_release_func(vm, t->handle);
    t->handle = 0;
    t->alive = false;
    updateTimerPrecision();
    return retBool(vm, true);
}

// timers() → {id = {ms=, once=, count=, perFrame=}, ...}（调试/测试用）
static int nat_timers(AeVM* vm, int, AeValue*) {
    AeValue t = ae_new_table(vm);
    for (Timer& tm : g_timers) {
        if (!tm.alive) continue;
        AeValue one = ae_new_table(vm);
        ae_table_set(vm, one, ae_make_str(vm, "ms"),    ae_int_value(tm.ms));
        ae_table_set(vm, one, ae_make_str(vm, "once"),  ae_bool_value(tm.once));
        ae_table_set(vm, one, ae_make_str(vm, "count"), ae_int_value(tm.count));
        ae_table_set(vm, one, ae_make_str(vm, "frame"), ae_bool_value(tm.ms == 0));
        ae_table_set(vm, t, ae_int_value(tm.id), one);
    }
    ae_push_table(vm, t);
    return 1;
}

// ── 删除 / 列表 / 重画 / 取色 ───────────────────────────────────────────────
static int nat_remove(AeVM* vm, int argc, AeValue* argv) {
    int64_t id = aelib::argInt(vm, argv, argc, 0, "ui.remove");
    Obj* o = objAt(id);
    if (!o) { objOf(vm, id, "ui.remove"); return 0; }
    if (o->kind == OBJ_WINDOW) {
        DestroyWindow(o->hwnd);          // WM_DESTROY 里统一清标志和图形
        return retBool(vm, true);
    }
    Obj* win = objAt(o->sh.winId);
    if (o->kind == OBJ_GROUP) killObj(vm, *o);       // 组：连子图形一起删
    releaseHandlers(vm, *o);                         // ★ 图形被删 → 回调也要释放
    o->alive = false;
    if (win) {
        auto& v = win->shapes;
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
        invalidateSurface(win);
    }
    return retBool(vm, true);
}

static int nat_clear(AeVM* vm, int argc, AeValue* argv) {
    Obj* win = surfOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.clear"), "ui.clear");
    if (!win) return 0;
    for (int64_t sid : win->shapes) {
        Obj* s = objAt(sid);
        if (s) { killObj(vm, *s); s->alive = false; }    // 组会带走子图形
    }
    win->shapes.clear();
    invalidateSurface(win);
    return retBool(vm, true);
}

static int nat_shapes(AeVM* vm, int argc, AeValue* argv) {
    Obj* win = surfOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.shapes"), "ui.shapes");
    if (!win) return 0;
    AeValue t = ae_new_table(vm);
    std::vector<Obj*> v = shapesInDrawOrder(*win, false);   // 列表含隐藏的，自己用 props 过滤
    for (size_t i = 0; i < v.size(); i++)
        ae_table_set(vm, t, ae_int_value((int64_t)i + 1), ae_int_value(idOfObj(v[i])));
    ae_push_table(vm, t);
    return 1;
}

static int nat_redraw(AeVM* vm, int argc, AeValue* argv) {
    Obj* win = surfOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.redraw"), "ui.redraw");
    if (!win) return 0;
    win->dirty = true;                           // 画布：标记重画
    invalidateSurface(win);
    if (win->hwnd) UpdateWindow(win->hwnd);      // 窗口：立刻重画，不等下一次 pump
    return retBool(vm, true);
}

// pixel(win, x, y) → 0xRRGGBB | null：读窗口/画布缓冲里的像素（测试/取色用）
//   ★ 第四批之后它是【读缓冲】：先把待重画的那块画完，再读一个字节 ——
//     不再"每次调用都重画整个场景"，于是几百次取色的测试从几秒变成几毫秒
static int nat_pixel(AeVM* vm, int argc, AeValue* argv) {
    int64_t pid = aelib::argInt(vm, argv, argc, 0, "ui.pixel");
    if (Obj* im = objAt(pid); im && im->kind == OBJ_IMAGE) {      // 图片也直接取色
        int64_t px = aelib::argInt(vm, argv, argc, 1, "ui.pixel");
        int64_t py = aelib::argInt(vm, argv, argc, 2, "ui.pixel");
        if (!im->img || px < 0 || py < 0 || px >= (int64_t)im->img->GetWidth() ||
            py >= (int64_t)im->img->GetHeight()) { ae_push_null(vm); return 1; }
        Gdiplus::Color c;
        im->img->GetPixel((INT)px, (INT)py, &c);
        ae_push_int(vm, ((int64_t)c.GetR() << 16) | ((int64_t)c.GetG() << 8) | (int64_t)c.GetB());
        return 1;
    }
    if (Obj* g = objAt(pid); g && g->kind == OBJ_GROUP) {
        aelib::raise(vm, "ui.pixel：组没有自己的尺寸，请对窗口或画布取色（组内图形的位置要用世界坐标）");
        return 0;
    }
    Obj* win = surfOf(vm, pid, "ui.pixel");
    if (!win) return 0;
    int64_t px = aelib::argInt(vm, argv, argc, 1, "ui.pixel");
    int64_t py = aelib::argInt(vm, argv, argc, 2, "ui.pixel");
    Argb c = 0;
    if (!readSurfacePixel(*win, (int)px, (int)py, &c)) { ae_push_null(vm); return 1; }
    ae_push_int(vm, (int64_t)c);
    return 1;
}

// damage(win|画布) → 表：上一次重画的统计（测试/调优用，不是给你每帧查的）
//   paints  累计重画了几次
//   rects   上次重画了几个矩形
//   drawn   上次【真正画】了几个图形（图形数，不含组）
//   skipped 上次【跳过】了几个图形（在脏区外面，一个字节都没碰；一次重画里只算一次）
//   full    上次是不是整面重画
//   x y w h 上次重画区域的包围盒
//   pending / pendingFull / px py pw ph   还没画的部分（刚改完属性、还没 pump 时能看到）
//
//  用法：改一个图形 → ui.damage(w).drawn 应该很小（1~几个），full = false；
//        如果变成 full = true，说明这块区域算不准（宁可多画面，不会画错）
static int nat_damage(AeVM* vm, int argc, AeValue* argv) {
    int64_t did = aelib::argInt(vm, argv, argc, 0, "ui.damage");
    Obj* o = objAt(did);
    if (!o) { objOf(vm, did, "ui.damage"); return 0; }
    if (o->kind == OBJ_GROUP) {
        aelib::raise(vm, "ui.damage：组没有自己的重画区，问它所在的窗口或画布");
        return 0;
    }
    Obj* s = surfOf(vm, did, "ui.damage");
    if (!s) return 0;
    AeValue t = ae_new_table(vm);
    auto set = [&](const char* k, AeValue v) { ae_table_set(vm, t, ae_make_str(vm, k), v); };
    set("paints", ae_int_value(s->paints));
    set("rects",  ae_int_value(s->lastRects));
    set("drawn",  ae_int_value(s->lastDrawn));
    set("skipped",ae_int_value(s->lastSkipped));
    set("full",   ae_bool_value(s->lastFull));
    set("x", ae_int_value((int64_t)llround(s->lastRegion.X)));
    set("y", ae_int_value((int64_t)llround(s->lastRegion.Y)));
    set("w", ae_int_value((int64_t)llround(s->lastRegion.Width)));
    set("h", ae_int_value((int64_t)llround(s->lastRegion.Height)));
    Gdiplus::RectF pend(0, 0, 0, 0);
    for (size_t i = 0; i < s->damage.size(); i++) rectUnion(pend, s->damage[i]);
    set("pending",     ae_int_value((int64_t)s->damage.size()));
    set("pendingFull", ae_bool_value(s->fullDamage));
    set("px", ae_int_value((int64_t)llround(pend.X)));
    set("py", ae_int_value((int64_t)llround(pend.Y)));
    set("pw", ae_int_value((int64_t)llround(pend.Width)));
    set("ph", ae_int_value((int64_t)llround(pend.Height)));
    ae_push_table(vm, t);
    return 1;
}

// =============================================================================
//  ★ 事件：命中选择 / 派发 / 对脚本的接口
// =============================================================================
const char* kEventKinds =
    "click dblclick mousedown mouseup mousemove mouseenter mouseleave wheel "
    "keydown keyup char resize close";

// 虚拟键码 → 可读键名（Ae 层写快捷键就不用记 VK 码了）
std::string vkName(int vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    switch (vk) {
        case VK_RETURN: return "Enter";     case VK_ESCAPE: return "Escape";
        case VK_SPACE:  return "Space";     case VK_TAB:    return "Tab";
        case VK_BACK:   return "Backspace"; case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";    case VK_HOME:   return "Home";
        case VK_END:    return "End";       case VK_PRIOR:  return "PageUp";
        case VK_NEXT:   return "PageDown";  case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";     case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";      case VK_SHIFT:  return "Shift";
        case VK_CONTROL:return "Ctrl";      case VK_MENU:   return "Alt";
        case VK_CAPITAL:return "CapsLock";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk == 0) return "";
    return "VK" + std::to_string(vk);      // 没收录的给个明确写法，别静默
}

bool isEventKind(const std::string& k) {
    static const char* kinds[] = { "click","dblclick","mousedown","mouseup","mousemove",
                                   "mouseenter","mouseleave","wheel","keydown","keyup","char",
                                   "resize","close" };
    for (const char* s : kinds) if (k == s) return true;
    return false;
}

// =============================================================================
//  命中测试：和绘制共用同一份几何（buildPath）
// -----------------------------------------------------------------------------
//  判定规则（尽量"所见即所点"）：
//    · 封闭图形（矩形/圆/椭圆/闭合多边形/路径/文字框）：点在【区域内】就算命中
//      —— 空心按钮的整个矩形都该能点，这是 UI 里更合手感的约定
//    · 线段/点/不闭合折线：没有内部，按"轮廓 + 容差"算（把路径按笔宽 Widen 后判定）
//    · 描边也能点：区域判定不中时，再把路径加宽判定一次
//    · 裁剪矩形同样约束命中（被裁掉的部分点不到）
// =============================================================================
// ★ 精细命中判定（贵：要建路径、还要 Widen）。所以上游一定要先用【包围盒粗筛】——
//   见 collectHits 里的 pointInBoundsNear。
bool hitShape(Shape& s, double x, double y) {
    if (!s.visible) return false;
    if (!s.hits) return false;                    // 设了鼠标穿透（装饰件）
    if (!ensureGdiplus()) return false;
    invTransformPoint(s, &x, &y);                 // ★ 旋转/缩放：把点反变换回本地坐标
    // ★ 裁剪在【本地坐标】里判：转过的图形，它的裁剪矩形也跟着转（和绘制一致）
    if (!insideClip(s, x, y)) return false;       // 被裁剪掉的部分不参与命中

    // ★ 共用一个 1x1 的 Graphics：以前每次命中测试都新建一个 Bitmap + Graphics，
    //   800 个图形上一条 mousemove 要 30ms，全花在这上面（只是给 IsVisible 当"上下文"用）
    Gdiplus::Graphics& g = measureGraphics();
    Gdiplus::PointF pt((REAL)x, (REAL)y);

    Gdiplus::GraphicsPath path;
    if (!buildPath(path, s)) return false;

    bool regionLike = (s.kind == SK_RECT || s.kind == SK_CIRCLE || s.kind == SK_ELLIPSE ||
                       s.kind == SK_TEXT || s.kind == SK_PATH ||
                       (s.kind == SK_POLY && s.closed) ||
                       (s.kind == SK_POINT&& s.fill));
    if (regionLike && path.IsVisible(pt, &g)) return true;

    // 轮廓判定：把路径按"笔宽 + 容差"加宽，等价于以前手写的那点距离容差
    double w = s.stroke > 0 ? s.stroke : 0;
    if (s.kind == SK_LINE || s.kind == SK_POINT || (s.kind == SK_POLY && !s.closed))
        w = (std::max)(w, 6.0);                    // 细线也要点得到
    else if (w <= 0)
        return false;                              // 区域不中、又没描边 → 没得点了
    Gdiplus::GraphicsPath* widened = path.Clone();      // GDI+ 的拷贝构造是 protected，只能 Clone
    if (!widened) return false;
    Gdiplus::Pen pen(Gdiplus::Color(255, 0, 0, 0), (REAL)(w + 6.0));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    widened->Widen(&pen, nullptr);
    bool hit = widened->IsVisible(pt, &g) != 0;
    delete widened;
    return hit;
}
bool groupBounds(Obj& grp, Gdiplus::RectF* out) {
    bool any = false;
    Gdiplus::RectF acc(0, 0, 0, 0);
    for (int64_t cid : grp.shapes) {
        Obj* c = objAt(cid);
        if (!c) continue;
        Gdiplus::RectF b;
        bool ok = false;
        if (c->kind == OBJ_GROUP) {
            ok = groupBounds(*c, &b);
            // ★ 子组的 x/y 是它相对父组的平移，必须算进去
            //   （子组自己的旋转/缩放不算 —— 这是本函数的"近似"，够用）
            if (ok) { b.X += (REAL)c->sh.x; b.Y += (REAL)c->sh.y; }
        } else {
            Gdiplus::GraphicsPath p;
            if (buildPath(p, c->sh)) ok = (p.GetBounds(&b) == Gdiplus::Ok);
        }
        if (!ok) continue;
        if (!any) { acc = b; any = true; }
        else {
            double x1 = (std::min)((double)acc.X, (double)b.X);
            double y1 = (std::min)((double)acc.Y, (double)b.Y);
            double x2 = (std::max)((double)acc.X + acc.Width,  (double)b.X + b.Width);
            double y2 = (std::max)((double)acc.Y + acc.Height, (double)b.Y + b.Height);
            acc = Gdiplus::RectF((REAL)x1, (REAL)y1, (REAL)(x2 - x1), (REAL)(y2 - y1));
        }
    }
    if (any) *out = acc;
    return any;
}

// 收集命中对象（从最上层往下）。遇到组：先把点反变换进组坐标、递归收子图形，
// 如果点落在组的包围盒里，再把【组自己】也放进链里 —— 于是"子图形 → 组 → 窗口"
// 就是自然的事件冒泡顺序。
//  hits  = 直接命中的图形（【兄弟之间互斥】：派发时第一个有 handler 的接管）
//  bubbles = 命中项所属的组，从最内层到最外层（这些【都会】收到事件 = 冒泡）
void collectHits(Obj& surf, double x, double y,
                 std::vector<int64_t>& hits, std::vector<int64_t>& bubbles) {
    std::vector<Obj*> v = shapesInDrawOrder(surf, true);
    for (size_t i = v.size(); i-- > 0; ) {
        Obj* it = v[i];
        if (it->kind == OBJ_GROUP) {
            double gx = x - it->sh.x, gy = y - it->sh.y;       // 组的平移 + 旋转/缩放
            invTransformPoint(it->sh, &gx, &gy, it);
            // ★ 组的裁剪是【相对组自己】的（和绘制时 SetClip 的位置一致：
            //   先平移进组坐标，再设裁剪），所以这里用 gx/gy 判，不用 x/y
            if (!insideClip(it->sh, gx, gy)) continue;
            // ★ 粗筛：整个组（子图形并集）都不沾这个点 → 整棵子树都不用看
            Gdiplus::RectF gb;
            if (itemParentBounds(*it, &gb) && !pointInBoundsNear(gb, x, y, kHitSlack))
                continue;
            size_t before = hits.size();
            collectHits(*it, gx, gy, hits, bubbles);
            bool hitInside = (hits.size() > before);
            Gdiplus::RectF b;
            bool inBox = groupBounds(*it, &b) &&
                         gx >= b.X && gx <= b.X + b.Width && gy >= b.Y && gy <= b.Y + b.Height;
            if (hitInside || inBox) bubbles.push_back(idOfObj(it));   // 内层先 push → 顺序天然正确
        } else {
            // ★ 粗筛（第四批的性能关键）：先用【缓存的包围盒】把绝大多数图形挡掉。
            //   以前每个图形都要真的建一次路径 + Widen 才判定，800 个图形上一条
            //   mousemove 要 30ms（动画被拖到 30fps）；现在只有包围盒沾到点的才细判。
            Gdiplus::RectF b;
            if (itemParentBounds(*it, &b) && !pointInBoundsNear(b, x, y, kHitSlack))
                continue;
            if (hitShape(it->sh, x, y)) hits.push_back(idOfObj(it));
        }
    }
}

// ★ 命中的对象【链】：从最上层往下。（事件派发沿这条链走：谁注册了这种事件的 handler
//   谁就接住；都没注册就一直往下，最后冒泡到窗口。这样"按钮上盖了个文字标签"也不会
//   挡住点击 —— 那条链是 [标签, 按钮, 背景...]，点下去照样打到按钮上。）
std::vector<int64_t> hitCandidates(Obj& win, int x, int y) {   // 悬停用：直接命中的排前面
    std::vector<int64_t> hits, bubbles;
    collectHits(win, (double)x, (double)y, hits, bubbles);
    hits.insert(hits.end(), bubbles.begin(), bubbles.end());
    return hits;
}
bool hasHoverHandler(Obj& o) {
    for (const Handler& h : o.handlers)
        if (h.kind == "mouseenter" || h.kind == "mouseleave") return true;
    return false;
}

// 把事件表交给某个对象上登记的同类回调；返回 true 表示被"吃掉"（停止冒泡）
// 返回 true = 这个对象有该事件的 handler（事件归它管，命中链不再往下走）。
// ★ 注意区分两件事：
//     · "有 handler"  → 事件被这个对象【接管】，链上更下面的对象不会再收到
//     · "return true" → 连窗口也不再收到（吃掉 / consumed）
bool dispatchTo(AeVM* vm, int64_t id, const Ev& ev, bool* consumed) {
    Obj* o = objAt(id);
    if (!o) return false;
    bool handled = false;
    std::vector<Handler> hs = o->handlers;      // ★ 拷贝：回调里可能 off / remove
    for (const Handler& h : hs) {
        if (h.kind != ev.kind) continue;
        handled = true;
        AeValue t = ae_new_table(vm);
        auto set = [&](const char* k, AeValue v) { ae_table_set(vm, t, ae_make_str(vm, k), v); };
        set("kind",   ae_make_str(vm, ev.kind));
        set("win",    ae_int_value(ev.winId));
        set("target", ae_int_value(id));
        if (ev.hasPos) { set("x", ae_int_value(ev.x)); set("y", ae_int_value(ev.y)); }
        if (ev.button) set("button", ae_int_value(ev.button));
        if (ev.delta)  set("wheel",  ae_int_value(ev.delta));
        if (ev.key)    set("key",    ae_int_value(ev.key));
        if (ev.kind == "resize") { set("w", ae_int_value(ev.w)); set("h", ae_int_value(ev.h)); }
        set("shift", ae_bool_value(ev.shift));       // ★ 修饰键：多选/框选/快捷键组合要用
        set("ctrl",  ae_bool_value(ev.ctrl));
        set("alt",   ae_bool_value(ev.alt));
        set("time",  ae_int_value(ev.time));         // ★ 毫秒时间戳：长按/双击间隔/拖动速度
        if (ev.kind == "keydown" || ev.kind == "keyup" || ev.kind == "char")
            set("keyName", ae_make_str(vm, vkName(ev.key)));   // ★ 可读键名："Enter"/"Left"/"A"
        AeValue out;
        std::string err;
        if (!ae_call_func1(vm, h.handle, t, &out, &err)) {
            // 约定：回调里出错当场中断程序（变成带调用栈的运行错误）。
            // 外面有 try/catch 的话照样能接住。
            ae_raise(vm, std::string("事件回调 ") + ev.kind + " 出错: " + err);
            return true;                         // ae_raise 不返回
        }
        if (ae_is_bool(out) && out.i != 0) { if (consumed) *consumed = true; return true; }
    }
    return handled;
}

// 派发一批事件。★ Windows 只给 down/up，"点击"必须自己合成 —— 合成放在这里
// （事件层）而不是窗口过程里，好处是 ui.send 也走同一条路径，测试才覆盖得到真实行为。
void flushEvents(AeVM* vm) {
    if (g_events.empty()) return;
    std::vector<Ev> evs;
    evs.swap(g_events);                 // 回调里再 ui.send 的事件排到下一轮（不会无限递归）

    // 处理【一条】事件：悬停进出 → 命中链（从上层往下，谁注册谁接）→ 冒泡到窗口 → close 收尾
    auto handle = [&](const Ev& ev) -> bool {
        Obj* win = objAt(ev.winId);
        if (!win || win->kind != OBJ_WINDOW || !win->hwnd) return false;

        if (ev.kind == "mousemove") {
            std::vector<int64_t> cand = hitCandidates(*win, ev.x, ev.y);
            int64_t t = 0;
            for (int64_t id : cand) {                   // 谁注册了 enter/leave 才算悬停目标
                Obj* o = objAt(id);
                if (o && hasHoverHandler(*o)) { t = id; break; }
            }
            // ★ 光标形状：悬停链上第一个设了 cursor 的对象优先，否则用窗口默认
            std::string want;
            for (int64_t hid : cand) {
                Obj* ho = objAt(hid);
                if (ho && ho->kind == OBJ_SHAPE && ho->sh.hits && ho->sh.visible && !ho->sh.cursor.empty()) {
                    want = ho->sh.cursor;
                    break;
                }
            }
            if (want.empty()) want = win->cursorName;
            if (want != win->curCursor) {
                win->curCursor = want;
                if (loadCursorByName(want)) applyCursor(*win);
            }
            if (t != win->hover) {
                int64_t old = win->hover;
                win->hover = t;
                if (old) {
                    bool c = false;
                    Ev le = ev; le.kind = "mouseleave";
                    dispatchTo(vm, old, le, &c);
                    if (!objAt(ev.winId)) return false;
                }
                if (t) {
                    bool c = false;
                    Ev en = ev; en.kind = "mouseenter";
                    dispatchTo(vm, t, en, &c);
                    if (!objAt(ev.winId)) return false;
                }
            }
        } else if (ev.kind == "mouseleave") {
            int64_t old = win->hover;
            win->hover = 0;
            if (old) { bool c = false; dispatchTo(vm, old, ev, &c); }
            if (!objAt(ev.winId)) return false;
        }

        bool consumed = false;
        if (ev.hasPos) {
            std::vector<int64_t> hits, bubbles;
            collectHits(*win, (double)ev.x, (double)ev.y, hits, bubbles);
            // ① 直接命中的图形：从上层往下，第一个有 handler 的【接管】（兄弟之间互斥）
            for (int64_t id : hits) {
                if (dispatchTo(vm, id, ev, &consumed)) break;
                if (!objAt(ev.winId)) return false;
            }
            // ② 冒泡到它所属的组（从内到外，每个组都有机会）—— 自制控件的关键
            for (int64_t id : bubbles) {
                if (consumed) break;
                dispatchTo(vm, id, ev, &consumed);
                if (!objAt(ev.winId)) return false;
            }
        }
        // ③ 最后冒泡到窗口（除非被 return true 吃掉）——"点别处关掉菜单"靠它
        if (!consumed && objAt(ev.winId))
            dispatchTo(vm, ev.winId, ev, &consumed);

        Obj* w2 = objAt(ev.winId);
        if (ev.kind == "close" && w2 && w2->pendingClose && !consumed) {
            w2->pendingClose = false;
            DestroyWindow(w2->hwnd);                     // 回调没吃掉 close → 真关
        }
        return true;
    };

    for (const Ev& ev : evs) {
        Obj* win = objAt(ev.winId);
        if (!win || win->kind != OBJ_WINDOW || !win->hwnd) continue;

        bool synthClick = false;
        int  btn = ev.button;
        if (ev.kind == "mousedown" && btn >= 1 && btn <= 3) {
            win->downActive[btn] = true;
            win->downX[btn] = ev.x;
            win->downY[btn] = ev.y;
        } else if (ev.kind == "mouseup" && btn >= 1 && btn <= 3) {
            bool wasDown = win->downActive[btn];
            int dx = ev.x - win->downX[btn];
            int dy = ev.y - win->downY[btn];
            win->downActive[btn] = false;
            // 同一个键按下 → 抬起，且几乎没移动（4px 容差）= 一次点击
            if (wasDown && dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4) synthClick = true;
        } else if (ev.kind == "dblclick" && btn >= 1 && btn <= 3) {
            win->downActive[btn] = false;                // 双击的第二下不再算单击
        }

        if (!handle(ev)) continue;
        if (synthClick && objAt(ev.winId)) {             // 顺序和浏览器一致：up 之后才是 click
            Ev ck = ev;
            ck.kind = "click";
            handle(ck);
        }
    }
}
// ── 定时器派发 ──────────────────────────────────────────────────────────────
Timer* findTimer(int64_t id) {
    for (Timer& t : g_timers) if (t.id == id) return &t;
    return nullptr;
}

void flushTimers(AeVM* vm) {
    if (g_timers.empty()) return;
    int64_t now = (int64_t)(GetTickCount64() - g_startTick);
    std::vector<int64_t> due;
    for (Timer& t : g_timers)
        if (t.alive && (t.ms == 0 || now >= t.nextAt)) due.push_back(t.id);

    for (int64_t id : due) {
        Timer* t = findTimer(id);
        if (!t || !t->alive || !t->handle) continue;      // 回调里可能已经取消了
        int64_t late = (t->ms > 0 && now > t->nextAt) ? (now - t->nextAt) : 0;
        AeValue e = ae_new_table(vm);
        auto set = [&](const char* k, AeValue v) { ae_table_set(vm, e, ae_make_str(vm, k), v); };
        set("kind",  ae_make_str(vm, "timer"));
        set("id",    ae_int_value(id));
        set("count", ae_int_value(t->count));
        set("late",  ae_int_value(late));                 // 迟到了多少毫秒（脚本忙的时候会 >0）
        AeValue out;
        std::string err;
        if (!ae_call_func1(vm, t->handle, e, &out, &err)) {
            ae_raise(vm, std::string("定时器回调出错: ") + err);   // 和事件一样：当场中断
            return;
        }
        Timer* t2 = findTimer(id);
        if (!t2 || !t2->alive) continue;
        t2->count++;
        if (t2->once) {
            ae_release_func(vm, t2->handle);
            t2->handle = 0;
            t2->alive = false;
        } else if (t2->ms > 0) {
            int64_t n = (int64_t)(GetTickCount64() - g_startTick);
            int64_t steps = (n - t2->nextAt) / t2->ms + 1;
            t2->nextAt += steps * t2->ms;                 // 落后就跳到未来，不补发
        }
    }
    updateTimerPrecision();                               // 一次性定时器跑完 → 可能该还回精度
}

// ── 对脚本的事件接口 ────────────────────────────────────────────────────────
static int nat_on(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    Obj* o = objOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.on"), "ui.on");
    if (!o) return 0;
    const std::string& kind = aelib::argStr(vm, argv, argc, 1, "ui.on");
    if (!isEventKind(kind)) {
        aelib::raise(vm, "ui.on 不认识事件 '" + kind + "'；可用：" + kEventKinds);
        return 0;
    }
    if (!ae_is_func(argv[2])) {
        aelib::raise(vm, "ui.on 的第 3 个参数需要函数，例如 ui.on(w, \"click\", func(e) { ... })");
        return 0;
    }
    int h = ae_retain_func(vm, argv[2]);
    if (h <= 0) { aelib::fail("ui.on 无法持有回调（内部错误）"); return retBool(vm, false); }
    o->handlers.push_back({kind, h});
    return retBool(vm, true);
}

static int nat_off(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    Obj* o = objOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.off"), "ui.off");
    if (!o) return 0;
    const std::string& kind = aelib::argStr(vm, argv, argc, 1, "ui.off");
    int n = 0;
    std::vector<Handler> keep;
    for (Handler& h : o->handlers) {
        if (h.kind == kind) { ae_release_func(vm, h.handle); n++; }
        else keep.push_back(h);
    }
    o->handlers.swap(keep);
    ae_push_int(vm, n);
    return 1;
}

static int nat_offAll(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    Obj* o = objOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.offAll"), "ui.offAll");
    if (!o) return 0;
    int n = (int)o->handlers.size();
    releaseHandlers(vm, *o);
    ae_push_int(vm, n);
    return 1;
}

// handlers(target) → { kind = 个数, ... }（测试/调试用）
static int nat_handlers(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    Obj* o = objOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.handlers"), "ui.handlers");
    if (!o) return 0;
    AeValue t = ae_new_table(vm);
    for (const Handler& h : o->handlers) {
        AeValue k = ae_make_str(vm, h.kind);
        AeValue cur;
        int64_t n = 0;
        if (ae_table_get(vm, t, k, &cur) && ae_is_int(cur)) n = cur.i;
        ae_table_set(vm, t, k, ae_int_value(n + 1));
    }
    ae_push_table(vm, t);
    return 1;
}

// send(win, kind, {x=,y=,button=,wheel=,key=,w=,h=}) —— 合成一个事件塞进队列。
// 走的是和真实消息【完全一样】的派发路径，所以测试能覆盖真实行为；也能用来做
// 快捷键/演示脚本。
static int nat_send(AeVM* vm, int argc, AeValue* argv) {
    g_vm = vm;
    Obj* win = winOf(vm, aelib::argInt(vm, argv, argc, 0, "ui.send"), "ui.send");
    if (!win) return 0;
    const std::string& kind = aelib::argStr(vm, argv, argc, 1, "ui.send");
    if (!isEventKind(kind)) {
        aelib::raise(vm, "ui.send 不认识事件 '" + kind + "'；可用：" + kEventKinds);
        return 0;
    }
    Ev e;
    e.kind = kind;
    e.winId = idOfObj(win);
    if (argc > 2 && ae_is_table(argv[2])) {
        AeValue v;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "x"), &v) && ae_is_num(v)) {
            e.x = (int)(ae_is_int(v) ? v.i : (int64_t)v.f); e.hasPos = true;
        }
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "y"), &v) && ae_is_num(v))
            e.y = (int)(ae_is_int(v) ? v.i : (int64_t)v.f);
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "button"), &v) && ae_is_int(v)) e.button = (int)v.i;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "wheel"), &v) && ae_is_int(v))  e.delta = (int)v.i;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "key"), &v) && ae_is_int(v))    e.key = (int)v.i;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "w"), &v) && ae_is_int(v))      e.w = (int)v.i;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "h"), &v) && ae_is_int(v))      e.h = (int)v.i;
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "shift"), &v) && ae_is_bool(v)) e.shift = (v.i != 0);
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "ctrl"),  &v) && ae_is_bool(v)) e.ctrl  = (v.i != 0);
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "alt"),   &v) && ae_is_bool(v)) e.alt   = (v.i != 0);
        if (ae_table_get(vm, argv[2], ae_make_str(vm, "time"),  &v) && ae_is_int(v))  e.time  = v.i;
        if (e.time == 0) e.time = (int64_t)(GetTickCount64() - g_startTick);
    }
    // ★ 鼠标类事件默认左键：ui.send(w, "click", {x=,y=}) 不写 button 就当左键
    //   （真实消息里 button 一定是 1/2/3，这里是合成入口才需要兜底）
    if (e.button == 0 && (kind == "click" || kind == "dblclick" ||
                          kind == "mousedown" || kind == "mouseup")) e.button = 1;
    if (kind == "close") win->pendingClose = true;   // 合成的 close 也走"可取消"的语义
    if (kind == "click") {
        // ★ 和真鼠标一样：先 down 再 up，由事件层合成 click（这样测试覆盖的是真实路径）
        Ev d = e; d.kind = "mousedown"; g_events.push_back(d);
        Ev u = e; u.kind = "mouseup";   g_events.push_back(u);
        return retBool(vm, true);
    }
    if (kind == "dblclick") {
        Ev d1 = e; d1.kind = "mousedown"; g_events.push_back(d1);
        Ev u1 = e; u1.kind = "mouseup";   g_events.push_back(u1);
        Ev d2 = e; d2.kind = "dblclick";  g_events.push_back(d2);
        Ev u2 = e; u2.kind = "mouseup";   g_events.push_back(u2);
        return retBool(vm, true);
    }
    g_events.push_back(e);
    return retBool(vm, true);
}

}  // namespace

// =============================================================================
//  导出表
// =============================================================================
static const AeNativeDef kExports[] = {
    // 核心：建窗口 + 标题 / 大小 / 位置 / 缩放 / 最大化
    { "window",      3, 3, nat_window },
    { "title",       2, 2, nat_title },
    { "titleOf",     1, 1, nat_titleOf },
    { "size",        3, 3, nat_size },
    { "pos",         3, 3, nat_pos },
    { "resizable",   2, 2, nat_resizable },
    { "maximizable", 2, 2, nat_maximizable },
    // 看得见 / 关得掉：显示、隐藏、关闭 + 消息循环
    { "show",        1, 1, nat_show },
    { "hide",        1, 1, nat_hide },
    { "close",       1, 1, nat_close },
    { "pump",        0, 0, nat_pump },
    { "run",         1, 1, nat_run },
    // 查询
    { "visible",     1, 1, nat_visible },
    { "count",       0, 0, nat_count },
    { "info",        1, 1, nat_info },
    { "maximize",    1, 1, nat_maximize },
    { "minimize",    1, 1, nat_minimize },
    { "beginDrag",   1, 1, nat_beginDrag },
    { "restore",     1, 1, nat_restore },
    // ★ 绘制：创建（所有图形都是对象，返回 id）
    { "point",       3, 3, nat_point },
    { "line",        5, 5, nat_line },
    { "poly",        2, 2, nat_poly },
    { "rect",        5, 5, nat_rect },
    { "roundRect",   6, 6, nat_roundRect },
    { "circle",      4, 4, nat_circle },
    { "ellipse",     5, 5, nat_ellipse },
    { "text",        4, 4, nat_text },
    { "textSize",    1, 2, nat_textSize },
    { "measure",     1, 2, nat_measure },
    // ★ 画布 / 图片 / 贴图
    { "group",       1, 1, nat_group },
    { "canvas",      2, 2, nat_canvas },
    { "image",       1, 1, nat_image },
    { "drawImage",   4, 6, nat_drawImage },
    // ★ 路径（任意几何：曲线 / 图标 / 挖洞）
    { "path",        1, 2, nat_path },
    { "moveTo",      3, 3, nat_moveTo },
    { "lineTo",      3, 3, nat_lineTo },
    { "quadTo",      5, 5, nat_quadTo },
    { "curveTo",     7, 7, nat_curveTo },
    { "arc",         7, 7, nat_arc },
    { "closePath",   1, 1, nat_closePath },
    // ★ 绘制：属性读写 / 层级 / 删除 / 取色
    { "set",         3, 3, nat_set },
    { "setAll",      2, 2, nat_setAll },
    { "get",         2, 2, nat_get },
    { "props",       1, 1, nat_props },
    { "remove",      1, 1, nat_remove },
    { "clear",       1, 1, nat_clear },
    { "shapes",      1, 1, nat_shapes },
    { "redraw",      1, 1, nat_redraw },
    { "pixel",       3, 3, nat_pixel },
    { "damage",      1, 1, nat_damage },
    // ★ 事件
    { "on",          3, 3, nat_on },
    { "off",         2, 2, nat_off },
    { "offAll",      1, 1, nat_offAll },
    { "handlers",    1, 1, nat_handlers },
    { "send",        2, 3, nat_send },
    // ★ 定时器
    { "timer",       2, 2, nat_timer },
    { "timerOnce",   2, 2, nat_timerOnce },
    { "tick",        1, 1, nat_tick },
    { "cancelTimer", 1, 1, nat_cancelTimer },
    { "timers",      0, 0, nat_timers },
    { "lastError",   0, 0, nat_lastError },
};

static const AeNativeDef* getExports(int* n) {
    *n = (int)(sizeof(kExports) / sizeof(kExports[0]));
    return kExports;
}

AE_MODULE("ui", getExports)
