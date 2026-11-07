<script setup lang="ts">
import { ref, reactive, onMounted } from "vue";
import { invoke } from "@tauri-apps/api/core";
import { openUrl } from "@tauri-apps/plugin-opener";
import {
  Bookmark as BookmarkIcon,
  Folder,
  FolderPlus,
  Tag as TagIcon,
  Search,
  Plus,
  Trash2,
  Edit3,
  ExternalLink,
  CheckCircle2,
  XCircle,
  HelpCircle,
  ShieldAlert,
  Copy,
  RefreshCw,
  FolderCheck,
  LayoutGrid,
  Upload,
  Download,
  CopyCheck,
  Sparkles,
  PlayCircle,
  Loader2,
  Bot,
  FilterX,
  Globe,
  Wifi,
} from "@lucide/vue";

// 类型定义与 Rust 端对齐
interface Category {
  id: number;
  name: string;
  parent_id: number | null;
}

interface Tag {
  id: number;
  name: string;
}

interface Bookmark {
  id: number;
  title: string;
  url: string;
  url_key: string;
  icon_data_url: string | null;
  category_id: number | null;
  category_name: string | null;
  valid: number | null;
  status_code: number | null;
  last_checked_at: number | null;
  check_note: string | null;
  tags: string[];
  created_at: number;
  updated_at: number;
}

interface AiSuggestion {
  category_name: string;
  tags: string[];
}

interface ProxyConfig {
  mode: string; // "direct", "system", "custom"
  url: string | null;
}

interface ProxyTestResult {
  success: boolean;
  latency_ms: number | null;
  error: string | null;
}

// 状态管理
const categories = ref<Category[]>([]);
const tags = ref<Tag[]>([]);
const bookmarks = ref<Bookmark[]>([]);
const failureReasons = ref<string[]>([]);
const loading = ref(false);
const duplicateCount = ref(0);
const isBatchChecking = ref(false);

const filter = reactive({
  search: "",
  categoryId: null as number | null,
  tag: null as string | null,
  validState: "all",
  checkNote: null as string | null,
  sortBy: "created_desc",
});

// 模态框状态
const isAddEditModalOpen = ref(false);
const editingBookmarkId = ref<number | null>(null);
const isFetchingMeta = ref(false);
const fetchErrorTip = ref("");
const aiSuggestion = ref<AiSuggestion | null>(null);

const bookmarkForm = reactive({
  title: "",
  url: "",
  iconDataUrl: null as string | null,
  categoryId: null as number | null,
  tagsInput: "",
});

const isCategoryModalOpen = ref(false);
const newCategoryName = ref("");

// 代理设置弹窗与状态
const isProxyModalOpen = ref(false);
const isTestingProxy = ref(false);
const proxyTestResult = ref<ProxyTestResult | null>(null);
const proxyForm = reactive<ProxyConfig>({
  mode: "system",
  url: "",
});

// 文件导入引用
const fileInputRef = ref<HTMLInputElement | null>(null);

// 加载基础数据
async function loadCategories() {
  try {
    categories.value = await invoke("get_categories");
  } catch (err) {
    console.error("Failed to load categories:", err);
  }
}

async function loadTags() {
  try {
    tags.value = await invoke("get_tags");
  } catch (err) {
    console.error("Failed to load tags:", err);
  }
}

async function loadFailureReasons() {
  try {
    failureReasons.value = await invoke("get_failure_reasons");
  } catch (err) {
    console.error("Failed to load failure reasons:", err);
  }
}

async function loadProxySettings() {
  try {
    const cfg: ProxyConfig = await invoke("get_proxy_settings");
    proxyForm.mode = cfg.mode || "system";
    proxyForm.url = cfg.url || "";
  } catch (err) {
    console.error("Failed to load proxy settings:", err);
  }
}

async function checkDuplicatesCount() {
  try {
    const dups: Bookmark[][] = await invoke("find_duplicates");
    duplicateCount.value = dups.reduce((acc, g) => acc + (g.length - 1), 0);
  } catch (err) {
    console.error(err);
  }
}

// 核心筛选加载函数
async function loadBookmarks() {
  loading.value = true;
  try {
    bookmarks.value = await invoke("list_bookmarks", {
      filter: {
        search: filter.search || null,
        category_id: filter.categoryId,
        tag: filter.tag,
        valid_state: filter.validState === "all" ? null : filter.validState,
        check_note: filter.checkNote,
        sort_by: filter.sortBy,
      },
    });
  } catch (err) {
    console.error("Failed to load bookmarks:", err);
  } finally {
    loading.value = false;
  }
}

async function refreshAll() {
  await Promise.all([
    loadCategories(),
    loadTags(),
    loadFailureReasons(),
    loadBookmarks(),
    loadProxySettings(),
    checkDuplicatesCount(),
  ]);
}

// 筛选切换
function setCategory(id: number | null) {
  filter.categoryId = id;
  loadBookmarks();
}

function setTag(tagName: string | null) {
  filter.tag = tagName;
  loadBookmarks();
}

function setValidState(state: string) {
  filter.validState = state;
  if (state !== "invalid") {
    filter.checkNote = null;
  }
  loadBookmarks();
}

function clearAllFilters() {
  filter.search = "";
  filter.categoryId = null;
  filter.tag = null;
  filter.validState = "all";
  filter.checkNote = null;
  loadBookmarks();
}

// 打开外部链接
async function handleOpenUrl(url: string) {
  try {
    await openUrl(url);
  } catch (e) {
    window.open(url, "_blank");
  }
}

// 复制链接
async function copyUrl(url: string) {
  try {
    await navigator.clipboard.writeText(url);
    alert("已复制链接到剪贴板！");
  } catch (e) {
    console.error(e);
  }
}

// 智能自动抓取元数据（包含 Favicon 图标与 AI 建议分类/标签）
function handlePaste() {
  setTimeout(handleUrlBlurOrPaste, 120);
}

async function handleUrlBlurOrPaste() {
  const url = bookmarkForm.url.trim();
  if (!url || isFetchingMeta.value) return;

  if (bookmarkForm.title.trim()) return;

  isFetchingMeta.value = true;
  fetchErrorTip.value = "";
  try {
    const res: { title: string | null; favicon_data_url: string | null } = await invoke("fetch_metadata", { url });
    if (res.title && !bookmarkForm.title.trim()) {
      bookmarkForm.title = res.title;
    }
    if (res.favicon_data_url) {
      bookmarkForm.iconDataUrl = res.favicon_data_url;
    }
    await triggerAiSuggestion(url, bookmarkForm.title || res.title || url);
  } catch (e) {
    console.warn("抓取提示:", e);
    fetchErrorTip.value = "若访问受限或超时，请在右上角【代理设置】中配置代理";
  } finally {
    isFetchingMeta.value = false;
  }
}

// AI 建议
async function triggerAiSuggestion(url: string, title: string) {
  try {
    const suggestion: AiSuggestion = await invoke("suggest_ai_metadata", { url, title });
    aiSuggestion.value = suggestion;

    if (bookmarkForm.categoryId === null) {
      const matchCat = categories.value.find((c) => c.name.toLowerCase() === suggestion.category_name.toLowerCase());
      if (matchCat) {
        bookmarkForm.categoryId = matchCat.id;
      }
    }

    if (!bookmarkForm.tagsInput.trim()) {
      bookmarkForm.tagsInput = suggestion.tags.join(", ");
    }
  } catch (e) {
    console.error("AI 建议生成失败:", e);
  }
}

function applyAiSuggestion() {
  if (!aiSuggestion.value) return;
  const matchCat = categories.value.find(
    (c) => c.name.toLowerCase() === aiSuggestion.value!.category_name.toLowerCase()
  );
  if (matchCat) {
    bookmarkForm.categoryId = matchCat.id;
  }
  bookmarkForm.tagsInput = aiSuggestion.value.tags.join(", ");
}

// 手动强制重新抓取
async function forceFetchMeta() {
  const url = bookmarkForm.url.trim();
  if (!url) {
    alert("请先填写网址 URL");
    return;
  }
  isFetchingMeta.value = true;
  fetchErrorTip.value = "";
  try {
    const res: { title: string | null; favicon_data_url: string | null } = await invoke("fetch_metadata", { url });
    if (res.title) {
      bookmarkForm.title = res.title;
    }
    if (res.favicon_data_url) {
      bookmarkForm.iconDataUrl = res.favicon_data_url;
    }
    await triggerAiSuggestion(url, bookmarkForm.title);
  } catch (e) {
    fetchErrorTip.value = "抓取失败: " + e + " (可尝试在右上角配置代理)";
  } finally {
    isFetchingMeta.value = false;
  }
}

// 新增 / 编辑书签
function openCreateModal() {
  editingBookmarkId.value = null;
  aiSuggestion.value = null;
  fetchErrorTip.value = "";
  bookmarkForm.title = "";
  bookmarkForm.url = "";
  bookmarkForm.iconDataUrl = null;
  bookmarkForm.categoryId = filter.categoryId && filter.categoryId > 0 ? filter.categoryId : null;
  bookmarkForm.tagsInput = "";
  isAddEditModalOpen.value = true;
}

function openEditModal(bm: Bookmark) {
  editingBookmarkId.value = bm.id;
  aiSuggestion.value = null;
  fetchErrorTip.value = "";
  bookmarkForm.title = bm.title;
  bookmarkForm.url = bm.url;
  bookmarkForm.iconDataUrl = bm.icon_data_url;
  bookmarkForm.categoryId = bm.category_id;
  bookmarkForm.tagsInput = bm.tags.join(", ");
  isAddEditModalOpen.value = true;
}

async function submitBookmark() {
  if (!bookmarkForm.url.trim()) {
    alert("请输入有效的网址");
    return;
  }
  const tags = bookmarkForm.tagsInput
    .split(/[,， ]+/)
    .map((s) => s.trim())
    .filter(Boolean);

  try {
    if (editingBookmarkId.value === null) {
      await invoke("create_bookmark", {
        input: {
          title: bookmarkForm.title.trim(),
          url: bookmarkForm.url.trim(),
          category_id: bookmarkForm.categoryId,
          tags,
          icon_data_url: bookmarkForm.iconDataUrl,
        },
      });
    } else {
      await invoke("update_bookmark", {
        input: {
          id: editingBookmarkId.value,
          title: bookmarkForm.title.trim() || bookmarkForm.url.trim(),
          url: bookmarkForm.url.trim(),
          category_id: bookmarkForm.categoryId,
          tags,
          icon_data_url: bookmarkForm.iconDataUrl,
        },
      });
    }
    isAddEditModalOpen.value = false;
    refreshAll();
  } catch (err) {
    alert("保存失败: " + err);
  }
}

async function handleDeleteBookmark(id: number) {
  if (confirm("确定要删除这条书签记录吗？")) {
    try {
      await invoke("delete_bookmark", { id });
      refreshAll();
    } catch (err) {
      alert("删除失败: " + err);
    }
  }
}

// 代理设置与即时测试
function openProxyModal() {
  proxyTestResult.value = null;
  isProxyModalOpen.value = true;
}

async function handleTestProxy() {
  const url = proxyForm.url?.trim() || "";
  if (!url) {
    alert("请先填写代理服务器地址，如 http://127.0.0.1:7890 或 socks5://127.0.0.1:10808");
    return;
  }
  isTestingProxy.value = true;
  proxyTestResult.value = null;
  try {
    const res: ProxyTestResult = await invoke("test_proxy", { proxyUrl: url });
    proxyTestResult.value = res;
  } catch (err) {
    proxyTestResult.value = {
      success: false,
      latency_ms: null,
      error: String(err),
    };
  } finally {
    isTestingProxy.value = false;
  }
}

async function saveProxyConfig() {
  try {
    await invoke("save_proxy_settings", {
      config: {
        mode: proxyForm.mode,
        url: proxyForm.mode === "custom" ? proxyForm.url?.trim() || null : null,
      },
    });
    isProxyModalOpen.value = false;
    alert("代理设置已保存并立即全局生效！");
  } catch (err) {
    alert("保存失败: " + err);
  }
}

// 高并发批量死链与反爬检测
async function runBatchValidation() {
  if (bookmarks.value.length === 0) {
    alert("当前无书签需要检测");
    return;
  }
  if (!confirm(`将对当前列表中的 ${bookmarks.value.length} 条书签进行并发可用性校验，是否继续？`)) {
    return;
  }

  isBatchChecking.value = true;
  try {
    const targets = bookmarks.value.map((bm) => ({
      id: bm.id,
      url: bm.url,
      current_valid: bm.valid,
    }));

    const results: Array<{ id: number; valid: number | null; status_code: number; check_note: string | null }> =
      await invoke("check_bookmarks_batch", { targets });

    const validCount = results.filter((r) => r.valid === 1).length;
    const invalidCount = results.filter((r) => r.valid === 0).length;
    const blockedCount = results.filter((r) => r.check_note === "blocked").length;

    await refreshAll();
    alert(`批量校验完成！\n- 有效: ${validCount} 条\n- 失效: ${invalidCount} 条\n- 待复测 (反爬拦截): ${blockedCount} 条`);
  } catch (err) {
    alert("检测失败: " + err);
  } finally {
    isBatchChecking.value = false;
  }
}

// 一键查重合并
async function handleMergeDuplicates() {
  if (duplicateCount.value === 0) {
    alert("暂无重复书签");
    return;
  }
  if (confirm(`检测到约 ${duplicateCount.value} 条重复书签，合并将保留最早添加记录并自动合并标签，是否继续？`)) {
    try {
      const mergedCount: number = await invoke("merge_duplicates");
      alert(`成功合并并清理了 ${mergedCount} 条重复书签！`);
      refreshAll();
    } catch (e) {
      alert("合并失败: " + e);
    }
  }
}

// 导出标准 Netscape HTML
async function handleExportHtml() {
  try {
    const html: string = await invoke("export_bookmarks_html");
    const blob = new Blob([html], { type: "text/html;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `bookmarks_${new Date().toISOString().slice(0, 10)}.html`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    alert("导出失败: " + e);
  }
}

// 导入标准 Netscape HTML
function triggerImportFile() {
  fileInputRef.value?.click();
}

async function handleFileSelected(event: Event) {
  const target = event.target as HTMLInputElement;
  const file = target.files?.[0];
  if (!file) return;

  const reader = new FileReader();
  reader.onload = async (e) => {
    const content = e.target?.result as string;
    try {
      const count: number = await invoke("import_bookmarks_html", { content });
      alert(`导入成功！共导入 ${count} 条书签及分类结构。`);
      refreshAll();
    } catch (err) {
      alert("导入失败: " + err);
    } finally {
      target.value = "";
    }
  };
  reader.readAsText(file);
}

// 分类管理
function openCreateCategoryModal() {
  newCategoryName.value = "";
  isCategoryModalOpen.value = true;
}

async function submitCategory() {
  const name = newCategoryName.value.trim();
  if (!name) return;
  try {
    await invoke("create_category", { name, parent_id: null });
    isCategoryModalOpen.value = false;
    await loadCategories();
  } catch (err) {
    alert("创建分类失败: " + err);
  }
}

async function handleDeleteCategory(id: number, e: Event) {
  e.stopPropagation();
  if (confirm("删除该分类会将该分类下的书签移至未分类，确认删除吗？")) {
    try {
      await invoke("delete_category", { id });
      if (filter.categoryId === id) {
        filter.categoryId = null;
      }
      refreshAll();
    } catch (err) {
      alert("删除分类失败: " + err);
    }
  }
}

function formatDate(ts: number) {
  return new Date(ts * 1000).toLocaleDateString();
}

onMounted(() => {
  refreshAll();
});
</script>

<template>
  <div class="flex h-screen w-screen overflow-hidden bg-slate-50 text-slate-800">
    <!-- 隐藏的文件上传 input -->
    <input
      type="file"
      ref="fileInputRef"
      @change="handleFileSelected"
      accept=".html,.htm"
      class="hidden"
    />

    <!-- 左侧边栏 -->
    <aside class="w-64 border-r border-slate-200 bg-white flex flex-col shrink-0 select-none">
      <!-- 品牌 Logo -->
      <div class="h-16 flex items-center px-5 border-b border-slate-100 gap-3">
        <div class="w-9 h-9 rounded-xl bg-blue-600 text-white flex items-center justify-center shadow-md shadow-blue-500/20">
          <BookmarkIcon class="w-5 h-5" />
        </div>
        <div>
          <h1 class="text-base font-bold text-slate-900 leading-tight">书签管理器</h1>
          <p class="text-xs text-slate-400">Tauri 2.0 + Rust + Vue</p>
        </div>
      </div>

      <!-- 快捷视图列表 -->
      <div class="p-3 border-b border-slate-100 space-y-1">
        <button
          @click="setCategory(null); setValidState('all')"
          :class="[
            'w-full flex items-center justify-between px-3 py-2 rounded-lg text-sm font-medium transition cursor-pointer',
            filter.categoryId === null && filter.validState === 'all' && filter.tag === null
              ? 'bg-blue-50 text-blue-600 font-semibold'
              : 'text-slate-600 hover:bg-slate-100',
          ]"
        >
          <span class="flex items-center gap-2.5">
            <LayoutGrid class="w-4 h-4" /> 全部书签
          </span>
          <span class="text-xs text-slate-400 font-normal">{{ bookmarks.length }}</span>
        </button>

        <button
          @click="setCategory(-1)"
          :class="[
            'w-full flex items-center px-3 py-2 rounded-lg text-sm font-medium transition gap-2.5 cursor-pointer',
            filter.categoryId === -1 ? 'bg-blue-50 text-blue-600 font-semibold' : 'text-slate-600 hover:bg-slate-100',
          ]"
        >
          <Folder class="w-4 h-4 text-slate-400" /> 未分类书签
        </button>

        <!-- 专属筛选：AI 辅助分类书签 -->
        <button
          @click="setTag('ai辅助')"
          :class="[
            'w-full flex items-center px-3 py-2 rounded-lg text-sm font-medium transition gap-2.5 cursor-pointer',
            filter.tag === 'ai辅助' ? 'bg-indigo-50 text-indigo-700 font-semibold' : 'text-slate-600 hover:bg-slate-100',
          ]"
        >
          <Bot class="w-4 h-4 text-indigo-500" /> 🤖 AI 辅助分类
        </button>

        <!-- 筛选：待复测 (反爬标记 403/429) -->
        <button
          @click="setValidState('blocked')"
          :class="[
            'w-full flex items-center px-3 py-2 rounded-lg text-sm font-medium transition gap-2.5 cursor-pointer',
            filter.validState === 'blocked' ? 'bg-amber-50 text-amber-700 font-semibold' : 'text-slate-600 hover:bg-slate-100',
          ]"
        >
          <ShieldAlert class="w-4 h-4 text-amber-500" /> 待复测 (反爬拦截)
        </button>

        <!-- 筛选：失效书签 -->
        <button
          @click="setValidState('invalid')"
          :class="[
            'w-full flex items-center px-3 py-2 rounded-lg text-sm font-medium transition gap-2.5 cursor-pointer',
            filter.validState === 'invalid' ? 'bg-rose-50 text-rose-700 font-semibold' : 'text-slate-600 hover:bg-slate-100',
          ]"
        >
          <XCircle class="w-4 h-4 text-rose-500" /> ❌ 失效死链
        </button>
      </div>

      <!-- 分类树 -->
      <div class="flex-1 overflow-y-auto p-3">
        <div class="flex items-center justify-between px-2 mb-2 text-xs font-semibold text-slate-400 uppercase tracking-wider">
          <span>书签分类</span>
          <button @click="openCreateCategoryModal" class="p-1 hover:text-blue-600 rounded transition cursor-pointer" title="新建分类">
            <FolderPlus class="w-4 h-4" />
          </button>
        </div>

        <div class="space-y-0.5">
          <div
            v-for="cat in categories"
            :key="cat.id"
            @click="setCategory(cat.id)"
            :class="[
              'group flex items-center justify-between px-3 py-2 rounded-lg text-sm cursor-pointer transition',
              filter.categoryId === cat.id ? 'bg-blue-50 text-blue-600 font-semibold' : 'text-slate-600 hover:bg-slate-100',
            ]"
          >
            <span class="flex items-center gap-2.5 truncate">
              <FolderCheck v-if="filter.categoryId === cat.id" class="w-4 h-4 text-blue-600 shrink-0" />
              <Folder v-else class="w-4 h-4 text-slate-400 group-hover:text-slate-500 shrink-0" />
              <span class="truncate">{{ cat.name }}</span>
            </span>
            <button
              @click="(e) => handleDeleteCategory(cat.id, e)"
              class="opacity-0 group-hover:opacity-100 text-slate-300 hover:text-red-500 p-1 rounded transition cursor-pointer"
              title="删除分类"
            >
              <Trash2 class="w-3.5 h-3.5" />
            </button>
          </div>
          <div v-if="categories.length === 0" class="text-xs text-slate-400 px-3 py-2 italic">
            暂无自定义分类
          </div>
        </div>

        <!-- 标签云 -->
        <div class="mt-6">
          <div class="px-2 mb-2 text-xs font-semibold text-slate-400 uppercase tracking-wider">
            常用标签
          </div>
          <div class="flex flex-wrap gap-1.5 px-2">
            <button
              v-for="t in tags"
              :key="t.id"
              @click="setTag(filter.tag === t.name ? null : t.name)"
              :class="[
                'text-xs px-2.5 py-1 rounded-full border transition flex items-center gap-1 cursor-pointer',
                filter.tag === t.name
                  ? 'bg-blue-600 text-white border-blue-600 shadow-sm'
                  : 'bg-slate-50 text-slate-600 border-slate-200 hover:border-slate-300',
              ]"
            >
              <TagIcon class="w-3 h-3" />
              {{ t.name }}
            </button>
            <div v-if="tags.length === 0" class="text-xs text-slate-400 italic">
              暂无标签
            </div>
          </div>
        </div>
      </div>

      <!-- 底部导入/导出操作 -->
      <div class="p-3 border-t border-slate-100 flex items-center gap-2 bg-slate-50/50">
        <button
          @click="triggerImportFile"
          class="flex-1 flex items-center justify-center gap-1.5 py-2 px-3 text-xs font-medium text-slate-600 bg-white border border-slate-200 rounded-lg hover:bg-slate-100 transition shadow-2xs cursor-pointer"
          title="从 Chrome/Edge 导入标准 HTML 书签"
        >
          <Upload class="w-3.5 h-3.5" /> 导入书签
        </button>
        <button
          @click="handleExportHtml"
          class="flex-1 flex items-center justify-center gap-1.5 py-2 px-3 text-xs font-medium text-slate-600 bg-white border border-slate-200 rounded-lg hover:bg-slate-100 transition shadow-2xs cursor-pointer"
          title="导出为标准 Netscape HTML 书签"
        >
          <Download class="w-3.5 h-3.5" /> 导出书签
        </button>
      </div>
    </aside>

    <!-- 右侧主界面 -->
    <main class="flex-1 flex flex-col min-w-0 bg-slate-50">
      <!-- 顶部综合操作与过滤工具栏 -->
      <header class="h-16 bg-white border-b border-slate-200 px-6 flex items-center justify-between gap-4 shrink-0">
        <!-- 搜索框（支持搜索网址、标题、失效原因） -->
        <div class="relative w-80">
          <Search class="w-4 h-4 text-slate-400 absolute left-3 top-1/2 -translate-y-1/2 pointer-events-none" />
          <input
            v-model="filter.search"
            @input="loadBookmarks"
            type="text"
            placeholder="搜索网址、标题、或失效原因..."
            class="w-full bg-slate-100 hover:bg-slate-200/60 focus:bg-white text-sm pl-9 pr-4 py-2 rounded-lg border border-transparent focus:border-blue-500 focus:outline-none transition"
          />
        </div>

        <!-- 综合筛选与操作区 -->
        <div class="flex items-center gap-2.5">
          <!-- 状态过滤选择器 -->
          <select
            v-model="filter.validState"
            @change="setValidState(filter.validState)"
            class="bg-white border border-slate-200 text-xs rounded-lg px-2.5 py-2 text-slate-700 focus:outline-none focus:border-blue-500"
          >
            <option value="all">全部状态</option>
            <option value="valid">✅ 仅有效</option>
            <option value="invalid">❌ 仅失效</option>
            <option value="blocked">⚠️ 仅待复测 (反爬)</option>
            <option value="unknown">❓ 仅未校验</option>
          </select>

          <!-- 当选择了“仅失效”时，支持精准选择失效原因！ -->
          <select
            v-if="filter.validState === 'invalid'"
            v-model="filter.checkNote"
            @change="loadBookmarks"
            class="bg-rose-50 border border-rose-200 text-xs rounded-lg px-2.5 py-2 text-rose-700 focus:outline-none focus:border-rose-500"
          >
            <option :value="null">所有失效原因</option>
            <option v-for="r in failureReasons" :key="r" :value="r">
              原因: {{ r }}
            </option>
          </select>

          <!-- 排序 -->
          <select
            v-model="filter.sortBy"
            @change="loadBookmarks"
            class="bg-white border border-slate-200 text-xs rounded-lg px-2.5 py-2 text-slate-600 focus:outline-none focus:border-blue-500"
          >
            <option value="created_desc">添加时间 (最新优先)</option>
            <option value="created_asc">添加时间 (最早优先)</option>
            <option value="title_asc">标题 (A-Z)</option>
            <option value="url_asc">域名/网址 (A-Z)</option>
          </select>

          <!-- 重复书签合并提示按钮 -->
          <button
            v-if="duplicateCount > 0"
            @click="handleMergeDuplicates"
            class="flex items-center gap-1.5 bg-purple-50 text-purple-700 border border-purple-200 text-xs font-semibold px-3 py-2 rounded-lg hover:bg-purple-100 transition cursor-pointer"
            title="点击合并重复书签（保留最早添加并合并标签）"
          >
            <CopyCheck class="w-4 h-4 text-purple-600" />
            <span>合并重复 ({{ duplicateCount }})</span>
          </button>

          <!-- 批量校验按钮 -->
          <button
            @click="runBatchValidation"
            :disabled="isBatchChecking"
            class="flex items-center gap-1.5 bg-slate-100 hover:bg-slate-200 text-slate-700 text-xs font-medium px-3 py-2 rounded-lg transition disabled:opacity-50 cursor-pointer"
            title="并发检测书签死链与反爬状态"
          >
            <Loader2 v-if="isBatchChecking" class="w-4 h-4 text-blue-600 animate-spin" />
            <PlayCircle v-else class="w-4 h-4 text-blue-600" />
            <span>{{ isBatchChecking ? "高并发检测中..." : "批量校验" }}</span>
          </button>

          <!-- 网络代理设置按钮 -->
          <button
            @click="openProxyModal"
            class="relative flex items-center gap-1.5 bg-slate-100 hover:bg-slate-200 text-slate-700 text-xs font-medium px-3 py-2 rounded-lg transition cursor-pointer"
            title="网络代理设置 (解决境外与受限网站访问)"
          >
            <Wifi class="w-4 h-4 text-slate-600" />
            <span>代理设置</span>
            <!-- 工作指示灯：若自定义代理则显示小绿点 -->
            <span
              v-if="proxyForm.mode === 'custom'"
              class="w-2 h-2 rounded-full bg-emerald-500 absolute -top-0.5 -right-0.5"
              title="当前自定义代理已开启"
            ></span>
          </button>

          <!-- 刷新 -->
          <button
            @click="refreshAll"
            class="p-2 border border-slate-200 hover:bg-slate-100 rounded-lg text-slate-600 transition cursor-pointer"
            title="刷新"
          >
            <RefreshCw :class="['w-4 h-4', loading ? 'animate-spin text-blue-600' : '']" />
          </button>

          <!-- 新建书签按钮 -->
          <button
            @click="openCreateModal"
            class="flex items-center gap-2 bg-blue-600 hover:bg-blue-700 text-white text-sm font-medium px-4 py-2 rounded-lg shadow-sm shadow-blue-500/20 transition cursor-pointer"
          >
            <Plus class="w-4 h-4" />
            <span>添加书签</span>
          </button>
        </div>
      </header>

      <!-- 筛选激活状态条（显示当前筛选标签，支持一键清空） -->
      <div
        v-if="filter.categoryId !== null || filter.tag !== null || filter.validState !== 'all' || filter.checkNote !== null || filter.search"
        class="bg-blue-50/70 border-b border-blue-100 px-6 py-2 flex items-center justify-between text-xs text-blue-800"
      >
        <div class="flex items-center gap-2 flex-wrap">
          <span class="font-semibold text-blue-900">当前筛选：</span>
          <span v-if="filter.categoryId !== null" class="bg-white px-2 py-0.5 rounded border border-blue-200">
            分类: {{ categories.find((c) => c.id === filter.categoryId)?.name || "未分类" }}
          </span>
          <span v-if="filter.tag !== null" class="bg-white px-2 py-0.5 rounded border border-blue-200">
            标签: #{{ filter.tag }}
          </span>
          <span v-if="filter.validState !== 'all'" class="bg-white px-2 py-0.5 rounded border border-blue-200">
            状态: {{ filter.validState === "valid" ? "有效" : filter.validState === "invalid" ? "失效" : filter.validState === "blocked" ? "待复测" : "未测" }}
          </span>
          <span v-if="filter.checkNote" class="bg-white px-2 py-0.5 rounded border border-rose-300 text-rose-700">
            原因: {{ filter.checkNote }}
          </span>
          <span v-if="filter.search" class="bg-white px-2 py-0.5 rounded border border-blue-200">
            搜索: "{{ filter.search }}"
          </span>
          <span class="text-slate-400">（匹配到 {{ bookmarks.length }} 条）</span>
        </div>
        <button
          @click="clearAllFilters"
          class="flex items-center gap-1 text-blue-600 hover:text-blue-800 font-medium cursor-pointer"
        >
          <FilterX class="w-3.5 h-3.5" /> 重置所有筛选
        </button>
      </div>

      <!-- 书签列表区 -->
      <section class="flex-1 overflow-y-auto p-6">
        <div v-if="loading && bookmarks.length === 0" class="flex items-center justify-center h-64 text-slate-400">
          加载中...
        </div>

        <div v-else-if="bookmarks.length === 0" class="flex flex-col items-center justify-center h-80 text-center">
          <div class="w-16 h-16 rounded-2xl bg-slate-100 flex items-center justify-center text-slate-400 mb-4">
            <BookmarkIcon class="w-8 h-8" />
          </div>
          <p class="text-base font-semibold text-slate-700 mb-1">未找到符合当前条件的书签</p>
          <p class="text-sm text-slate-400 mb-4">请尝试调整筛选选项或搜索关键词</p>
          <button
            @click="clearAllFilters"
            class="px-4 py-2 bg-slate-200 hover:bg-slate-300 text-slate-700 text-sm font-medium rounded-lg transition cursor-pointer"
          >
            重置所有筛选
          </button>
        </div>

        <!-- 书签列表卡片 -->
        <div v-else class="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-4">
          <div
            v-for="bm in bookmarks"
            :key="bm.id"
            class="group bg-white rounded-xl border border-slate-200/80 hover:border-blue-400/80 p-4 shadow-sm hover:shadow-md transition-all flex flex-col justify-between"
          >
            <div>
              <!-- 头部：Favicon 图标 + 标题 + 状态 -->
              <div class="flex items-start justify-between gap-2.5 mb-2">
                <div class="flex items-start gap-2.5 flex-1 min-w-0">
                  <div class="w-5 h-5 mt-0.5 shrink-0 rounded flex items-center justify-center bg-slate-100 overflow-hidden">
                    <img
                      v-if="bm.icon_data_url"
                      :src="bm.icon_data_url"
                      class="w-full h-full object-contain"
                      alt="favicon"
                      @error="bm.icon_data_url = null"
                    />
                    <Globe v-else class="w-3.5 h-3.5 text-slate-400" />
                  </div>

                  <h3
                    @click="handleOpenUrl(bm.url)"
                    class="font-semibold text-slate-900 text-sm line-clamp-1 hover:text-blue-600 cursor-pointer flex-1"
                    :title="bm.title"
                  >
                    {{ bm.title }}
                  </h3>
                </div>

                <!-- 有效性 Badge（包含具体失效原因展示） -->
                <span v-if="bm.valid === 1" class="shrink-0 flex items-center gap-1 text-[11px] font-medium text-emerald-600 bg-emerald-50 px-2 py-0.5 rounded-full">
                  <CheckCircle2 class="w-3 h-3" /> 有效
                </span>
                <span
                  v-else-if="bm.valid === 0"
                  class="shrink-0 flex items-center gap-1 text-[11px] font-medium text-rose-600 bg-rose-50 px-2 py-0.5 rounded-full"
                  :title="bm.check_note ? '失效原因: ' + bm.check_note : '失效死链'"
                >
                  <XCircle class="w-3 h-3" /> {{ bm.check_note || "失效" }}
                </span>
                <span v-else-if="bm.check_note === 'blocked'" class="shrink-0 flex items-center gap-1 text-[11px] font-medium text-amber-700 bg-amber-50 px-2 py-0.5 rounded-full" title="403/429 拦截，需配置代理重测">
                  <ShieldAlert class="w-3 h-3" /> 待复测
                </span>
                <span v-else class="shrink-0 flex items-center gap-1 text-[11px] font-medium text-slate-400 bg-slate-100 px-2 py-0.5 rounded-full">
                  <HelpCircle class="w-3 h-3" /> 未测
                </span>
              </div>

              <!-- URL 归一化展示 -->
              <div class="text-xs text-slate-400 truncate mb-3 select-all">
                {{ bm.url }}
              </div>

              <!-- 分类与标签 -->
              <div class="flex flex-wrap items-center gap-1.5 mb-3">
                <span v-if="bm.category_name" class="inline-flex items-center gap-1 text-xs bg-slate-100 text-slate-600 px-2 py-0.5 rounded">
                  <Folder class="w-3 h-3 text-slate-400" />
                  {{ bm.category_name }}
                </span>
                <span
                  v-for="tag in bm.tags"
                  :key="tag"
                  :class="[
                    'inline-flex items-center gap-1 text-xs px-2 py-0.5 rounded',
                    tag === 'ai辅助'
                      ? 'bg-indigo-50 text-indigo-700 font-semibold border border-indigo-200'
                      : 'bg-blue-50 text-blue-600',
                  ]"
                >
                  <Bot v-if="tag === 'ai辅助'" class="w-3 h-3 text-indigo-500" />
                  #{{ tag }}
                </span>
              </div>
            </div>

            <!-- 卡片底部：时间与快捷操作 -->
            <div class="pt-3 border-t border-slate-100 flex items-center justify-between text-xs text-slate-400">
              <span>{{ formatDate(bm.created_at) }}</span>
              <div class="flex items-center gap-1 opacity-60 group-hover:opacity-100 transition">
                <button
                  @click="copyUrl(bm.url)"
                  class="p-1.5 hover:text-blue-600 hover:bg-slate-100 rounded transition cursor-pointer"
                  title="复制链接"
                >
                  <Copy class="w-3.5 h-3.5" />
                </button>
                <button
                  @click="openEditModal(bm)"
                  class="p-1.5 hover:text-blue-600 hover:bg-slate-100 rounded transition cursor-pointer"
                  title="编辑"
                >
                  <Edit3 class="w-3.5 h-3.5" />
                </button>
                <button
                  @click="handleDeleteBookmark(bm.id)"
                  class="p-1.5 hover:text-red-600 hover:bg-red-50 rounded transition cursor-pointer"
                  title="删除"
                >
                  <Trash2 class="w-3.5 h-3.5" />
                </button>
                <button
                  @click="handleOpenUrl(bm.url)"
                  class="p-1.5 hover:text-blue-600 hover:bg-blue-50 rounded transition cursor-pointer"
                  title="在浏览器中打开"
                >
                  <ExternalLink class="w-3.5 h-3.5" />
                </button>
              </div>
            </div>
          </div>
        </div>
      </section>
    </main>

    <!-- 弹窗：网络代理设置 -->
    <div
      v-if="isProxyModalOpen"
      class="fixed inset-0 bg-slate-900/40 backdrop-blur-xs flex items-center justify-center p-4 z-50"
    >
      <div class="bg-white rounded-2xl shadow-xl w-full max-w-md overflow-hidden animate-in fade-in zoom-in-95 duration-150">
        <div class="px-6 py-4 border-b border-slate-100 flex items-center justify-between">
          <div class="flex items-center gap-2">
            <Wifi class="w-5 h-5 text-blue-600" />
            <h2 class="text-base font-bold text-slate-900">网络代理设置</h2>
          </div>
          <button @click="isProxyModalOpen = false" class="text-slate-400 hover:text-slate-600 text-lg leading-none cursor-pointer">
            &times;
          </button>
        </div>

        <form @submit.prevent="saveProxyConfig" class="p-6 space-y-4">
          <div>
            <label class="block text-xs font-semibold text-slate-600 uppercase mb-2">代理模式</label>
            <div class="grid grid-cols-3 gap-2">
              <label
                :class="[
                  'flex items-center justify-center gap-1.5 p-2 rounded-lg border text-xs font-medium cursor-pointer transition',
                  proxyForm.mode === 'system'
                    ? 'border-blue-600 bg-blue-50 text-blue-700'
                    : 'border-slate-200 text-slate-600 hover:bg-slate-50',
                ]"
              >
                <input type="radio" value="system" v-model="proxyForm.mode" class="hidden" />
                <span>跟随系统代理</span>
              </label>

              <label
                :class="[
                  'flex items-center justify-center gap-1.5 p-2 rounded-lg border text-xs font-medium cursor-pointer transition',
                  proxyForm.mode === 'custom'
                    ? 'border-blue-600 bg-blue-50 text-blue-700'
                    : 'border-slate-200 text-slate-600 hover:bg-slate-50',
                ]"
              >
                <input type="radio" value="custom" v-model="proxyForm.mode" class="hidden" />
                <span>自定义代理</span>
              </label>

              <label
                :class="[
                  'flex items-center justify-center gap-1.5 p-2 rounded-lg border text-xs font-medium cursor-pointer transition',
                  proxyForm.mode === 'direct'
                    ? 'border-blue-600 bg-blue-50 text-blue-700'
                    : 'border-slate-200 text-slate-600 hover:bg-slate-50',
                ]"
              >
                <input type="radio" value="direct" v-model="proxyForm.mode" class="hidden" />
                <span>完全直连</span>
              </label>
            </div>
          </div>

          <!-- 自定义代理地址输入 -->
          <div v-if="proxyForm.mode === 'custom'" class="space-y-2">
            <div class="flex items-center justify-between">
              <label class="block text-xs font-semibold text-slate-600 uppercase">代理地址 (HTTP / SOCKS5)</label>
              <button
                type="button"
                @click="handleTestProxy"
                :disabled="isTestingProxy"
                class="text-xs text-blue-600 hover:text-blue-700 font-medium flex items-center gap-1 cursor-pointer disabled:opacity-50"
              >
                <Loader2 v-if="isTestingProxy" class="w-3 h-3 animate-spin" />
                <span>{{ isTestingProxy ? "测试中..." : "测试连接" }}</span>
              </button>
            </div>
            <input
              v-model="proxyForm.url"
              type="text"
              placeholder="例如 http://127.0.0.1:7890 或 socks5://127.0.0.1:10808"
              class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500 font-mono"
            />
            <p class="text-[11px] text-slate-400">
              常见客户端本地端口：Clash (7890)、v2rayN/Xray (10808 或 10809)
            </p>

            <!-- 测试结果反馈 -->
            <div
              v-if="proxyTestResult"
              :class="[
                'p-2.5 rounded-lg text-xs flex items-start gap-2',
                proxyTestResult.success ? 'bg-emerald-50 text-emerald-800 border border-emerald-200' : 'bg-rose-50 text-rose-800 border border-rose-200',
              ]"
            >
              <CheckCircle2 v-if="proxyTestResult.success" class="w-4 h-4 text-emerald-600 shrink-0 mt-0.5" />
              <XCircle v-else class="w-4 h-4 text-rose-600 shrink-0 mt-0.5" />
              <div>
                <p v-if="proxyTestResult.success" class="font-semibold">
                  连接测试成功！往返延迟: {{ proxyTestResult.latency_ms }} ms
                </p>
                <p v-else class="font-semibold">
                  连接测试失败：{{ proxyTestResult.error }}
                </p>
              </div>
            </div>
          </div>

          <p class="text-xs text-slate-400 bg-slate-50 p-2.5 rounded-lg leading-relaxed">
            💡 提示：开启代理后，抓取网页标题、Favicon 网站图标及批量死链检测将全部通过该代理连接，解决境内访问受阻的问题。
          </p>

          <div class="pt-2 flex justify-end gap-3">
            <button
              type="button"
              @click="isProxyModalOpen = false"
              class="px-4 py-2 text-sm text-slate-600 hover:bg-slate-100 rounded-lg transition cursor-pointer"
            >
              取消
            </button>
            <button
              type="submit"
              class="px-4 py-2 text-sm font-medium bg-blue-600 hover:bg-blue-700 text-white rounded-lg shadow-sm shadow-blue-500/20 transition cursor-pointer"
            >
              保存并应用
            </button>
          </div>
        </form>
      </div>
    </div>

    <!-- 弹窗：添加 / 编辑书签 -->
    <div
      v-if="isAddEditModalOpen"
      class="fixed inset-0 bg-slate-900/40 backdrop-blur-xs flex items-center justify-center p-4 z-50"
    >
      <div class="bg-white rounded-2xl shadow-xl w-full max-w-md overflow-hidden animate-in fade-in zoom-in-95 duration-150">
        <div class="px-6 py-4 border-b border-slate-100 flex items-center justify-between">
          <h2 class="text-base font-bold text-slate-900">
            {{ editingBookmarkId === null ? "添加书签" : "编辑书签" }}
          </h2>
          <button @click="isAddEditModalOpen = false" class="text-slate-400 hover:text-slate-600 text-lg leading-none cursor-pointer">
            &times;
          </button>
        </div>

        <form @submit.prevent="submitBookmark" class="p-6 space-y-4">
          <!-- 网址输入 -->
          <div>
            <div class="flex items-center justify-between mb-1">
              <label class="block text-xs font-semibold text-slate-600 uppercase">网址 URL (必填)</label>
              <button
                type="button"
                @click="forceFetchMeta"
                :disabled="isFetchingMeta"
                class="flex items-center gap-1 text-xs text-blue-600 hover:text-blue-700 font-medium disabled:opacity-50 cursor-pointer"
                title="重新自动提取标题与图标"
              >
                <Loader2 v-if="isFetchingMeta" class="w-3 h-3 animate-spin" />
                <Sparkles v-else class="w-3 h-3" />
                <span>{{ isFetchingMeta ? "抓取中..." : "重新抓取" }}</span>
              </button>
            </div>
            <div class="flex items-center gap-2">
              <div v-if="bookmarkForm.iconDataUrl" class="w-8 h-8 rounded border border-slate-200 p-1 flex items-center justify-center shrink-0">
                <img :src="bookmarkForm.iconDataUrl" class="w-full h-full object-contain" alt="icon" />
              </div>
              <input
                v-model="bookmarkForm.url"
                @blur="handleUrlBlurOrPaste"
                @paste="handlePaste"
                type="text"
                required
                placeholder="例如：github.com 或 https://example.com"
                class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500"
              />
            </div>
            <p v-if="isFetchingMeta" class="text-xs text-blue-500 mt-1 flex items-center gap-1">
              <Loader2 class="w-3 h-3 animate-spin" /> 正在自动从网址解析标题、Favicon与AI分类...
            </p>
            <p v-else-if="fetchErrorTip" class="text-xs text-amber-600 mt-1">
              ⚠️ {{ fetchErrorTip }}
            </p>
          </div>

          <!-- AI 智能建议提示横条 -->
          <div v-if="aiSuggestion" class="bg-indigo-50 border border-indigo-100 rounded-lg p-2.5 text-xs text-indigo-900 flex items-start justify-between gap-2">
            <div>
              <div class="flex items-center gap-1 font-semibold text-indigo-800 mb-0.5">
                <Bot class="w-3.5 h-3.5 text-indigo-600" /> AI 建议归类：
              </div>
              <p>分类: <span class="font-medium text-indigo-950">{{ aiSuggestion.category_name }}</span> | 标签: {{ aiSuggestion.tags.join(", ") }}</p>
            </div>
            <button
              type="button"
              @click="applyAiSuggestion"
              class="px-2 py-1 bg-indigo-600 text-white rounded font-medium hover:bg-indigo-700 transition shrink-0 cursor-pointer"
            >
              采纳
            </button>
          </div>

          <!-- 标题 -->
          <div>
            <label class="block text-xs font-semibold text-slate-600 uppercase mb-1">标题 (选填，自动抓取)</label>
            <input
              v-model="bookmarkForm.title"
              type="text"
              placeholder="留空保存时将自动从网址抓取"
              class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500"
            />
          </div>

          <!-- 分类 -->
          <div>
            <label class="block text-xs font-semibold text-slate-600 uppercase mb-1">
              所属分类 (留空时由 AI 辅助自动归类)
            </label>
            <select
              v-model="bookmarkForm.categoryId"
              class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 bg-white"
            >
              <option :value="null">未指定 (将由 AI 辅助自动完成分类)</option>
              <option v-for="cat in categories" :key="cat.id" :value="cat.id">
                {{ cat.name }}
              </option>
            </select>
          </div>

          <!-- 标签 -->
          <div>
            <label class="block text-xs font-semibold text-slate-600 uppercase mb-1">
              标签 (留空时由 AI 辅助补充，AI分类会自动加上 #ai辅助)
            </label>
            <input
              v-model="bookmarkForm.tagsInput"
              type="text"
              placeholder="编程, 工具, 学习"
              class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500"
            />
          </div>

          <div class="pt-2 flex justify-end gap-3">
            <button
              type="button"
              @click="isAddEditModalOpen = false"
              class="px-4 py-2 text-sm text-slate-600 hover:bg-slate-100 rounded-lg transition cursor-pointer"
            >
              取消
            </button>
            <button
              type="submit"
              class="px-4 py-2 text-sm font-medium bg-blue-600 hover:bg-blue-700 text-white rounded-lg shadow-sm shadow-blue-500/20 transition cursor-pointer"
            >
              保存
            </button>
          </div>
        </form>
      </div>
    </div>

    <!-- 弹窗：创建分类 -->
    <div
      v-if="isCategoryModalOpen"
      class="fixed inset-0 bg-slate-900/40 backdrop-blur-xs flex items-center justify-center p-4 z-50"
    >
      <div class="bg-white rounded-2xl shadow-xl w-full max-w-sm overflow-hidden">
        <div class="px-6 py-4 border-b border-slate-100 flex items-center justify-between">
          <h2 class="text-base font-bold text-slate-900">新建分类</h2>
          <button @click="isCategoryModalOpen = false" class="text-slate-400 hover:text-slate-600 text-lg leading-none cursor-pointer">
            &times;
          </button>
        </div>
        <form @submit.prevent="submitCategory" class="p-6 space-y-4">
          <div>
            <label class="block text-xs font-semibold text-slate-600 uppercase mb-1">分类名称</label>
            <input
              v-model="newCategoryName"
              type="text"
              required
              placeholder="例如：技术开发、影音娱乐"
              class="w-full text-sm px-3 py-2 rounded-lg border border-slate-200 focus:outline-none focus:border-blue-500 focus:ring-1 focus:ring-blue-500"
            />
          </div>
          <div class="flex justify-end gap-3">
            <button
              type="button"
              @click="isCategoryModalOpen = false"
              class="px-4 py-2 text-sm text-slate-600 hover:bg-slate-100 rounded-lg transition cursor-pointer"
            >
              取消
            </button>
            <button
              type="submit"
              class="px-4 py-2 text-sm font-medium bg-blue-600 hover:bg-blue-700 text-white rounded-lg transition cursor-pointer"
            >
              创建
            </button>
          </div>
        </form>
      </div>
    </div>
  </div>
</template>
