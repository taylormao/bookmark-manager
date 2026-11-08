import { reactive } from "vue";

export interface DragState {
  isDragging: boolean;
  type: "bookmark" | "category" | null;
  bookmarkId: number | null;
  bookmarkTitle: string;
  categoryId: number | null;
  categoryName: string;
}

export const dragManager = reactive<DragState>({
  isDragging: false,
  type: null,
  bookmarkId: null,
  bookmarkTitle: "",
  categoryId: null,
  categoryName: "",
});

export function startDragBookmark(id: number, title: string) {
  dragManager.isDragging = true;
  dragManager.type = "bookmark";
  dragManager.bookmarkId = id;
  dragManager.bookmarkTitle = title;
  dragManager.categoryId = null;
  dragManager.categoryName = "";
  if (typeof window !== "undefined") {
    (window as any).__dragBookmarkId = id;
    (window as any).__dragBookmarkTitle = title;
    delete (window as any).__dragCategoryId;
  }
}

export function startDragCategory(id: number, name: string) {
  dragManager.isDragging = true;
  dragManager.type = "category";
  dragManager.categoryId = id;
  dragManager.categoryName = name;
  dragManager.bookmarkId = null;
  dragManager.bookmarkTitle = "";
  if (typeof window !== "undefined") {
    (window as any).__dragCategoryId = id;
    (window as any).__dragCategoryName = name;
    delete (window as any).__dragBookmarkId;
  }
}

export function endDrag() {
  dragManager.isDragging = false;
  dragManager.type = null;
  dragManager.bookmarkId = null;
  dragManager.bookmarkTitle = "";
  dragManager.categoryId = null;
  dragManager.categoryName = "";
  if (typeof window !== "undefined") {
    delete (window as any).__dragBookmarkId;
    delete (window as any).__dragBookmarkTitle;
    delete (window as any).__dragCategoryId;
    delete (window as any).__dragCategoryName;
  }
}

