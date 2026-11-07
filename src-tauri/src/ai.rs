use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiSuggestion {
    pub category_name: String,
    pub tags: Vec<String>,
}

/// 基于语义模式与特征词的智能分类与标签推荐引擎（无需外接 Key，本地零延迟响应）
pub fn suggest_category_and_tags(
    url: &str,
    title: &str,
    existing_categories: &[String],
) -> AiSuggestion {
    let lower_url = url.to_ascii_lowercase();
    let lower_title = title.to_ascii_lowercase();
    let combined = format!("{} {}", lower_url, lower_title);

    // 规则库匹配
    let (cat, tags) = if combined.contains("github")
        || combined.contains("gitlab")
        || combined.contains("gitee")
        || combined.contains("rust")
        || combined.contains("python")
        || combined.contains("vue")
        || combined.contains("react")
        || combined.contains("docker")
        || combined.contains("kubernetes")
        || combined.contains("code")
        || combined.contains("developer")
        || combined.contains("api")
        || combined.contains("npm")
        || combined.contains("crates.io")
        || combined.contains("stack overflow")
    {
        (
            "编程开发",
            vec!["代码", "编程", "开源", "开发工具"],
        )
    } else if combined.contains("chatgpt")
        || combined.contains("claude")
        || combined.contains("deepseek")
        || combined.contains("openai")
        || combined.contains("gemini")
        || combined.contains("anthropic")
        || combined.contains("llm")
        || combined.contains("huggingface")
        || combined.contains("ai")
        || combined.contains("artificial intelligence")
        || combined.contains("大模型")
        || combined.contains("人工智能")
    {
        (
            "人工智能",
            vec!["AI", "大模型", "生成式AI"],
        )
    } else if combined.contains("bilibili")
        || combined.contains("youtube")
        || combined.contains("netflix")
        || combined.contains("douban")
        || combined.contains("video")
        || combined.contains("movie")
        || combined.contains("music")
        || combined.contains("spotify")
        || combined.contains("acfun")
        || combined.contains("影视")
        || combined.contains("视频")
        || combined.contains("动漫")
    {
        (
            "影音娱乐",
            vec!["视频", "影视", "娱乐"],
        )
    } else if combined.contains("zhihu")
        || combined.contains("v2ex")
        || combined.contains("juejin")
        || combined.contains("weibo")
        || combined.contains("reddit")
        || combined.contains("twitter")
        || combined.contains("x.com")
        || combined.contains("news")
        || combined.contains("sspai")
        || combined.contains("少数派")
        || combined.contains("资讯")
        || combined.contains("社区")
        || combined.contains("博客")
    {
        (
            "资讯社区",
            vec!["资讯", "社区", "阅读"],
        )
    } else if combined.contains("figma")
        || combined.contains("dribbble")
        || combined.contains("behance")
        || combined.contains("design")
        || combined.contains("canvas")
        || combined.contains("ui")
        || combined.contains("设计")
        || combined.contains("图标")
    {
        (
            "设计创意",
            vec!["设计", "UI", "灵感"],
        )
    } else if combined.contains("bank")
        || combined.contains("finance")
        || combined.contains("stock")
        || combined.contains("crypto")
        || combined.contains("bitcoin")
        || combined.contains("理财")
        || combined.contains("股票")
        || combined.contains("基金")
        || combined.contains("财经")
    {
        (
            "金融理财",
            vec!["理财", "金融", "投资"],
        )
    } else {
        (
            "常用工具",
            vec!["工具", "效率"],
        )
    };

    // 优先匹配用户已有分类列表（若已有分类中有同名或相似的，归入用户已有分类）
    let matched_cat = existing_categories
        .iter()
        .find(|c| c.eq_ignore_ascii_case(cat) || c.contains(cat) || cat.contains(c.as_str()))
        .cloned()
        .unwrap_or_else(|| cat.to_string());

    // 核心硬性约束：AI 辅助完成的分类必须添加 "ai辅助" 标签！
    let mut final_tags = vec!["ai辅助".to_string()];
    for t in tags {
        final_tags.push(t.to_string());
    }

    AiSuggestion {
        category_name: matched_cat,
        tags: final_tags,
    }
}
