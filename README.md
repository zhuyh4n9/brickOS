# TangramOS 设计文档仓库

设计文档正文在 [`docs/`](docs/README.md)(10 个分类, 全局编号 = 文档稳定身份)。

## 文档里的图怎么改

**图源与图片分离: 人只改 `.puml` 图源, 图片由脚本编译生成, 不要手改图片。**

```
docs/<分类>/<文档>.md                  ← 正文, 用 ![](pics/xxx.png) 引用图片
docs/<分类>/plantUML/<文档>-NN.puml    ← 图源(用 PlantUML 语法手改这个)
docs/<分类>/pics/<文档>-NN.png         ← 编译产物(markdown 引用, 不要手改)
```

```bash
cd docs
./render-plantuml.sh         # 增量编译: 只重编译内容变了的图
./render-plantuml.sh -w      # 挂着监听, 图源一存盘就自动重编译(推荐边改边看图)
./render-plantuml.sh -l      # 列出 图源 → 图片 映射、体积与新鲜度
./render-plantuml.sh -c      # 只查语法, 不写文件
./render-plantuml.sh -V      # 严格校验: 图片与图源是否逐字节一致(可当 CI 门禁)
./render-plantuml.sh -h      # 全部选项
```

改完图**把 `plantUML/` 和 `pics/` 一起提交** —— 只提交图源的话, 别人 clone 下来就没有图可看。

### 为什么图片也要提交进仓库

- PlantUML 输出是确定性的(同源 + 同版本 = 同字节), 所以 `./render-plantuml.sh -V`
  能长期当门禁: 谁改了图源却忘了重编译, CI 直接报出来并打印差异。
- 图片里内嵌了图源, 万一图源丢了还能反解回来: `plantuml -metadata xxx.png`。
- 读者不需要装 PlantUML/Graphviz 就能看文档。

### 新建一张图

1. 正文里写好 ```` ```plantuml ```` 代码块, 本地确认能出图;
2. 把代码块内容另存为 `plantUML/<文档名>-NN.puml`(编号顺着已有图往后排);
3. 正文里的代码块换成图片引用 + 源文件指针:

   ```markdown
   ![一句话说明](pics/<文档名>-NN.png)

   > 源文件: [plantUML/<文档名>-NN.puml](plantUML/<文档名>-NN.puml)
   ```

4. `./render-plantuml.sh` 生成 `pics/<文档名>-NN.png`。

### 依赖

- `plantuml` 命令(本机装在 `~/.local/share/plantuml/`, 见其 `README.md`);
  脚本找不到时会提示, 也可用 `PLANTUML=/path/to/plantuml` 指定。
- Graphviz(`dot`): 类图/组件图/状态图的布局引擎。**缺失时 PlantUML 不报错**,
  会静默退回内置布局, 出图与已提交的不一致 —— `./render-plantuml.sh -V` 会把这种情况报出来。

### 改图时注意

`plantUML/` 下每张图的文件头都写了该图的布局约束(例如 `00-architecture-01.puml` 里
记着"可见箭头不要连 L3 这类大 cluster"等 dot 布局的坑), 动图前先读一下, 能少走弯路。
