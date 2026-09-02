/* =========================================================================
   Ae Language — 前端逻辑
   功能：哈希路由（首页 / 文档 / 下载）、移动端汉堡菜单、滚动高亮侧边栏
   ========================================================================= */

(function () {
    'use strict';

    // ---------- 页面路由表 ----------
    const PAGES = {
        '/':       'page-home',
        '/docs':   'page-docs',
        '/download': 'page-download',
    };

    const navItems = document.querySelectorAll('.nav-links a[data-page]');
    const sideLinks = document.querySelectorAll('.side-link');

    // 根据 hash 渲染对应页面
    function render() {
        const raw = location.hash.replace(/^#/, '') || '/';
        // 去掉 #anchor 部分，只取路径
        const path = raw.split('#')[0] || '/';
        const targetId = PAGES[path] || 'page-home';

        // 显示 / 隐藏页面
        Object.values(PAGES).forEach(id => {
            const el = document.getElementById(id);
            if (el) el.classList.toggle('hidden', id !== targetId);
        });

        // 顶部导航高亮
        navItems.forEach(a => a.classList.toggle('active', a.dataset.page === pageKey(path)));

        // 滚动到锚点（文档页内跳转）
        const hashPart = location.hash.includes('#') ? location.hash.split('#').pop() : '';
        if (hashPart && path === '/docs') {
            // 等待页面渲染后跳转
            requestAnimationFrame(() => {
                const el = document.getElementById(hashPart);
                if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
            });
        }

        // 回到顶部（页面切换时）
        if (!hashPart) window.scrollTo(0, 0);
    }

    function pageKey(path) {
        if (path === '/' || path === '') return 'home';
        if (path.startsWith('/docs')) return 'docs';
        if (path.startsWith('/download')) return 'download';
        return 'home';
    }

    // 供 onclick 调用：更新 hash 并渲染
    window.navigate = function (path) {
        if (location.hash !== '#' + path) {
            location.hash = '#' + path;
        } else {
            render();
        }
        closeMenu();
    };

    // 供侧边栏链接：先导航、再滚动
    window.scrollToHash = function (id) {
        requestAnimationFrame(() => {
            const el = document.getElementById(id);
            if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
        });
    };

    // ---------- 哈希变化监听 ----------
    window.addEventListener('hashchange', render);

    // ---------- 移动端汉堡菜单 ----------
    const hamburger = document.getElementById('hamburger');
    const navLinks = document.getElementById('navLinks');

    function closeMenu() { navLinks.classList.remove('open'); }

    if (hamburger) {
        hamburger.addEventListener('click', () => {
            navLinks.classList.toggle('open');
        });
    }

    // ---------- 文档侧边栏：滚动时高亮当前章节 ----------
    function setupScrollSpy() {
        const sections = document.querySelectorAll('.doc-section');
        if (!sections.length) return;

        const observer = new IntersectionObserver((entries) => {
            entries.forEach(entry => {
                if (entry.isIntersecting) {
                    const id = entry.target.id;
                    sideLinks.forEach(link => {
                        const active = link.getAttribute('href').endsWith('#' + id);
                        link.classList.toggle('active', active);
                    });
                }
            });
        }, {
            // 顶部偏移导航高度，提前触发
            rootMargin: '-80px 0px -65% 0px',
            threshold: 0,
        });

        sections.forEach(s => observer.observe(s));
    }

    // ---------- 初始化 ----------
    function init() {
        if (!location.hash) location.hash = '#/';
        render();
        setupScrollSpy();
    }

    init();
})();
