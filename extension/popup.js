/**
 * Chrome 浏览器扩展 Popup 脚本
 * 支持获取当前标签页信息、读取与写入 Cloudflare Worker 云端收藏、云端书签检索。
 */

// 当前活动标签页信息
let currentTab = {
  url: "",
  title: "",
  favIconUrl: "",
};

let workerConfig = {
  workerUrl: "",
  apiToken: "",
};

let cachedBookmarks = [];

// DOM 元素
const tabBtnSave = document.getElementById("tab-btn-save");
const tabBtnSearch = document.getElementById("tab-btn-search");
const btnSettings = document.getElementById("btn-settings");
const btnGoSettings = document.getElementById("btn-go-settings");
const btnBackFromSettings = document.getElementById("btn-back-from-settings");

const viewSave = document.getElementById("view-save");
const viewSearch = document.getElementById("view-search");
const viewSettings = document.getElementById("view-settings");
const unconfiguredTip = document.getElementById("unconfigured-tip");

const tabFavicon = document.getElementById("tab-favicon");
const tabTitleText = document.getElementById("tab-title-text");
const tabUrlText = document.getElementById("tab-url-text");
const inputTitle = document.getElementById("input-title");
const selectCategory = document.getElementById("select-category");
const inputTags = document.getElementById("input-tags");
const saveForm = document.getElementById("save-form");
const btnSubmitSave = document.getElementById("btn-submit-save");
const saveMsg = document.getElementById("save-msg");

const searchInput = document.getElementById("search-input");
const searchResults = document.getElementById("search-results");

const settingsForm = document.getElementById("settings-form");
const settingsWorkerUrl = document.getElementById("settings-worker-url");
const settingsToken = document.getElementById("settings-token");
const settingsMsg = document.getElementById("settings-msg");
const btnSaveSettings = document.getElementById("btn-save-settings");

const statusDot = document.getElementById("status-dot");
const statusText = document.getElementById("status-text");

// 视图切换
function switchView(viewName) {
  [viewSave, viewSearch, viewSettings, unconfiguredTip].forEach((v) => {
    v.classList.remove("active");
    v.style.display = "none";
  });

  tabBtnSave.classList.remove("active");
  tabBtnSearch.classList.remove("active");

  if (viewName === "save") {
    if (!workerConfig.workerUrl) {
      unconfiguredTip.style.display = "block";
      unconfiguredTip.classList.add("active");
    } else {
      viewSave.style.display = "block";
      viewSave.classList.add("active");
      tabBtnSave.classList.add("active");
    }
  } else if (viewName === "search") {
    if (!workerConfig.workerUrl) {
      unconfiguredTip.style.display = "block";
      unconfiguredTip.classList.add("active");
    } else {
      viewSearch.style.display = "block";
      viewSearch.classList.add("active");
      tabBtnSearch.classList.add("active");
      loadCloudBookmarks();
    }
  } else if (viewName === "settings") {
    viewSettings.style.display = "block";
    viewSettings.classList.add("active");
  }
}

// 事件绑定
tabBtnSave.addEventListener("click", () => switchView("save"));
tabBtnSearch.addEventListener("click", () => switchView("search"));
btnSettings.addEventListener("click", () => switchView("settings"));
btnGoSettings.addEventListener("click", () => switchView("settings"));
btnBackFromSettings.addEventListener("click", () => switchView("save"));

// 加载当前活动标签页
function loadCurrentTab() {
  if (typeof chrome !== "undefined" && chrome.tabs && chrome.tabs.query) {
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      if (tabs && tabs[0]) {
        const tab = tabs[0];
        currentTab.url = tab.url || "";
        currentTab.title = tab.title || "";
        currentTab.favIconUrl = tab.favIconUrl || "";

        tabTitleText.textContent = currentTab.title || "无标题";
        tabUrlText.textContent = currentTab.url;
        inputTitle.value = currentTab.title || currentTab.url;

        if (currentTab.favIconUrl) {
          tabFavicon.src = currentTab.favIconUrl;
        } else {
          try {
            const domain = new URL(currentTab.url).hostname;
            tabFavicon.src = `https://www.google.com/s2/favicons?domain=${encodeURIComponent(domain)}`;
          } catch (e) {
            tabFavicon.src = "icons/icon16.png";
          }
        }
      }
    });
  }
}

// 加载配置
async function loadConfig() {
  const storage = typeof chrome !== "undefined" && chrome.storage ? chrome.storage.sync : null;
  if (!storage) return;

  storage.get(["workerUrl", "apiToken"], async (data) => {
    workerConfig.workerUrl = (data.workerUrl || "").trim().replace(/\/+$/, "");
    workerConfig.apiToken = (data.apiToken || "").trim();

    settingsWorkerUrl.value = workerConfig.workerUrl;
    settingsToken.value = workerConfig.apiToken;

    if (!workerConfig.workerUrl) {
      switchView("settings");
      statusDot.className = "dot";
      statusText.textContent = "未配置";
    } else {
      statusDot.className = "dot green";
      statusText.textContent = "已连接云端";
      switchView("save");
      await loadCloudCategories();
    }
  });
}

// 获取分类列表
async function loadCloudCategories() {
  if (!workerConfig.workerUrl) return;
  try {
    const headers = {};
    if (workerConfig.apiToken) {
      headers["Authorization"] = `Bearer ${workerConfig.apiToken}`;
    }

    const res = await fetch(`${workerConfig.workerUrl}/api/categories`, { headers });
    if (res.ok) {
      const categories = await res.json();
      selectCategory.innerHTML = '<option value="">-- 未分类 (默认) --</option>';
      categories.forEach((c) => {
        const opt = document.createElement("option");
        opt.value = c.id;
        opt.textContent = c.name;
        selectCategory.appendChild(opt);
      });
    }
  } catch (err) {
    console.error("加载云端分类失败:", err);
  }
}

// 保存书签
saveForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  if (!workerConfig.workerUrl) {
    alert("请先配置 Cloudflare Worker 地址！");
    switchView("settings");
    return;
  }

  btnSubmitSave.disabled = true;
  btnSubmitSave.innerHTML = "<span>⏳</span><span>正在同步至云端...</span>";
  saveMsg.style.display = "none";

  const tags = inputTags.value
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);

  const payload = {
    url: currentTab.url,
    title: inputTitle.value.trim() || currentTab.title,
    icon: currentTab.favIconUrl || null,
    category_id: selectCategory.value ? parseInt(selectCategory.value, 10) : null,
    tags,
  };

  try {
    const headers = { "Content-Type": "application/json" };
    if (workerConfig.apiToken) {
      headers["Authorization"] = `Bearer ${workerConfig.apiToken}`;
    }

    const res = await fetch(`${workerConfig.workerUrl}/api/bookmarks`, {
      method: "POST",
      headers,
      body: JSON.stringify(payload),
    });

    if (res.ok) {
      saveMsg.textContent = "✅ 已成功收藏至 Cloudflare 云端！";
      saveMsg.style.display = "flex";
      btnSubmitSave.innerHTML = "<span>✓</span><span>保存成功</span>";

      setTimeout(() => {
        window.close();
      }, 1400);
    } else {
      const data = await res.json().catch(() => ({}));
      alert(`保存失败: ${data.error || res.statusText}`);
      btnSubmitSave.disabled = false;
      btnSubmitSave.innerHTML = "<span>☁️</span><span>保存到云端收藏夹</span>";
    }
  } catch (err) {
    alert(`网络请求异常: ${err.message}`);
    btnSubmitSave.disabled = false;
    btnSubmitSave.innerHTML = "<span>☁️</span><span>保存到云端收藏夹</span>";
  }
});

// 检索书签
async function loadCloudBookmarks() {
  if (!workerConfig.workerUrl) return;

  searchResults.innerHTML = '<div style="text-align: center; color: #94a3b8; padding: 20px 0;">加载中...</div>';

  try {
    const headers = {};
    if (workerConfig.apiToken) {
      headers["Authorization"] = `Bearer ${workerConfig.apiToken}`;
    }

    const res = await fetch(`${workerConfig.workerUrl}/api/bookmarks`, { headers });
    if (res.ok) {
      cachedBookmarks = await res.json();
      renderSearchResults(cachedBookmarks);
    } else {
      searchResults.innerHTML = '<div style="text-align: center; color: #ef4444; padding: 20px 0;">拉取失败，请检查配置与网络</div>';
    }
  } catch (err) {
    searchResults.innerHTML = `<div style="text-align: center; color: #ef4444; padding: 20px 0;">网络异常: ${err.message}</div>`;
  }
}

function renderSearchResults(list) {
  if (!list || list.length === 0) {
    searchResults.innerHTML = '<div style="text-align: center; color: #94a3b8; padding: 20px 0;">未找到匹配的书签</div>';
    return;
  }

  searchResults.innerHTML = "";
  list.forEach((b) => {
    const item = document.createElement("a");
    item.className = "bookmark-item";
    item.href = b.url;
    item.title = `${b.title}\n${b.url}`;

    const iconUrl = b.icon || `https://www.google.com/s2/favicons?domain=${encodeURIComponent(b.url)}`;

    item.innerHTML = `
      <div class="item-left">
        <img src="${iconUrl}" class="item-icon" onerror="this.src='icons/icon16.png'">
        <span class="item-title">${b.title || b.url}</span>
      </div>
      <span class="item-cat">${b.category_name || "未分类"}</span>
    `;

    item.addEventListener("click", (e) => {
      e.preventDefault();
      if (typeof chrome !== "undefined" && chrome.tabs && chrome.tabs.create) {
        chrome.tabs.create({ url: b.url });
      } else {
        window.open(b.url, "_blank");
      }
    });

    searchResults.appendChild(item);
  });
}

// 实时搜索输入过滤
searchInput.addEventListener("input", () => {
  const query = searchInput.value.trim().toLowerCase();
  if (!query) {
    renderSearchResults(cachedBookmarks);
    return;
  }
  const filtered = cachedBookmarks.filter((b) => {
    return (
      (b.title && b.title.toLowerCase().includes(query)) ||
      (b.url && b.url.toLowerCase().includes(query)) ||
      (b.category_name && b.category_name.toLowerCase().includes(query)) ||
      (Array.isArray(b.tags) && b.tags.some((t) => t.toLowerCase().includes(query)))
    );
  });
  renderSearchResults(filtered);
});

// 保存配置与连接测试
settingsForm.addEventListener("submit", async (e) => {
  e.preventDefault();

  const inputUrl = settingsWorkerUrl.value.trim().replace(/\/+$/, "");
  const inputTok = settingsToken.value.trim();

  settingsMsg.style.display = "flex";
  settingsMsg.className = "tip-box";
  settingsMsg.textContent = "正在测试连接 Cloudflare Worker...";
  btnSaveSettings.disabled = true;

  try {
    const headers = {};
    if (inputTok) {
      headers["Authorization"] = `Bearer ${inputTok}`;
    }

    const res = await fetch(`${inputUrl}/api/status`, { headers });
    if (res.ok) {
      const data = await res.json();
      const storage = typeof chrome !== "undefined" && chrome.storage ? chrome.storage.sync : null;
      if (storage) {
        storage.set({ workerUrl: inputUrl, apiToken: inputTok }, () => {
          workerConfig.workerUrl = inputUrl;
          workerConfig.apiToken = inputTok;

          settingsMsg.className = "tip-box success";
          settingsMsg.textContent = `✅ 连接成功！云端共有 ${data.bookmarks_count || 0} 条书签。配置已保存！`;

          statusDot.className = "dot green";
          statusText.textContent = "已连接云端";

          setTimeout(() => {
            switchView("save");
            loadCloudCategories();
          }, 1200);
        });
      }
    } else {
      settingsMsg.className = "tip-box warning";
      settingsMsg.textContent = `❌ 连接失败 (HTTP ${res.status})，请检查 Token 或 Worker 状态。`;
    }
  } catch (err) {
    settingsMsg.className = "tip-box warning";
    settingsMsg.textContent = `❌ 请求错误: ${err.message}，请核对 Worker 地址是否正确。`;
  } finally {
    btnSaveSettings.disabled = false;
  }
});

// 初始化
loadCurrentTab();
loadConfig();
