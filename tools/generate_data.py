import os
import json
import random
import time
import zipfile
from datetime import datetime

# 1. 定义时间戳范围 (2025-05-16 至 2026-04-01)
start_date = datetime(2025, 5, 16, 0, 0, 0)
end_date = datetime(2026, 4, 1, 23, 59, 59)
start_ts = int(time.mktime(start_date.timetuple()))
end_ts = int(time.mktime(end_date.timetuple()))

# 2. 准备英文文本生成的基础词汇池
subjects = ["AI technology", "Global economy", "Quantum computing", "Renewable energy", "Autonomous driving", "Biotech breakthrough", "Cybersecurity", "Space exploration"]
verbs = ["revolutionizes", "impacts", "accelerates", "transforms", "shapes", "enhances", "redefines", "challenges"]
objects = ["the future of work", "modern society", "industrial manufacturing", "healthcare systems", "global communication", "environmental sustainability"]
adjectives = ["sustainable", "innovative", "unprecedented", "critical", "next-generation", "efficient", "automated"]
categories = ["Technology", "Finance", "Science", "Health", "Environment"]

def generate_mock_data():
    # 随机生成符合字数要求的Title (不超过20个单词)
    title_words = [random.choice(adjectives), random.choice(subjects), random.choice(verbs), random.choice(objects)]
    title = "How " + " ".join(title_words)
    
    # 随机生成Content (根据Title衍生，不超过800个单词)
    content_paragraphs = []
    for _ in range(random.randint(3, 5)):
        para = f"In recent developments concerning {title.lower()}, experts emphasize that {random.choice(subjects).lower()} is playing a {random.choice(adjectives)} role. " \
               f"This trend significantly {random.choice(verbs)} the landscape of {random.choice(objects)}. " \
               f"Furthermore, implementers are finding new ways to ensure it remains {random.choice(adjectives)} and productive over the long term. " \
               f"As we look ahead, the integration of these elements will likely create {random.choice(adjectives)} opportunities and mitigate existing risks in the market."
        content_paragraphs.append(para)
    content = "\n\n".join(content_paragraphs)
    
    # 随机生成Abstract (对Content的总结，不超过100个单词)
    abstract = f"This paper explores the relationship between {title_words[1].lower()} and {title_words[3]}, " \
               f"highlighting how recent advancements act as a {random.choice(adjectives)} driver for change in {random.choice(categories)}."
               
    # 随机生成Pubtime (uint64) 和 Uid (1-10000)
    pubtime = random.randint(start_ts, end_ts)
    uid = random.randint(1, 10000)
    category = random.choice(categories)
    
    return {
        "title": title,
        "content": content,
        "abstract": abstract,
        "pubtime": int(pubtime), # 对应整型/uint64
        "uid": int(uid),         # 对应整型
        "category": category
    }

def main():
    output_dir = "./generated_json_files"
    zip_filename = "json_files_1024.zip"
    
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    print("正在生成1024个JSON文件...")
    
    # 生成1024个文件
    for i in range(1, 1025):
        data = generate_mock_data()
        file_path = os.path.join(output_dir, f"file_{i:04d}.json")
        with open(file_path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
            
    print("文件生成完毕，正在打包压缩...")
    
    # 打包成压缩包
    with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for root, dirs, files in os.walk(output_dir):
            for file in files:
                file_path = os.path.join(root, file)
                zipf.write(file_path, os.path.relpath(file_path, output_dir))
                # 可选：打包后删除原文件以节省空间
                os.remove(file_path)
        os.rmdir(output_dir)
                
    print(f"成功！已生成压缩包：{os.path.abspath(zip_filename)}")

if __name__ == "__main__":
    main()
