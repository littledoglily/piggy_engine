#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// field/field_descriptor.h  —  字段描述符类层级
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "field/schema.h"
#include "analysis/analyzer.h"
#include "field/fast_field_writer.h"
#include <memory>
#include <type_traits>
#include <unordered_map>

namespace ii {

class FieldDescriptor {
public:
    virtual ~FieldDescriptor() = default;

    virtual const std::string& name()        const = 0;
    virtual FieldType          type()        const = 0;
    virtual IndexOption        indexOption() const = 0;
    virtual bool               isStored()   const = 0;
    virtual bool               isFast()     const = 0;

    virtual uint32_t buildTokens(DocId doc_id, const FieldVal* val,
                                  uint32_t pos_base, Analyzer& analyzer,
                                  std::vector<Token>& out) const
    { return pos_base; }

    virtual uint32_t buildTokensDoc(
        DocId doc_id,
        const std::unordered_map<std::string, FieldVal>& fields,
        uint32_t pos_base, Analyzer& analyzer, std::vector<Token>& out) const
    {
        auto it = fields.find(name());
        const FieldVal* val = (it != fields.end()) ? &it->second : nullptr;
        return buildTokens(doc_id, val, pos_base, analyzer, out);
    }

    virtual void writeFast(const FieldVal* val, FastFieldWriter& ff) const {}
    virtual std::string storeStr(const FieldVal* val) const { return {}; }
};

template<typename T>
class FixedField : public FieldDescriptor {
    static_assert(std::is_same_v<T, int64_t> || std::is_same_v<T, float>);
public:
    FixedField(std::string name, IndexOption idx, bool stored, bool fast)
        : name_(std::move(name)), index_(idx), stored_(stored), fast_(fast) {}

    const std::string& name()        const override { return name_; }
    FieldType          type()        const override {
        if constexpr (std::is_same_v<T, int64_t>) return FieldType::Int64;
        else                                       return FieldType::Float32;
    }
    IndexOption indexOption() const override { return index_; }
    bool        isStored()   const override { return stored_; }
    bool        isFast()     const override { return fast_; }

    void writeFast(const FieldVal* val, FastFieldWriter& ff) const override {
        if (!fast_) return;
        T v{};
        if (val) { const T* p = std::get_if<T>(val); if (p) v = *p; }
        if constexpr (std::is_same_v<T, int64_t>) ff.addInt64  (name_, v);
        else                                       ff.addFloat32(name_, v);
    }
private:
    std::string name_;
    IndexOption index_;
    bool        stored_, fast_;
};

class VarSingleField : public FieldDescriptor {
public:
    VarSingleField(std::string name, FieldType type, IndexOption idx, bool stored)
        : name_(std::move(name)), type_(type), index_(idx), stored_(stored) {}

    const std::string& name()        const override { return name_; }
    FieldType          type()        const override { return type_; }
    IndexOption        indexOption() const override { return index_; }
    bool               isStored()   const override { return stored_; }
    bool               isFast()     const override { return false; }

    uint32_t buildTokens(DocId doc_id, const FieldVal* val,
                          uint32_t pos_base, Analyzer& analyzer,
                          std::vector<Token>& out) const override {
        if (index_ == IndexOption::None || !val) return pos_base;
        const std::string* s = std::get_if<std::string>(val);
        if (!s || s->empty()) return pos_base;
        if (type_ == FieldType::Text) {
            auto tokens = analyzer.analyze(doc_id, *s);
            uint32_t max_pos = pos_base;
            for (auto& tok : tokens) {
                tok.position += pos_base; tok.field = name_;
                max_pos = std::max(max_pos, tok.position);
                out.push_back(std::move(tok));
            }
            return max_pos + 1;
        } else {
            Token tok;
            tok.term = *s; tok.doc_id = doc_id; tok.position = pos_base;
            tok.start_off = 0; tok.end_off = (uint32_t)s->size(); tok.field = name_;
            out.push_back(std::move(tok));
            return pos_base + 1;
        }
    }
    std::string storeStr(const FieldVal* val) const override {
        if (!stored_ || !val) return {};
        const std::string* s = std::get_if<std::string>(val);
        return s ? *s : std::string{};
    }
private:
    std::string name_; FieldType type_; IndexOption index_; bool stored_;
};

class VarMultiField : public FieldDescriptor {
public:
    VarMultiField(std::string name, FieldType type, IndexOption idx, bool stored)
        : name_(std::move(name)), type_(type), index_(idx), stored_(stored) {}

    const std::string& name()        const override { return name_; }
    FieldType          type()        const override { return type_; }
    IndexOption        indexOption() const override { return index_; }
    bool               isStored()   const override { return stored_; }
    bool               isFast()     const override { return false; }

    uint32_t buildTokens(DocId doc_id, const FieldVal* val,
                          uint32_t pos_base, Analyzer& analyzer,
                          std::vector<Token>& out) const override {
        if (index_ == IndexOption::None || !val) return pos_base;
        const auto* vs = std::get_if<std::vector<std::string>>(val);
        if (!vs || vs->empty()) return pos_base;
        uint32_t cur = pos_base;
        for (const auto& s : *vs) {
            if (s.empty()) continue;
            if (type_ == FieldType::Text) {
                auto tokens = analyzer.analyze(doc_id, s);
                for (auto& tok : tokens) { tok.position = cur++; tok.field = name_; out.push_back(std::move(tok)); }
            } else {
                Token tok; tok.term = s; tok.doc_id = doc_id; tok.position = cur++;
                tok.start_off = 0; tok.end_off = (uint32_t)s.size(); tok.field = name_;
                out.push_back(std::move(tok));
            }
        }
        return cur;
    }
    std::string storeStr(const FieldVal* val) const override {
        if (!stored_ || !val) return {};
        const auto* vs = std::get_if<std::vector<std::string>>(val);
        if (!vs) return {};
        std::string r;
        for (size_t i = 0; i < vs->size(); ++i) { if (i) r += '\x1f'; r += (*vs)[i]; }
        return r;
    }
private:
    std::string name_; FieldType type_; IndexOption index_; bool stored_;
};

class CombinedField : public FieldDescriptor {
public:
    CombinedField(std::string name, std::vector<std::string> sources,
                  FieldType token_type, IndexOption idx)
        : name_(std::move(name)), sources_(std::move(sources))
        , token_type_(token_type), index_(idx) {}

    const std::string& name()        const override { return name_; }
    FieldType          type()        const override { return FieldType::Combined; }
    IndexOption        indexOption() const override { return index_; }
    bool               isStored()   const override { return false; }
    bool               isFast()     const override { return false; }

    uint32_t buildTokensDoc(DocId doc_id,
        const std::unordered_map<std::string, FieldVal>& fields,
        uint32_t pos_base, Analyzer& analyzer, std::vector<Token>& out) const override
    {
        if (index_ == IndexOption::None) return pos_base;
        uint32_t cur = pos_base;
        for (const auto& src : sources_) {
            auto it = fields.find(src);
            if (it == fields.end()) continue;
            const std::string* s = std::get_if<std::string>(&it->second);
            if (!s || s->empty()) continue;
            if (token_type_ == FieldType::Text) {
                auto tokens = analyzer.analyze(doc_id, *s);
                for (auto& tok : tokens) {
                    tok.position += cur; tok.field = name_;
                    cur = std::max(cur, tok.position) + 1;
                    out.push_back(std::move(tok));
                }
            } else {
                Token tok; tok.term = *s; tok.doc_id = doc_id; tok.position = cur++;
                tok.start_off = 0; tok.end_off = (uint32_t)s->size(); tok.field = name_;
                out.push_back(std::move(tok));
            }
        }
        return cur;
    }
private:
    std::string name_; std::vector<std::string> sources_;
    FieldType token_type_; IndexOption index_;
};

inline std::vector<std::unique_ptr<FieldDescriptor>>
buildDescriptors(const Schema& schema)
{
    std::vector<std::unique_ptr<FieldDescriptor>> result;
    result.reserve(schema.fields.size());
    for (const auto& fs : schema.fields) {
        switch (fs.type) {
            case FieldType::Int64:
                result.push_back(std::make_unique<FixedField<int64_t>>(fs.name, fs.index, fs.stored, fs.fast)); break;
            case FieldType::Float32:
                result.push_back(std::make_unique<FixedField<float>>(fs.name, fs.index, fs.stored, fs.fast)); break;
            case FieldType::Text: case FieldType::Keyword:
                if (fs.multi) result.push_back(std::make_unique<VarMultiField>(fs.name, fs.type, fs.index, fs.stored));
                else          result.push_back(std::make_unique<VarSingleField>(fs.name, fs.type, fs.index, fs.stored));
                break;
            case FieldType::Combined:
                result.push_back(std::make_unique<CombinedField>(fs.name, fs.sources, FieldType::Text, fs.index)); break;
        }
    }
    return result;
}

} // namespace ii
