/*
 * bmk_i18n.h - 界面字符串集中管理（需求 5.6）
 *
 * 界面文本一律通过 T(key) 获取，不得在控件代码里硬编码字面量。
 * 目前内置中/英两套，默认中文，由 settings.language 决定。
 */
#ifndef BMK_I18N_H
#define BMK_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

/* lang: "zh" 或 "en"，其它值按 "zh" 处理。 */
void        bmk_i18n_init(const char *lang);
const char *bmk_i18n_lang(void);

/* 取字符串；未登记的 key 原样返回，便于快速定位遗漏。 */
const char *bmk_i18n_get(const char *key);
#define T(key) bmk_i18n_get(key)

#ifdef __cplusplus
}
#endif
#endif /* BMK_I18N_H */
