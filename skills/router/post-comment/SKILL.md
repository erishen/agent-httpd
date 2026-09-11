
# Post Comment 技能（WordPress）

适用场景：用户要求在 WordPress 站点随机找篇文章加评论，或测试博客评论功能。

## 步骤

1. **随机选文章**：用 `shell` 运行 `node ${SKILL_DIR}/scripts/random-comment.mjs --pick`，
   一次调用即可，得到 `{ id, title, link }`（脚本直接从配置的 WordPress 站点拉取随机文章，
   不依赖外部 pick-post 工具）。不要重复调用。

2. **看一眼文章**（可选）：用 `browser-open` 打开上一步的 `link`，了解文章主题，好写出有针对性的评论。

3. **提交评论**：用 `shell` 工具运行

   ```
   node ${SKILL_DIR}/scripts/random-comment.mjs --post <文章id> --content "<一句话评论>"
   ```

   评论内容：基于文章主题写一句**真诚、具体**的中文评论（别用「好文章」「学习了」这类空话）。评论者默认虚构身份 `程序猿小林 / noreply@example.com`（像真实访客的昵称+邮箱，与用户本人无关），如需自定义用 `--name` / `--email` 传入。

4. **汇报结果**：把输出 JSON 转述给用户：文章标题 + 链接 + 评论 id + 状态（`approved` 已通过 / `pending` 待审核）。

## 注意事项

- 这是对**用户自己站点**的写操作，会经过 `shell` 工具的审批（模型请求时用户需批准）；也依赖沙箱放行外网（`HARNESS_ALLOW_NETWORK=1`），否则请求会失败。
- 若站点开启了「必须登录后才能评论」——**匿名评论会 401**（`rest_comment_login_required`）。需要 `PROD_WORDPRESS_USERNAME` / `PROD_WORDPRESS_APP_PASSWORD`（WordPress 应用密码）。三选一放凭据（脚本按序加载，均**不覆盖**已存在的环境变量）：
  1. 本机稳定位置 `~/.config/resolve-skills/.env`（**推荐**，任意 checkout——含 resolve-tui 的 submodule——都能取到；直接把仓库根 `.env` 软链过去最省心）
  2. 环境变量 `HARNESS_SKILLS_ENV` 指向一个 `.env` 绝对路径
  3. 本仓库根 `.env`（复制 `${SKILL_DIR}/../../.env.example` 填入），仅源仓库场景生效
  模板见 `resolve-skills/.env.example`。没配就匿名试、失败把错误**原样转述**给用户，不要猜测或硬塞认证信息（Wordfence 可能拦截应用密码认证，遇 401/403 如实报告）。可用 `node ${SKILL_DIR}/scripts/random-comment.mjs --env-check` 零副作用检查凭据是否就位。
- 不要修改/删除已提交的评论；评论内容只由用户要求或模型基于文章生成。
