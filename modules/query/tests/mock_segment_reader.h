#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/tests/mock_segment_reader.h  —  ISegmentReader 测试辅助
//
// 内部使用 IndexWriter 在 /tmp/ 下建临时 Segment，析构时自动清理。
//
// 用法：
//   MockSegmentReader mock;
//   mock.addPosting("body", "python", {{1, 2}, {3, 1}, {7, 5}});
//   mock.addPosting("body", "numpy",  {{2, 1}, {7, 3}});
//   mock.build(/*total_docs=*/10);
//   ScorerContext ctx{mock, idfs, mock.docCount(), nullptr};
// ─────────────────────────────────────────────────────────────────────────────
#include "index/i_segment_reader.h"
#include "index/segment_reader.h"
#include "index/index_writer.h"
#include "field/schema.h"
#include "core/types.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <memory>

namespace ii {

class MockSegmentReader : public ISegmentReader {
public:
    MockSegmentReader() {
        tmpdir_ = "/tmp/mock_seg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        std::filesystem::remove_all(tmpdir_);
        std::filesystem::create_directories(tmpdir_);
    }

    ~MockSegmentReader() {
        reader_.reset();
        std::filesystem::remove_all(tmpdir_);
    }

    // 添加 posting：docs 为升序 {doc_id, tf}，在 build() 前调用
    void addPosting(const std::string& field, const std::string& term,
                    std::vector<std::pair<DocId, uint32_t>> docs) {
        specs_.push_back({field, term, std::move(docs)});
        fields_.insert(field);
    }

    // 写入索引并打开 SegmentReader；total_docs 为文档总数（决定 docCount()）
    void build(uint32_t total_docs = 0) {
        if (reader_) return;

        // 求最大 doc_id
        uint32_t max_doc = total_docs;
        for (const auto& s : specs_)
            for (const auto& [d, tf] : s.docs)
                max_doc = std::max(max_doc, d);

        // 构造 schema：每个字段为 Text + FreqsPositions + stored
        Schema schema;
        for (const auto& f : fields_)
            schema.fields.push_back({f, FieldType::Text, IndexOption::FreqsPositions, true});

        // 收集每个 doc 在每个字段的文本（按 tf 重复词）
        std::map<DocId, std::map<std::string, std::string>> doc_text;
        for (uint32_t i = 1; i <= max_doc; ++i)
            doc_text[i];  // 确保每个 doc_id 都存在

        for (const auto& s : specs_) {
            for (const auto& [doc_id, tf] : s.docs) {
                std::string& text = doc_text[doc_id][s.field];
                for (uint32_t t = 0; t < tf; ++t) {
                    if (!text.empty()) text += " ";
                    text += s.term;
                }
            }
        }

        // 写入
        IndexWriter writer(tmpdir_, /*ram_mb=*/16.f, schema);
        for (const auto& [doc_id, field_map] : doc_text) {
            Document doc;
            doc.ext_id = doc_id;
            for (const auto& f : fields_) {
                auto it = field_map.find(f);
                doc.set(f, it != field_map.end() ? it->second : "");
            }
            writer.addDocument(doc);
        }
        writer.flush();

        reader_ = std::make_unique<SegmentReader>(tmpdir_, 0);
    }

    // ISegmentReader interface

    std::unique_ptr<IPostingIterator> postingIterator(
        const std::string& field, const std::string& term) const override {
        ensureBuilt();
        return reader_->postingIterator(field, term);
    }

    const TermMeta* getTermMeta(const std::string& field,
                                const std::string& term) const override {
        ensureBuilt();
        return reader_->getTermMeta(field, term);
    }

    uint32_t fieldDocLen(const std::string& field, DocId doc_id) const override {
        ensureBuilt();
        return reader_->fieldDocLen(field, doc_id);
    }

    float fieldAvgDocLen(const std::string& field) const override {
        auto it = avg_overrides_.find(field);
        if (it != avg_overrides_.end()) return it->second;
        ensureBuilt();
        return reader_->fieldAvgDocLen(field);
    }

    uint32_t docCount()  const override { ensureBuilt(); return reader_->docCount(); }
    uint32_t segmentId() const override { return 0; }

    const std::vector<std::string>& indexedFieldNames() const override {
        ensureBuilt();
        return reader_->indexedFieldNames();
    }

    bool isAlive(DocId doc_id) const override {
        ensureBuilt();
        return reader_->isAlive(doc_id);
    }

    // Override avgdl for controlled BM25 scoring tests
    void setFieldAvgDocLen(const std::string& field, float avg) {
        avg_overrides_[field] = avg;
    }

    const SegmentReader& segmentReader() const { ensureBuilt(); return *reader_; }

private:
    void ensureBuilt() const { const_cast<MockSegmentReader*>(this)->build(); }

    struct Spec {
        std::string                             field;
        std::string                             term;
        std::vector<std::pair<DocId, uint32_t>> docs;
    };

    std::string                         tmpdir_;
    std::vector<Spec>                   specs_;
    std::set<std::string>               fields_;
    std::unique_ptr<SegmentReader>      reader_;
    std::map<std::string, float>        avg_overrides_;
};

} // namespace ii
