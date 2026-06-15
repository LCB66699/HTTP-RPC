FROM elasticsearch:8.15.0

# 安装 IK 中文分词插件 (版本与 ES 8.15.0 匹配)
RUN elasticsearch-plugin install --batch https://get.infini.cloud/elasticsearch/analysis-ik/8.15.0

# 安装拼音分词插件 (可选, 支持拼音搜索)
RUN elasticsearch-plugin install --batch https://get.infini.cloud/elasticsearch/analysis-pinyin/8.15.0
