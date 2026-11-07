use regex::Regex;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct ImportedBookmark {
    pub title: String,
    pub url: String,
    pub folder_path: Vec<String>, // 分类层级，例如 ["书签栏", "技术"]
    pub tags: Vec<String>,
    pub add_date: Option<i64>,
    pub icon: Option<String>,
}

/// 解析 Netscape HTML 书签文件
pub fn parse_netscape_html(content: &str) -> Vec<ImportedBookmark> {
    let mut bookmarks = Vec::new();
    let mut folder_stack: Vec<String> = Vec::new();
    let mut pending_folder: Option<String> = None;

    // 正则表达式匹配关键标签
    // 匹配 <H3 ...>Folder Name</H3>
    let h3_regex = Regex::new(r"(?i)<H3[^>]*>(.*?)</H3>").unwrap();
    // 匹配 <A HREF="..." ...>Title</A>
    let a_regex = Regex::new(r#"(?i)<A\s+([^>]*?)>(.*?)</A>"#).unwrap();
    let href_regex = Regex::new(r#"(?i)HREF\s*=\s*["']([^"']+)["']"#).unwrap();
    let tags_regex = Regex::new(r#"(?i)TAGS\s*=\s*["']([^"']+)["']"#).unwrap();
    let add_date_regex = Regex::new(r#"(?i)ADD_DATE\s*=\s*["'](\d+)["']"#).unwrap();
    let icon_regex = Regex::new(r#"(?i)ICON\s*=\s*["']([^"']+)["']"#).unwrap();

    // 按行或按标签流式推进
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        // 检查是否有 <H3
        if let Some(caps) = h3_regex.captures(line) {
            let folder_name = html_unescape(caps.get(1).map_or("", |m| m.as_str()).trim());
            if !folder_name.is_empty() {
                pending_folder = Some(folder_name);
            }
        }

        // 检查是否有 <DL (开始一个新文件夹)
        if line.to_ascii_uppercase().contains("<DL") {
            if let Some(folder) = pending_folder.take() {
                folder_stack.push(folder);
            } else {
                // 根目录或者匿名文件夹
                // 如果是第一个根 DL，则不增加深度
            }
        }

        // 检查是否有 </DL> (离开当前文件夹)
        if line.to_ascii_uppercase().contains("</DL>") {
            folder_stack.pop();
        }

        // 检查是否有 <A
        if let Some(caps) = a_regex.captures(line) {
            let attrs = caps.get(1).map_or("", |m| m.as_str());
            let title = html_unescape(caps.get(2).map_or("", |m| m.as_str()).trim());

            if let Some(href_cap) = href_regex.captures(attrs) {
                let url = html_unescape(href_cap.get(1).map_or("", |m| m.as_str()).trim());
                if !url.is_empty() {
                    let mut tags = Vec::new();
                    if let Some(tags_cap) = tags_regex.captures(attrs) {
                        for t in tags_cap.get(1).unwrap().as_str().split(',') {
                            let t = t.trim();
                            if !t.is_empty() {
                                tags.push(t.to_string());
                            }
                        }
                    }

                    let add_date = add_date_regex
                        .captures(attrs)
                        .and_then(|c| c.get(1).unwrap().as_str().parse::<i64>().ok());

                    let icon = icon_regex
                        .captures(attrs)
                        .map(|c| c.get(1).unwrap().as_str().to_string());

                    bookmarks.push(ImportedBookmark {
                        title: if title.is_empty() { url.clone() } else { title },
                        url,
                        folder_path: folder_stack.clone(),
                        tags,
                        add_date,
                        icon,
                    });
                }
            }
        }
    }

    bookmarks
}

/// 导出为 Netscape Bookmark File 格式
pub fn export_netscape_html(
    bookmarks: &[crate::models::Bookmark],
    categories: &[crate::models::Category],
) -> String {
    let mut out = String::new();
    out.push_str("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n");
    out.push_str("<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n");
    out.push_str("<TITLE>Bookmarks</TITLE>\n");
    out.push_str("<H1>Bookmarks</H1>\n");
    out.push_str("<DL><p>\n");

    // 分类分组
    let mut grouped: HashMap<Option<i64>, Vec<&crate::models::Bookmark>> = HashMap::new();
    for bm in bookmarks {
        grouped.entry(bm.category_id).or_default().push(bm);
    }

    // 首先输出各分类
    for cat in categories {
        if cat.parent_id.is_none() {
            render_category_html(&mut out, cat, categories, &grouped, 1);
        }
    }

    // 最后输出未分类书签
    if let Some(uncategorized) = grouped.get(&None) {
        for bm in uncategorized {
            render_bookmark_link(&mut out, bm, 1);
        }
    }

    out.push_str("</DL><p>\n");
    out
}

fn render_category_html(
    out: &mut String,
    cat: &crate::models::Category,
    all_cats: &[crate::models::Category],
    grouped: &HashMap<Option<i64>, Vec<&crate::models::Bookmark>>,
    depth: usize,
) {
    let indent = "    ".repeat(depth);
    out.push_str(&format!(
        "{}<DT><H3>{}</H3>\n{}<DL><p>\n",
        indent,
        html_escape(&cat.name),
        indent
    ));

    // 输出属于该分类的书签
    if let Some(bms) = grouped.get(&Some(cat.id)) {
        for bm in bms {
            render_bookmark_link(out, bm, depth + 1);
        }
    }

    // 递归输出子分类
    for sub in all_cats {
        if sub.parent_id == Some(cat.id) {
            render_category_html(out, sub, all_cats, grouped, depth + 1);
        }
    }

    out.push_str(&format!("{}</DL><p>\n", indent));
}

fn render_bookmark_link(out: &mut String, bm: &crate::models::Bookmark, depth: usize) {
    let indent = "    ".repeat(depth);
    let mut attrs = format!("HREF=\"{}\" ADD_DATE=\"{}\"", html_escape(&bm.url), bm.created_at);

    if !bm.tags.is_empty() {
        attrs.push_str(&format!(" TAGS=\"{}\"", html_escape(&bm.tags.join(","))));
    }

    out.push_str(&format!(
        "{}<DT><A {}>{}</A>\n",
        indent,
        attrs,
        html_escape(&bm.title)
    ));
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

fn html_unescape(s: &str) -> String {
    s.replace("&amp;", "&")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_appendix_a_sample() {
        let sample = r#"<!DOCTYPE NETSCAPE-Bookmark-file-1>
<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">
<TITLE>Bookmarks</TITLE>
<H1>Bookmarks</H1>
<DL><p>
    <DT><H3 ADD_DATE="1700000000" LAST_MODIFIED="1700000100" PERSONAL_TOOLBAR_FOLDER="true">书签栏</H3>
    <DL><p>
        <DT><A HREF="https://example.com/" ADD_DATE="1700000001" ICON="data:image/png;base64,iVBORw0KGgo=">Example</A>
        <DT><A HREF="https://github.com/" ADD_DATE="1700000002" TAGS="dev,code">GitHub</A>
    </DL><p>
    <DT><H3 ADD_DATE="1700001000">技术</H3>
    <DL><p>
        <DT><A HREF="https://news.ycombinator.com/" ADD_DATE="1700001001">Hacker News</A>
    </DL><p>
</DL><p>"#;

        let res = parse_netscape_html(sample);
        assert_eq!(res.len(), 3);

        assert_eq!(res[0].title, "Example");
        assert_eq!(res[0].url, "https://example.com/");
        assert_eq!(res[0].folder_path, vec!["书签栏"]);

        assert_eq!(res[1].title, "GitHub");
        assert_eq!(res[1].tags, vec!["dev", "code"]);
        assert_eq!(res[1].folder_path, vec!["书签栏"]);

        assert_eq!(res[2].title, "Hacker News");
        assert_eq!(res[2].folder_path, vec!["技术"]);
    }
}
