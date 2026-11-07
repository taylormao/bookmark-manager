<script setup lang="ts">
import { ref, computed, nextTick } from "vue";
import {
  Folder,
  FolderOpen,
  ChevronRight,
  ChevronDown,
  Edit3,
  Trash2,
  Plus,
  GripVertical,
} from "@lucide/vue";

export interface CategoryNode {
  id: number;
  name: string;
  parent_id: number | null;
  sort_order: number;
  children: CategoryNode[];
}

const props = defineProps<{
  node: CategoryNode;
  activeCategoryId: number | null;
  expandedIds: Set<number>;
  level?: number;
}>();

const emit = defineEmits<{
  (e: "select", id: number): void;
  (e: "toggleExpand", id: number): void;
  (e: "rename", id: number, newName: string): void;
  (e: "delete", id: number): void;
  (e: "addChild", parentId: number): void;
  (
    e: "dropCategory",
    payload: {
      draggedId: number;
      targetId: number;
      position: "before" | "after" | "inside";
    }
  ): void;
  (e: "dropBookmark", payload: { bookmarkId: number; targetCategoryId: number }): void;
}>();

const currentLevel = computed(() => props.level ?? 0);
const hasChildren = computed(() => props.node.children.length > 0);
const isExpanded = computed(() => props.expandedIds.has(props.node.id));
const isSelected = computed(() => props.activeCategoryId === props.node.id);

// 行内重命名状态
const isEditing = ref(false);
const editName = ref("");
const inputRef = ref<HTMLInputElement | null>(null);

function startRename() {
  editName.value = props.node.name;
  isEditing.value = true;
  nextTick(() => {
    inputRef.value?.focus();
    inputRef.value?.select();
  });
}

function finishRename() {
  if (!isEditing.value) return;
  const trimmed = editName.value.trim();
  isEditing.value = false;
  if (trimmed && trimmed !== props.node.name) {
    emit("rename", props.node.id, trimmed);
  }
}

function cancelRename() {
  isEditing.value = false;
}

// 拖拽状态
const dragOverPosition = ref<"before" | "after" | "inside" | null>(null);
const isBookmarkOver = ref(false);

function handleDragStart(e: DragEvent) {
  if (!e.dataTransfer) return;
  e.dataTransfer.setData("text/category-id", String(props.node.id));
  e.dataTransfer.effectAllowed = "move";
  // 设置拖动透明度或类
  e.dataTransfer.setData("text/plain", props.node.name);
}

function handleDragOver(e: DragEvent) {
  e.preventDefault();
  if (!e.dataTransfer) return;

  const isBookmark = e.dataTransfer.types.includes("text/bookmark-id");
  const isCategory = e.dataTransfer.types.includes("text/category-id");

  if (isBookmark) {
    isBookmarkOver.value = true;
    dragOverPosition.value = "inside";
    e.dataTransfer.dropEffect = "move";
    return;
  }

  if (isCategory) {
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const offsetY = e.clientY - rect.top;
    const ratio = offsetY / rect.height;

    if (ratio < 0.28) {
      dragOverPosition.value = "before";
    } else if (ratio > 0.72) {
      dragOverPosition.value = "after";
    } else {
      dragOverPosition.value = "inside";
    }
    e.dataTransfer.dropEffect = "move";
  }
}

function handleDragLeave() {
  dragOverPosition.value = null;
  isBookmarkOver.value = false;
}

function handleDrop(e: DragEvent) {
  e.preventDefault();
  e.stopPropagation();
  if (!e.dataTransfer) return;

  const catIdStr = e.dataTransfer.getData("text/category-id");
  const bmIdStr = e.dataTransfer.getData("text/bookmark-id");
  const position = dragOverPosition.value;
  dragOverPosition.value = null;
  isBookmarkOver.value = false;

  if (bmIdStr) {
    const bookmarkId = parseInt(bmIdStr, 10);
    if (!isNaN(bookmarkId)) {
      emit("dropBookmark", { bookmarkId, targetCategoryId: props.node.id });
    }
    return;
  }

  if (catIdStr && position) {
    const draggedId = parseInt(catIdStr, 10);
    if (!isNaN(draggedId) && draggedId !== props.node.id) {
      emit("dropCategory", {
        draggedId,
        targetId: props.node.id,
        position,
      });
    }
  }
}
</script>

<template>
  <div class="category-tree-node select-none">
    <!-- 当前分类行 -->
    <div
      class="group relative flex items-center justify-between py-1.5 px-2 rounded-lg text-sm cursor-pointer transition-all border border-transparent"
      :class="[
        isSelected
          ? 'bg-blue-50 text-blue-600 font-semibold'
          : 'text-slate-600 hover:bg-slate-100 hover:text-slate-900',
        dragOverPosition === 'inside' && !isBookmarkOver
          ? 'bg-blue-100/70 border-dashed !border-blue-400'
          : '',
        isBookmarkOver
          ? 'bg-emerald-50 !border-2 !border-emerald-500 shadow-xs text-emerald-800 font-medium'
          : '',
      ]"
      :style="{ paddingLeft: `${currentLevel * 14 + 8}px` }"
      draggable="true"
      @dragstart="handleDragStart"
      @dragover="handleDragOver"
      @dragleave="handleDragLeave"
      @drop="handleDrop"
      @click="emit('select', node.id)"
    >
      <!-- 顶部放置指示线（调整到前面） -->
      <div
        v-if="dragOverPosition === 'before'"
        class="absolute -top-0.5 left-2 right-2 h-0.5 bg-blue-600 rounded-full z-10 pointer-events-none shadow"
      ></div>

      <!-- 左侧展开箭头、拖动柄与图标 -->
      <div class="flex items-center gap-1.5 min-w-0 flex-1">
        <!-- 拖动手柄 -->
        <span
          class="opacity-0 group-hover:opacity-40 hover:!opacity-100 cursor-grab text-slate-400 active:cursor-grabbing p-0.5"
          title="按住拖动以调整顺序或层级"
          @click.stop
        >
          <GripVertical class="w-3.5 h-3.5" />
        </span>

        <!-- 展开/折叠三角 -->
        <button
          v-if="hasChildren"
          @click.stop="emit('toggleExpand', node.id)"
          class="w-4 h-4 flex items-center justify-center text-slate-400 hover:text-slate-700 transition"
        >
          <ChevronDown v-if="isExpanded" class="w-3.5 h-3.5" />
          <ChevronRight v-else class="w-3.5 h-3.5" />
        </button>
        <span v-else class="w-4 h-4 inline-block"></span>

        <!-- 分类文件夹图标 -->
        <FolderOpen
          v-if="isExpanded"
          class="w-4 h-4 shrink-0"
          :class="isSelected ? 'text-blue-600' : 'text-blue-400'"
        />
        <Folder
          v-else
          class="w-4 h-4 shrink-0"
          :class="isSelected ? 'text-blue-600' : 'text-slate-400 group-hover:text-slate-500'"
        />

        <!-- 分类名称或行内编辑输入框 -->
        <template v-if="isEditing">
          <input
            ref="inputRef"
            v-model="editName"
            type="text"
            class="flex-1 min-w-0 px-1 py-0.5 text-xs bg-white border border-blue-500 rounded outline-none shadow-xs text-slate-800"
            @blur="finishRename"
            @keydown.enter.prevent="finishRename"
            @keydown.esc.prevent="cancelRename"
            @click.stop
          />
        </template>
        <template v-else>
          <span
            class="truncate text-xs font-medium"
            :title="`${node.name} (双击重命名)`"
            @dblclick.stop="startRename"
          >
            {{ node.name }}
          </span>
          <span v-if="hasChildren" class="text-[10px] text-slate-400 font-normal ml-0.5">
            ({{ node.children.length }})
          </span>
          <!-- 书签拖拽悬浮提示徽章 -->
          <span
            v-if="isBookmarkOver"
            class="ml-auto text-[10px] bg-emerald-600 text-white px-1.5 py-0.5 rounded font-medium shrink-0 shadow-xs"
          >
            放开归类
          </span>
        </template>
      </div>

      <!-- 右侧操作工具栏 (悬浮展示) -->
      <div class="hidden group-hover:flex items-center gap-0.5 shrink-0" @click.stop>
        <!-- 添加子分类 -->
        <button
          @click.stop="emit('addChild', node.id)"
          class="p-1 text-slate-400 hover:text-blue-600 hover:bg-white rounded transition"
          title="在此分类下添加子分类"
        >
          <Plus class="w-3.5 h-3.5" />
        </button>

        <!-- 重命名 -->
        <button
          @click.stop="startRename"
          class="p-1 text-slate-400 hover:text-slate-700 hover:bg-white rounded transition"
          title="重命名"
        >
          <Edit3 class="w-3.5 h-3.5" />
        </button>

        <!-- 删除 -->
        <button
          @click.stop="emit('delete', node.id)"
          class="p-1 text-slate-400 hover:text-rose-600 hover:bg-white rounded transition"
          title="删除分类"
        >
          <Trash2 class="w-3.5 h-3.5" />
        </button>
      </div>

      <!-- 底部放置指示线（调整到后面） -->
      <div
        v-if="dragOverPosition === 'after'"
        class="absolute -bottom-0.5 left-2 right-2 h-0.5 bg-blue-600 rounded-full z-10 pointer-events-none shadow"
      ></div>
    </div>

    <!-- 子分类列表（递归渲染） -->
    <div v-if="hasChildren && isExpanded" class="subcategories">
      <CategoryTreeItem
        v-for="child in node.children"
        :key="child.id"
        :node="child"
        :active-category-id="activeCategoryId"
        :expanded-ids="expandedIds"
        :level="currentLevel + 1"
        @select="(id) => emit('select', id)"
        @toggle-expand="(id) => emit('toggleExpand', id)"
        @rename="(id, name) => emit('rename', id, name)"
        @delete="(id) => emit('delete', id)"
        @add-child="(id) => emit('addChild', id)"
        @drop-category="(payload) => emit('dropCategory', payload)"
        @drop-bookmark="(payload) => emit('dropBookmark', payload)"
      />
    </div>
  </div>
</template>

