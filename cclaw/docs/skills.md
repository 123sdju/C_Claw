# Skills Catalog

Skills 是 AgentSkills 风格的 `SKILL.md` 目录。SDK 负责解析、合并和生成 prompt
快照；文件监听和目录发现由 app 触发。

## 目录格式

每个 skill 是一个目录，目录名就是 skill id：

```text
.agents/skills/
  code-review/
    SKILL.md
  ultrasound/
    SKILL.md
```

`SKILL.md` 内容会原样注入到 `[Skills]` prompt 段中。SDK 不解释 Markdown 语义，
只负责读取文本和按 allowlist 过滤。

## 加载顺序

`cc_skill_catalog_load_from_config()` 按低优先级到高优先级加载：

1. `~/.cclaw/skills`
2. `agents.defaults.agentDir/skills`
3. `plugins.entries.*.skills`
4. `skills.load.extraDirs[]`
5. `workspace/.agents/skills`

同名 skill 后加载覆盖先加载。这样 workspace 可以覆盖用户级或 plugin 提供的默认
skill，适合项目内定制。

## Agent Allowlist

每个 agent 可以通过 `agents.defaults.skills` 或具名 agent 的 `skills` 声明 allowlist。
allowlist 为空时注入所有已加载 skill；非空时只注入同名 skill。

```json
{
  "agents": {
    "defaults": {
      "id": "default",
      "agentDir": ".agents/default",
      "skills": ["code-review"]
    }
  }
}
```

prompt snapshot 由 runtime builder 构建。reload 成功后新 run 使用新 prompt；
已经开始的 run 继续使用自己的 generation。

## Watcher 与裁剪

`CC_ENABLE_SKILLS` 控制 SDK catalog 是否编译。`CC_ENABLE_SKILL_WATCHER` 只控制
应用层是否编译 watcher。

watcher 不属于 SDK。应用可以用 polling、mtime、平台通知或其它机制检测
`config.json` 或 skill 文件变化，再调用 runtime builder reload。设备应用可以保留
静态 skill catalog，同时不编译 watcher。
