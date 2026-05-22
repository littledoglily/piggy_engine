#include "store/pos_iterator.h"
#include <stdexcept>

namespace ii {

PosIterator::PosIterator(const std::string& pos_path,
                         uint64_t           pos_offset,
                         uint32_t           doc_freq)
    : remaining_(doc_freq)
{
    if (doc_freq == 0) return;
    file_.open(pos_path, std::ios::binary);
    if (!file_)
        throw std::runtime_error("PosIterator: cannot open " + pos_path);
    file_.seekg(static_cast<std::streamoff>(pos_offset));
    loadNext();
}

void PosIterator::loadNext() {
    if (remaining_ == 0) {
        preloaded_ = false;
        return;
    }
    --remaining_;
    file_.read(reinterpret_cast<char*>(&cur_doc_id_), 4);
    file_.read(reinterpret_cast<char*>(&cur_tf_),     4);
    cur_positions_.resize(cur_tf_);
    for (uint32_t i = 0; i < cur_tf_; ++i)
        file_.read(reinterpret_cast<char*>(&cur_positions_[i]), 4);
    preloaded_ = file_.good();
}

void PosIterator::next() {
    loadNext();
}

} // namespace ii
