/**
 * Cloudflare Worker: 独立在线网络收藏夹与云同步后端
 * 具备独立 Web 端操作界面、D1 数据库持久化、Chrome 插件 API 与多端增量同步引擎。
 */

export interface Env {
  DB: D1Database;
  ACCESS_TOKEN?: string;
  APP_TITLE?: string;
  ALLOW_PUBLIC_READ?: string;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;

    // 处理 CORS 跨域（供桌面客户端与浏览器插件调用）
    if (method === "OPTIONS") {
      return handleCors();
    }

    // 静态资源与页面渲染
    if (path === "/" || path === "/index.html") {
      const authorized = checkAuth(request, env);
      const isPublicRead = env.ALLOW_PUBLIC_READ === "true";
      if (!authorized && !isPublicRead && env.ACCESS_TOKEN) {
        return new Response(renderLoginPage(env.APP_TITLE || "在线网络收藏夹"), {
          headers: { "Content-Type": "text/html; charset=utf-8" },
        });
      }
      return new Response(renderWebApp(env.APP_TITLE || "我的在线网络收藏夹", authorized), {
        headers: { "Content-Type": "text/html; charset=utf-8" },
      });
    }

    // --- API 鉴权 ---
    if (path.startsWith("/api/")) {
      // 登录与健康检查豁免
      if (path === "/api/auth/login" && method === "POST") {
        return handleLogin(request, env);
      }
      if (path === "/api/status" && method === "GET") {
        return handleStatus(request, env);
      }

      const authorized = checkAuth(request, env);
      if (!authorized && env.ACCESS_TOKEN) {
        return jsonResponse({ error: "Unauthorized: Invalid or missing Access Token" }, 401);
      }

      // API 路由分发
      try {
        if (path === "/api/categories") {
          if (method === "GET") return handleGetCategories(env);
          if (method === "POST") return handleCreateCategory(request, env);
        }
        if (path === "/api/bookmarks") {
          if (method === "GET") return handleGetBookmarks(url, env);
          if (method === "POST") return handleCreateBookmark(request, env);
        }
        if (path.startsWith("/api/bookmarks/") && method === "DELETE") {
          const id = parseInt(path.replace("/api/bookmarks/", ""), 10);
          return handleDeleteBookmark(id, env);
        }
        if (path === "/api/fetch-meta" && method === "GET") {
          return handleFetchMeta(url);
        }
        if (path === "/api/sync") {
          if (method === "GET") return handleSyncPull(url, env);
          if (method === "POST") return handleSyncPush(request, env);
        }
        if (path === "/api/wipe" && method === "POST") {
          return handleWipe(env);
        }
      } catch (err: any) {
        return jsonResponse({ error: err.message || "Internal Server Error" }, 500);
      }

      return jsonResponse({ error: "Not Found" }, 404);
    }

    return new Response("Not Found", { status: 404 });
  },
};

// --- 鉴权工具 ---
function checkAuth(request: Request, env: Env): boolean {
  if (!env.ACCESS_TOKEN) return true; // 未设 token 则公开

  // 1. Bearer Header
  const authHeader = request.headers.get("Authorization");
  if (authHeader && authHeader.startsWith("Bearer ")) {
    if (authHeader.slice(7).trim() === env.ACCESS_TOKEN.trim()) return true;
  }

  // 2. Query param ?token=
  const url = new URL(request.url);
  if (url.searchParams.get("token") === env.ACCESS_TOKEN.trim()) return true;

  // 3. Cookie cf_bmk_token=
  const cookies = request.headers.get("Cookie") || "";
  const match = cookies.match(/cf_bmk_token=([^;]+)/);
  if (match && decodeURIComponent(match[1]) === env.ACCESS_TOKEN.trim()) return true;

  return false;
}

async function handleLogin(request: Request, env: Env): Promise<Response> {
  const body: any = await request.json().catch(() => ({}));
  const token = body.token?.trim() || "";

  if (env.ACCESS_TOKEN && token !== env.ACCESS_TOKEN.trim()) {
    return jsonResponse({ success: false, error: "访问密钥无效，请核对后重试" }, 401);
  }

  const cookie = `cf_bmk_token=${encodeURIComponent(token)}; Path=/; Max-Age=2592000; SameSite=Lax; HttpOnly`;
  return new Response(JSON.stringify({ success: true, message: "登录成功" }), {
    headers: {
      "Content-Type": "application/json",
      "Set-Cookie": cookie,
      ...corsHeaders(),
    },
  });
}

// --- API 实现 ---
async function handleStatus(request: Request, env: Env): Promise<Response> {
  const totalB = await env.DB.prepare("SELECT COUNT(*) as c FROM bookmarks WHERE is_deleted = 0").first<any>();
  const totalC = await env.DB.prepare("SELECT COUNT(*) as c FROM categories WHERE is_deleted = 0").first<any>();
  return jsonResponse({
    status: "ok",
    app: env.APP_TITLE || "Online Bookmark Manager",
    bookmarks_count: totalB?.c ?? 0,
    categories_count: totalC?.c ?? 0,
    server_time: Math.floor(Date.now() / 1000),
  });
}

async function handleGetCategories(env: Env): Promise<Response> {
  const result = await env.DB.prepare(
    "SELECT id, uuid, name, parent_id, sort_order, updated_at FROM categories WHERE is_deleted = 0 ORDER BY coalesce(sort_order, 0) ASC, name ASC"
  ).all();
  return jsonResponse(result.results);
}

async function handleCreateCategory(request: Request, env: Env): Promise<Response> {
  const data: any = await request.json();
  const name = (data.name || "").trim();
  if (!name) return jsonResponse({ error: "分类名不能为空" }, 400);

  const now = Math.floor(Date.now() / 1000);
  const uuid = crypto.randomUUID();
  const res = await env.DB.prepare(
    "INSERT INTO categories (uuid, name, parent_id, sort_order, updated_at, is_deleted) VALUES (?, ?, ?, ?, ?, 0)"
  )
    .bind(uuid, name, data.parent_id || null, data.sort_order || 0, now)
    .run();

  return jsonResponse({ id: res.meta.last_row_id, uuid, name, parent_id: data.parent_id || null });
}

async function handleGetBookmarks(url: URL, env: Env): Promise<Response> {
  const search = url.searchParams.get("search");
  const categoryId = url.searchParams.get("category_id");
  const tag = url.searchParams.get("tag");

  let query = `
    SELECT b.id, b.uuid, b.title, b.url, b.url_key, b.icon, b.category_id,
           c.name as category_name, b.valid, b.created_at, b.updated_at
    FROM bookmarks b
    LEFT JOIN categories c ON b.category_id = c.id
    WHERE b.is_deleted = 0
  `;
  const params: any[] = [];

  if (search) {
    query += " AND (b.title LIKE ? OR b.url LIKE ?)";
    params.push(`%${search}%`, `%${search}%`);
  }
  if (categoryId !== null && categoryId !== undefined && categoryId !== "") {
    if (categoryId === "-1") {
      query += " AND b.category_id IS NULL";
    } else {
      query += " AND b.category_id = ?";
      params.push(parseInt(categoryId, 10));
    }
  }

  query += " ORDER BY b.created_at DESC LIMIT 500";
  const stmt = env.DB.prepare(query);
  const rows = await (params.length ? stmt.bind(...params) : stmt).all();

  // 附带标签
  const bookmarks = rows.results;
  if (bookmarks.length > 0) {
    const ids = bookmarks.map((b: any) => b.id).join(",");
    const tagRows = await env.DB.prepare(
      `SELECT bt.bookmark_id, t.name FROM bookmark_tags bt JOIN tags t ON bt.tag_id = t.id WHERE bt.bookmark_id IN (${ids})`
    ).all();

    const tagMap = new Map<number, string[]>();
    for (const r of tagRows.results as any[]) {
      if (!tagMap.has(r.bookmark_id)) tagMap.set(r.bookmark_id, []);
      tagMap.get(r.bookmark_id)!.push(r.name);
    }

    for (const b of bookmarks as any[]) {
      b.tags = tagMap.get(b.id) || [];
    }
  }

  return jsonResponse(bookmarks);
}

async function handleCreateBookmark(request: Request, env: Env): Promise<Response> {
  const data: any = await request.json();
  let url = (data.url || "").trim();
  if (!url) return jsonResponse({ error: "URL 不能为空" }, 400);
  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    url = "https://" + url;
  }

  let title = (data.title || "").trim();
  let icon = data.icon || null;

  // 自动抓取
  if (!title || !icon) {
    try {
      const meta = await fetchPageMeta(url);
      if (!title && meta.title) title = meta.title;
      if (!icon && meta.icon) icon = meta.icon;
    } catch (e) {}
  }
  if (!title) title = url;

  const now = Math.floor(Date.now() / 1000);
  const uuid = crypto.randomUUID();
  const urlKey = url.replace(/\/+$/, "");

  const res = await env.DB.prepare(
    `INSERT INTO bookmarks (uuid, title, url, url_key, icon, category_id, valid, created_at, updated_at, is_deleted)
     VALUES (?, ?, ?, ?, ?, ?, 1, ?, ?, 0)`
  )
    .bind(uuid, title, url, urlKey, icon, data.category_id || null, now, now)
    .run();

  const bookmarkId = res.meta.last_row_id;

  // 标签处理
  if (Array.isArray(data.tags)) {
    for (const t of data.tags) {
      const tagName = String(t).trim();
      if (!tagName) continue;
      await env.DB.prepare("INSERT OR IGNORE INTO tags (name) VALUES (?)").bind(tagName).run();
      const tagRow = await env.DB.prepare("SELECT id FROM tags WHERE name = ? COLLATE NOCASE").bind(tagName).first<any>();
      if (tagRow) {
        await env.DB.prepare("INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?, ?)").bind(bookmarkId, tagRow.id).run();
      }
    }
  }

  return jsonResponse({
    id: bookmarkId,
    uuid,
    title,
    url,
    icon,
    category_id: data.category_id || null,
    tags: data.tags || [],
  });
}

async function handleDeleteBookmark(id: number, env: Env): Promise<Response> {
  const now = Math.floor(Date.now() / 1000);
  await env.DB.prepare("UPDATE bookmarks SET is_deleted = 1, updated_at = ? WHERE id = ?").bind(now, id).run();
  return jsonResponse({ success: true, id });
}

// 增量同步拉取
async function handleSyncPull(url: URL, env: Env): Promise<Response> {
  const since = parseInt(url.searchParams.get("since") || "0", 10);
  const now = Math.floor(Date.now() / 1000);

  const categories = await env.DB.prepare(
    `SELECT c1.id, c1.uuid, c1.name, c1.parent_id, c2.name as parent_name, c1.sort_order, c1.updated_at, c1.is_deleted
     FROM categories c1
     LEFT JOIN categories c2 ON c1.parent_id = c2.id
     WHERE c1.updated_at > ?`
  ).bind(since).all();

  const bookmarks = await env.DB.prepare(
    `SELECT b.id, b.uuid, b.title, b.url, b.url_key, b.icon, b.category_id,
            c.name as category_name,
            b.valid, b.status_code, b.last_checked_at, b.check_note,
            b.created_at, b.updated_at, b.is_deleted
     FROM bookmarks b
     LEFT JOIN categories c ON b.category_id = c.id
     WHERE b.updated_at > ?`
  ).bind(since).all();

  // 关联标签（使用优化单次 JOIN 查询，杜绝 IN 语句过长与 CPU 超时）
  const bList = (bookmarks.results || []) as any[];
  if (bList.length > 0) {
    const tagRows = await env.DB.prepare(
      `SELECT bt.bookmark_id, t.name
       FROM bookmark_tags bt
       JOIN tags t ON bt.tag_id = t.id
       JOIN bookmarks b ON bt.bookmark_id = b.id
       WHERE b.updated_at > ?`
    ).bind(since).all();

    const tagMap = new Map<number, string[]>();
    for (const r of (tagRows.results || []) as any[]) {
      if (!tagMap.has(r.bookmark_id)) tagMap.set(r.bookmark_id, []);
      tagMap.get(r.bookmark_id)!.push(r.name);
    }
    for (const b of bList) {
      b.tags = tagMap.get(b.id) || [];
    }
  }

  return jsonResponse({
    server_time: now,
    categories: categories.results || [],
    bookmarks: bList,
  });
}

// 双向增量同步推送与合并（使用 D1 batch 事务批量合并，严格控制子请求在 8 次以内，彻底杜绝 1101 CPU 超时）
async function handleSyncPush(request: Request, env: Env): Promise<Response> {
  let payload: any;
  try {
    payload = await request.json();
  } catch (e: any) {
    return jsonResponse({ error: "Invalid JSON payload: " + e.message }, 400);
  }

  const clientCategories: any[] = payload.categories || [];
  const clientBookmarks: any[] = payload.bookmarks || [];
  const now = Math.floor(Date.now() / 1000);

  try {
    // 1. 批量合并分类 (如果包含分类)
    if (clientCategories.length > 0) {
      const catNames = Array.from(new Set(clientCategories.map(c => c.name).filter(Boolean)));
      if (catNames.length > 0) {
        const catPlaceholders = catNames.map(() => "?").join(",");
        const existingCats = await env.DB.prepare(
          `SELECT id, name, updated_at FROM categories WHERE name IN (${catPlaceholders}) COLLATE NOCASE`
        ).bind(...catNames).all<any>();

        const existingCatMap = new Map<string, { id: number; updated_at: number }>();
        for (const ec of (existingCats.results || [])) {
          if (ec.name) existingCatMap.set(ec.name.toLowerCase(), { id: ec.id, updated_at: ec.updated_at });
        }

        // 建立客户端 local id -> local name 映射，用于解算本地 parent_id
        const localIdToName = new Map<number, string>();
        for (const c of clientCategories) {
          if (c.id != null && c.name) {
            localIdToName.set(c.id, c.name.toLowerCase());
          }
        }

        const catStmts: any[] = [];
        const processedNames = new Set<string>();

        // 阶段一：先写入/更新分类基础数据，parent_id 暂时全部设为 NULL，彻底避免外键约束异常
        for (const c of clientCategories) {
          if (!c.name) continue;
          const lowerName = c.name.toLowerCase();
          if (processedNames.has(lowerName)) continue;
          processedNames.add(lowerName);

          const ex = existingCatMap.get(lowerName);
          if (!ex) {
            catStmts.push(
              env.DB.prepare(
                "INSERT INTO categories (name, parent_id, sort_order, updated_at, is_deleted) VALUES (?, NULL, ?, ?, ?)"
              ).bind(c.name, c.sort_order || 0, c.updated_at || now, c.is_deleted ? 1 : 0)
            );
          } else if ((c.updated_at || 0) > ex.updated_at) {
            catStmts.push(
              env.DB.prepare(
                "UPDATE categories SET sort_order = ?, updated_at = ?, is_deleted = ? WHERE id = ?"
              ).bind(c.sort_order || 0, c.updated_at || now, c.is_deleted ? 1 : 0, ex.id)
            );
          }
        }

        if (catStmts.length > 0) {
          await env.DB.batch(catStmts);
        }

        // 阶段二：所有分类均已安全入库后，查出所有分类的远端真实 id，建立远程分类层级关联
        const allRemoteCats = await env.DB.prepare("SELECT id, name FROM categories WHERE is_deleted = 0").all<any>();
        const remoteCatNameToId = new Map<string, number>();
        for (const rc of (allRemoteCats.results || [])) {
          if (rc.name) remoteCatNameToId.set(rc.name.toLowerCase(), rc.id);
        }

        const parentUpdateStmts: any[] = [];
        for (const c of clientCategories) {
          if (!c.name) continue;
          const currentRemoteId = remoteCatNameToId.get(c.name.toLowerCase());
          if (!currentRemoteId) continue;

          let targetParentRemoteId: number | null = null;
          // 优先使用显式传参 parent_name，次选 localIdToName 转换
          const parentName = c.parent_name || (c.parent_id != null ? localIdToName.get(c.parent_id) : null);
          if (parentName) {
            targetParentRemoteId = remoteCatNameToId.get(parentName.toLowerCase()) || null;
          }

          if (targetParentRemoteId !== null) {
            parentUpdateStmts.push(
              env.DB.prepare("UPDATE categories SET parent_id = ? WHERE id = ?").bind(targetParentRemoteId, currentRemoteId)
            );
          }
        }

        if (parentUpdateStmts.length > 0) {
          await env.DB.batch(parentUpdateStmts);
        }
      }
    }

    // 2. 批量合并书签 (如果包含书签)
    if (clientBookmarks.length > 0) {
      const validBms = clientBookmarks.filter(b => !!b.url);
      if (validBms.length > 0) {
        // 建立远程分类名称 -> 远程 id 的映射表，杜绝外键错误
        const allCats = await env.DB.prepare("SELECT id, name FROM categories WHERE is_deleted = 0").all<any>();
        const catNameToId = new Map<string, number>();
        for (const c of (allCats.results || [])) {
          if (c.name) catNameToId.set(c.name.toLowerCase(), c.id);
        }

        // 去重相同批次中具有相同 urlKey 的书签，保留更新时间较新者
        const bmMapByUrlKey = new Map<string, any>();
        for (const b of validBms) {
          const key = (b.url_key || b.url).replace(/\/+$/, "");
          if (!key) continue;
          const prev = bmMapByUrlKey.get(key);
          if (!prev || (b.updated_at || 0) >= (prev.updated_at || 0)) {
            bmMapByUrlKey.set(key, b);
          }
        }

        const uniqueBms = Array.from(bmMapByUrlKey.values());
        const urlKeys = Array.from(bmMapByUrlKey.keys());

        if (urlKeys.length > 0) {
          // 一次性查出已有书签，避免 N 次查询
          const bmPlaceholders = urlKeys.map(() => "?").join(",");
          const existingBms = await env.DB.prepare(
            `SELECT id, url_key, updated_at FROM bookmarks WHERE url_key IN (${bmPlaceholders})`
          ).bind(...urlKeys).all<any>();

          const existingBmMap = new Map<string, { id: number; updated_at: number }>();
          for (const eb of (existingBms.results || [])) {
            if (eb.url_key) existingBmMap.set(eb.url_key, { id: eb.id, updated_at: eb.updated_at });
          }

          const bmStmts: any[] = [];

          for (const b of uniqueBms) {
            const urlKey = (b.url_key || b.url).replace(/\/+$/, "");
            const targetCatId = b.category_name ? (catNameToId.get(b.category_name.toLowerCase()) || null) : null;
            const ex = existingBmMap.get(urlKey);

            if (!ex) {
              bmStmts.push(
                env.DB.prepare(
                  `INSERT INTO bookmarks (title, url, url_key, icon, category_id, valid, created_at, updated_at, is_deleted)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
                ).bind(b.title || b.url, b.url, urlKey, b.icon || null, targetCatId, b.valid ?? 1, b.created_at || now, b.updated_at || now, b.is_deleted ? 1 : 0)
              );
            } else if ((b.updated_at || 0) >= ex.updated_at) {
              bmStmts.push(
                env.DB.prepare(
                  `UPDATE bookmarks SET title = ?, url = ?, icon = coalesce(?, icon), category_id = ?, valid = ?, updated_at = ?, is_deleted = ?
                   WHERE id = ?`
                ).bind(b.title || b.url, b.url, b.icon || null, targetCatId, b.valid ?? 1, b.updated_at || now, b.is_deleted ? 1 : 0, ex.id)
              );
            }
          }

          if (bmStmts.length > 0) {
            await env.DB.batch(bmStmts);
          }

          // 3. 批量合并标签
          const allTagNamesSet = new Set<string>();
          for (const b of uniqueBms) {
            if (Array.isArray(b.tags) && !b.is_deleted) {
              for (const t of b.tags) {
                const trimmed = String(t).trim();
                if (trimmed) allTagNamesSet.add(trimmed);
              }
            }
          }

          if (allTagNamesSet.size > 0) {
            const allTagNames = Array.from(allTagNamesSet);
            // 1) 批量忽略插入标签表
            const tagInsertStmts = allTagNames.map(t => env.DB.prepare("INSERT OR IGNORE INTO tags (name) VALUES (?)").bind(t));
            await env.DB.batch(tagInsertStmts);

            // 2) 一次性拉取所有标签 ID
            const tagPlaceholders = allTagNames.map(() => "?").join(",");
            const tagRows = await env.DB.prepare(
              `SELECT id, name FROM tags WHERE name IN (${tagPlaceholders}) COLLATE NOCASE`
            ).bind(...allTagNames).all<any>();

            const tagNameToId = new Map<string, number>();
            for (const tr of (tagRows.results || [])) {
              if (tr.name) tagNameToId.set(tr.name.toLowerCase(), tr.id);
            }

            // 3) 一次性拉取这批书签的最新 ID
            const allUpdatedBms = await env.DB.prepare(
              `SELECT id, url_key FROM bookmarks WHERE url_key IN (${bmPlaceholders})`
            ).bind(...urlKeys).all<any>();

            const bmKeyToId = new Map<string, number>();
            for (const rb of (allUpdatedBms.results || [])) {
              if (rb.url_key) bmKeyToId.set(rb.url_key, rb.id);
            }

            // 4) 批量插入书签标签关联
            const btStmts: any[] = [];
            for (const b of uniqueBms) {
              if (!Array.isArray(b.tags) || b.is_deleted || b.tags.length === 0) continue;
              const urlKey = (b.url_key || b.url).replace(/\/+$/, "");
              const bmId = bmKeyToId.get(urlKey);
              if (!bmId) continue;

              for (const t of b.tags) {
                const tagId = tagNameToId.get(String(t).trim().toLowerCase());
                if (tagId) {
                  btStmts.push(
                    env.DB.prepare("INSERT OR IGNORE INTO bookmark_tags (bookmark_id, tag_id) VALUES (?, ?)").bind(bmId, tagId)
                  );
                }
              }
            }

            if (btStmts.length > 0) {
              await env.DB.batch(btStmts);
            }
          }
        }
      }
    }

    return jsonResponse({
      success: true,
      server_time: now,
      message: "Sync merged successfully",
    });
  } catch (err: any) {
    return jsonResponse({
      success: false,
      error: `D1 合并执行异常: ${err.message || String(err)}`,
      stack: err.stack,
    }, 500);
  }
}

// 清空云端所有收藏与分类数据（用于全新覆盖上传前清空远端）
async function handleWipe(env: Env): Promise<Response> {
  const now = Math.floor(Date.now() / 1000);
  try {
    await env.DB.batch([
      env.DB.prepare("DELETE FROM bookmark_tags"),
      env.DB.prepare("DELETE FROM bookmarks"),
      env.DB.prepare("DELETE FROM categories"),
      env.DB.prepare("DELETE FROM tags"),
      env.DB.prepare("DELETE FROM sync_meta"),
    ]);
    return jsonResponse({
      success: true,
      message: "云端数据已彻底清空",
      server_time: now,
    });
  } catch (err: any) {
    return jsonResponse({ error: `清空云端数据失败: ${err.message}` }, 500);
  }
}

// 网页元数据抓取辅助
async function handleFetchMeta(url: URL): Promise<Response> {
  const target = url.searchParams.get("url");
  if (!target) return jsonResponse({ error: "Missing url parameter" }, 400);
  const meta = await fetchPageMeta(target);
  return jsonResponse(meta);
}

async function fetchPageMeta(targetUrl: string): Promise<{ title: string | null; icon: string | null }> {
  try {
    const res = await fetch(targetUrl, {
      headers: { "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36" },
      cf: { cacheTtl: 3600 },
    });
    const html = await res.text();
    const titleMatch = html.match(/<title[^>]*>([^<]+)<\/title>/i);
    const title = titleMatch ? titleMatch[1].trim() : null;

    let icon: string | null = null;
    const iconMatch = html.match(/<link[^>]+rel=["'](?:shortcut )?icon["'][^>]+href=["']([^"']+)["']/i);
    if (iconMatch) {
      icon = new URL(iconMatch[1], targetUrl).toString();
    } else {
      icon = new URL("/favicon.ico", targetUrl).toString();
    }
    return { title, icon };
  } catch (e) {
    return { title: null, icon: null };
  }
}

// --- 通用辅助 ---
function jsonResponse(data: any, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      ...corsHeaders(),
    },
  });
}

function corsHeaders() {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, PUT, DELETE, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, Authorization",
  };
}

function handleCors() {
  return new Response(null, { headers: corsHeaders() });
}

// --- 独立 Web 应用 HTML 渲染引擎 ---
function renderLoginPage(title: string): string {
  return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>登录 - ${title}</title>
  <script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-slate-100 flex items-center justify-center min-h-screen p-4 font-sans text-slate-800">
  <div class="bg-white rounded-2xl shadow-xl border border-slate-200/80 p-8 w-full max-w-md">
    <div class="w-12 h-12 bg-blue-600 rounded-xl flex items-center justify-center text-white text-2xl font-bold mb-4 shadow-md shadow-blue-500/20">
      🔖
    </div>
    <h1 class="text-xl font-bold text-slate-900 mb-1">${title}</h1>
    <p class="text-xs text-slate-400 mb-6">请输入访问密钥 (Access Token) 解锁您的在线网络收藏夹</p>

    <div id="error-msg" class="hidden mb-4 p-3 bg-rose-50 text-rose-700 text-xs rounded-lg border border-rose-200"></div>

    <form id="login-form" class="space-y-4">
      <div>
        <label class="block text-xs font-semibold text-slate-600 uppercase mb-1">Access Token</label>
        <input id="token-input" type="password" placeholder="请输入配置的 ACCESS_TOKEN" required
               class="w-full text-sm px-3.5 py-2.5 rounded-xl border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500">
      </div>
      <button type="submit" class="w-full py-2.5 bg-blue-600 hover:bg-blue-700 text-white font-medium text-sm rounded-xl shadow-md shadow-blue-500/20 transition">
        安全登录
      </button>
    </form>
  </div>
  <script>
    document.getElementById('login-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const token = document.getElementById('token-input').value;
      const res = await fetch('/api/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token })
      });
      const data = await res.json();
      if (res.ok && data.success) {
        window.location.reload();
      } else {
        const err = document.getElementById('error-msg');
        err.textContent = data.error || '登录失败';
        err.classList.remove('hidden');
      }
    });
  </script>
</body>
</html>`;
}

function renderWebApp(title: string, authorized: boolean): string {
  return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${title}</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <style>
    .line-clamp-1 { display: -webkit-box; -webkit-line-clamp: 1; -webkit-box-orient: vertical; overflow: hidden; }
  </style>
</head>
<body class="bg-slate-50 font-sans text-slate-800 antialiased min-h-screen flex flex-col">
  <!-- 顶部导航栏 -->
  <header class="bg-white border-b border-slate-200 sticky top-0 z-30 shadow-xs">
    <div class="max-w-7xl mx-auto px-4 h-16 flex items-center justify-between gap-4">
      <div class="flex items-center gap-3">
        <div class="w-9 h-9 rounded-xl bg-blue-600 text-white flex items-center justify-center font-bold text-lg shadow-md shadow-blue-500/20">
          🔖
        </div>
        <div>
          <h1 class="text-base font-bold text-slate-900 leading-tight">${title}</h1>
          <p class="text-[11px] text-slate-400">Cloudflare Workers + D1 独立在线版</p>
        </div>
      </div>

      <!-- 搜索框 -->
      <div class="flex-1 max-w-md mx-4">
        <div class="relative">
          <input id="search-input" type="text" placeholder="快速搜索书签标题、网址或标签..."
                 class="w-full bg-slate-100 focus:bg-white text-xs px-3.5 py-2 pl-9 rounded-xl border border-transparent focus:border-blue-500 outline-none transition">
          <span class="absolute left-3 top-2 text-slate-400 text-xs">🔍</span>
        </div>
      </div>

      <!-- 右侧操作 -->
      <div class="flex items-center gap-2">
        <button onclick="openAddModal()" class="px-3.5 py-2 bg-blue-600 hover:bg-blue-700 text-white text-xs font-semibold rounded-xl shadow-xs transition flex items-center gap-1.5 cursor-pointer">
          <span>+</span><span>添加书签</span>
        </button>
        ${authorized ? `<button onclick="logout()" class="px-3 py-2 text-slate-500 hover:text-slate-800 text-xs rounded-xl hover:bg-slate-100 transition" title="退出登录">退出</button>` : ""}
      </div>
    </div>
  </header>

  <!-- 主体区域 -->
  <div class="max-w-7xl mx-auto px-4 py-6 flex-1 flex gap-6 w-full">
    <!-- 左侧分类侧边栏 -->
    <aside class="w-56 shrink-0 hidden md:block">
      <div class="bg-white rounded-2xl border border-slate-200/80 p-3 shadow-xs sticky top-20">
        <div class="text-[11px] font-bold text-slate-400 uppercase px-2 mb-2">书签分类</div>
        <div id="category-list" class="space-y-1 text-xs">
          <button onclick="selectCategory(null)" id="cat-all" class="w-full text-left px-3 py-2 rounded-lg font-semibold bg-blue-50 text-blue-600 transition">
            📂 全部书签
          </button>
          <button onclick="selectCategory(-1)" id="cat-none" class="w-full text-left px-3 py-2 rounded-lg text-slate-600 hover:bg-slate-50 transition">
            📁 未分类书签
          </button>
          <div id="dynamic-categories" class="space-y-1"></div>
        </div>
      </div>
    </aside>

    <!-- 右侧书签列表 -->
    <main class="flex-1 min-w-0">
      <div class="flex items-center justify-between mb-4">
        <div class="text-xs text-slate-500">
          共 <span id="bookmark-count" class="font-bold text-slate-900">0</span> 条书签
        </div>
      </div>

      <div id="bookmarks-grid" class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-4">
        <div class="col-span-full py-16 text-center text-slate-400 text-sm">正在加载书签...</div>
      </div>
    </main>
  </div>

  <!-- 添加书签模态框 -->
  <div id="add-modal" class="fixed inset-0 bg-slate-900/40 backdrop-blur-xs hidden items-center justify-center p-4 z-50">
    <div class="bg-white rounded-2xl shadow-2xl border border-slate-100 max-w-md w-full p-6">
      <h3 class="text-base font-bold text-slate-900 mb-4">添加网址书签</h3>
      <form id="add-bookmark-form" class="space-y-3.5">
        <div>
          <label class="block text-xs font-medium text-slate-600 mb-1">网页 URL (必填)</label>
          <input id="bm-url" type="url" required placeholder="https://..."
                 class="w-full text-xs px-3 py-2 border border-slate-200 rounded-lg outline-none focus:border-blue-500">
        </div>
        <div>
          <label class="block text-xs font-medium text-slate-600 mb-1">网页标题 (选填，自动抓取)</label>
          <input id="bm-title" type="text" placeholder="留空时自动从网页抓取"
                 class="w-full text-xs px-3 py-2 border border-slate-200 rounded-lg outline-none focus:border-blue-500">
        </div>
        <div>
          <label class="block text-xs font-medium text-slate-600 mb-1">所属分类</label>
          <select id="bm-category" class="w-full text-xs px-3 py-2 border border-slate-200 rounded-lg outline-none bg-white">
            <option value="">-- 未分类 --</option>
          </select>
        </div>
        <div>
          <label class="block text-xs font-medium text-slate-600 mb-1">标签 (逗号分隔)</label>
          <input id="bm-tags" type="text" placeholder="工具, 编程, 推荐"
                 class="w-full text-xs px-3 py-2 border border-slate-200 rounded-lg outline-none focus:border-blue-500">
        </div>
        <div class="flex justify-end gap-2 pt-3">
          <button type="button" onclick="closeAddModal()" class="px-4 py-2 text-xs text-slate-600 hover:bg-slate-100 rounded-lg">取消</button>
          <button type="submit" id="save-btn" class="px-4 py-2 text-xs font-semibold bg-blue-600 hover:bg-blue-700 text-white rounded-lg shadow-xs">保存收藏</button>
        </div>
      </form>
    </div>
  </div>

  <script>
    let currentCategoryId = null;
    let categoriesList = [];

    async function loadData() {
      await Promise.all([loadCategories(), loadBookmarks()]);
    }

    async function loadCategories() {
      const res = await fetch('/api/categories');
      if (res.ok) {
        categoriesList = await res.json();
        const container = document.getElementById('dynamic-categories');
        const select = document.getElementById('bm-category');
        container.innerHTML = categoriesList.map(c => \`
          <button onclick="selectCategory(\${c.id})" id="cat-\${c.id}" class="w-full text-left px-3 py-2 rounded-lg text-slate-600 hover:bg-slate-50 transition flex items-center justify-between">
            <span class="truncate">📁 \${c.name}</span>
          </button>
        \`).join('');

        select.innerHTML = '<option value="">-- 未分类 --</option>' + categoriesList.map(c => \`
          <option value="\${c.id}">\${c.name}</option>
        \`).join('');
      }
    }

    async function loadBookmarks() {
      const search = document.getElementById('search-input').value.trim();
      let url = '/api/bookmarks?';
      if (search) url += 'search=' + encodeURIComponent(search) + '&';
      if (currentCategoryId !== null) url += 'category_id=' + currentCategoryId + '&';

      const res = await fetch(url);
      if (res.ok) {
        const bookmarks = await res.json();
        document.getElementById('bookmark-count').textContent = bookmarks.length;
        const grid = document.getElementById('bookmarks-grid');
        if (bookmarks.length === 0) {
          grid.innerHTML = '<div class="col-span-full py-16 text-center text-slate-400 text-sm">暂无符合条件的书签</div>';
          return;
        }
        grid.innerHTML = bookmarks.map(b => \`
          <div class="group bg-white rounded-2xl border border-slate-200/80 hover:border-blue-400 p-4 shadow-xs hover:shadow-md transition flex flex-col justify-between">
            <div>
              <div class="flex items-start gap-2.5 mb-2">
                <img src="\${b.icon || 'https://www.google.com/s2/favicons?domain=' + encodeURIComponent(b.url)}"
                     class="w-4 h-4 mt-0.5 rounded shrink-0 object-contain" onerror="this.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 24 24%22 fill=%22none%22 stroke=%22currentColor%22><circle cx=%2212%22 cy=%2212%22 r=%2210%22/></svg>'">
                <a href="\${b.url}" target="_blank" rel="noopener noreferrer" class="text-xs font-semibold text-slate-900 hover:text-blue-600 line-clamp-1 leading-tight flex-1">
                  \${b.title || b.url}
                </a>
              </div>
              <p class="text-[11px] text-slate-400 truncate mb-3">\${b.url}</p>
            </div>
            <div class="flex items-center justify-between pt-2 border-t border-slate-100 text-[10px] text-slate-400">
              <span class="bg-slate-100 text-slate-600 px-2 py-0.5 rounded">\${b.category_name || '未分类'}</span>
              <button onclick="deleteBookmark(\${b.id})" class="opacity-0 group-hover:opacity-100 text-rose-500 hover:text-rose-700 transition">删除</button>
            </div>
          </div>
        \`).join('');
      }
    }

    function selectCategory(id) {
      currentCategoryId = id;
      document.querySelectorAll('#category-list button').forEach(b => {
        b.className = 'w-full text-left px-3 py-2 rounded-lg text-slate-600 hover:bg-slate-50 transition';
      });
      const activeBtn = document.getElementById(id === null ? 'cat-all' : id === -1 ? 'cat-none' : 'cat-' + id);
      if (activeBtn) activeBtn.className = 'w-full text-left px-3 py-2 rounded-lg font-semibold bg-blue-50 text-blue-600 transition';
      loadBookmarks();
    }

    document.getElementById('search-input').addEventListener('input', () => {
      loadBookmarks();
    });

    function openAddModal() {
      document.getElementById('add-modal').classList.remove('hidden');
      document.getElementById('add-modal').classList.add('flex');
    }
    function closeAddModal() {
      document.getElementById('add-modal').classList.remove('flex');
      document.getElementById('add-modal').classList.add('hidden');
    }

    document.getElementById('add-bookmark-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const saveBtn = document.getElementById('save-btn');
      saveBtn.textContent = '保存中...';
      saveBtn.disabled = true;

      const url = document.getElementById('bm-url').value;
      const title = document.getElementById('bm-title').value;
      const category_id = document.getElementById('bm-category').value ? parseInt(document.getElementById('bm-category').value, 10) : null;
      const tags = document.getElementById('bm-tags').value.split(',').map(s => s.trim()).filter(Boolean);

      try {
        const res = await fetch('/api/bookmarks', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ url, title, category_id, tags })
        });
        if (res.ok) {
          closeAddModal();
          document.getElementById('add-bookmark-form').reset();
          loadBookmarks();
        } else {
          alert('添加失败');
        }
      } finally {
        saveBtn.textContent = '保存收藏';
        saveBtn.disabled = false;
      }
    });

    async function deleteBookmark(id) {
      if (confirm('确认删除该书签吗？')) {
        await fetch('/api/bookmarks/' + id, { method: 'DELETE' });
        loadBookmarks();
      }
    }

    function logout() {
      document.cookie = 'cf_bmk_token=; Path=/; Expires=Thu, 01 Jan 1970 00:00:01 GMT;';
      window.location.reload();
    }

    loadData();
  </script>
</body>
</html>`;
}

